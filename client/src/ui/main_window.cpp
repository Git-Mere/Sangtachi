#include "sangtachi/ui/main_window.hpp"

#include "sangtachi/platform/session_end.hpp"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <string_view>

namespace sangtachi::ui {

namespace {

QString to_qt(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

}  // namespace

MainWindow::MainWindow(GuiUi& ui) : ui_(ui) {
    setWindowTitle("Sangtachi");

    name_ = new QLineEdit(this);
    host_ = new QPushButton("Host", this);
    room_input_ = new QLineEdit(this);
    room_input_->setPlaceholderText("Room code");
    join_ = new QPushButton("Join", this);
    leave_ = new QPushButton("Leave", this);
    room_ = new QLabel(this);
    room_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    copy_ = new QPushButton("Copy", this);
    virtual_ip_ = new QLabel(this);
    members_ = new QTableWidget(0, 3, this);
    members_->setHorizontalHeaderLabels({"Name", "Virtual IP", "Status"});
    members_->verticalHeader()->hide();
    members_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    members_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    members_->setSelectionMode(QAbstractItemView::NoSelection);
    status_ = new QLabel(this);
    status_->setWordWrap(true);

    auto* grid = new QGridLayout;
    grid->addWidget(new QLabel("Name", this), 0, 0);
    grid->addWidget(name_, 0, 1);
    grid->addWidget(host_, 0, 2);
    grid->addWidget(new QLabel("Join room", this), 1, 0);
    grid->addWidget(room_input_, 1, 1);
    grid->addWidget(join_, 1, 2);
    auto* room_row = new QHBoxLayout;
    room_row->addWidget(room_, 1);
    room_row->addWidget(copy_);
    room_row->addWidget(leave_);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(grid);
    layout->addLayout(room_row);
    layout->addWidget(virtual_ip_);
    layout->addWidget(members_, 1);
    layout->addWidget(status_);

    connect(host_, &QPushButton::clicked, this,
            [this] { submit(host_submit(state_, name_->text().toStdString())); });
    connect(join_, &QPushButton::clicked, this, [this] {
        submit(join_submit(state_, name_->text().toStdString(), room_input_->text().toStdString()));
    });
    connect(leave_, &QPushButton::clicked, this, [this] { submit(Submit{{"leave"}, {}}); });
    connect(copy_, &QPushButton::clicked, this,
            [this] { QApplication::clipboard()->setText(to_qt(state_.room_id.view())); });

    // `[loop]` 는 `[gui]` 를 깨우지 않는다. GUI_POLL_MS 마다 화면 큐를 읽는다 (concurrency.md 6장).
    connect(&timer_, &QTimer::timeout, this, [this] { poll(); });
    timer_.start(static_cast<int>(kGuiPollMs));
    render();
}

void MainWindow::poll() {
    auto latest = ui_.take_latest();
    if (!latest) {
        return;
    }
    state_ = *latest;
    // 창을 닫은 뒤에는 작업 스레드의 항목이 아직 루프의 것이어도 `Closing...` 이다 (ui/view.hpp 머리 표).
    if (closing_ && (state_.phase == Phase::kLoading || state_.phase == Phase::kReady)) {
        state_.phase = Phase::kClosing;
    }
    render();
    if (state_.phase == Phase::kClosed || state_.phase == Phase::kStartupFailed ||
        state_.phase == Phase::kEndedByItself) {
        finish();
    }
}

void MainWindow::render() {
    const Controls c = controls(state_);
    if (state_.name.view() != shown_name_) {
        shown_name_ = std::string(state_.name.view());
        name_->setText(to_qt(shown_name_));
    }
    name_->setEnabled(c.name_edit);
    host_->setEnabled(c.host);
    room_input_->setEnabled(c.join);
    join_->setEnabled(c.join);
    leave_->setEnabled(c.leave);
    copy_->setEnabled(c.copy_room);
    room_->setText(state_.room_id.view().empty() ? QString() : "Room: " + to_qt(state_.room_id.view()));
    const std::string vip = own_virtual_ip_text(state_);
    virtual_ip_->setText(vip.empty() ? QString() : "Your virtual IP: " + to_qt(vip));

    const auto rows = member_rows(state_);
    members_->setRowCount(static_cast<int>(rows.size()));
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const MemberRow& row = rows[static_cast<std::size_t>(i)];
        members_->setItem(i, 0, new QTableWidgetItem(to_qt(row.name)));
        members_->setItem(i, 1, new QTableWidgetItem(to_qt(row.virtual_ip)));
        members_->setItem(i, 2, new QTableWidgetItem(to_qt(row.state)));
    }

    if (fail_is_new(shown_fail_seq_, state_)) {
        note_ = fail_text(state_).value_or(std::string());
        shown_fail_seq_ = state_.fail_seq;
    }
    const std::string_view phase = phase_text(state_);
    status_->setText(to_qt(phase.empty() ? std::string_view(note_) : phase));
}

void MainWindow::submit(const Submit& submit) {
    if (!submit.error.empty()) {
        note_ = std::string(submit.error);
        render();
        return;
    }
    note_.clear();
    render();
    for (const auto& line : submit.lines) {
        (void)ui_.send(line);
    }
}

void MainWindow::finish() {
    timer_.stop();
    if (const auto text = dialog_text(state_)) {
        QMessageBox::critical(this, "Sangtachi", to_qt(*text));
    }
    hide();
    QApplication::quit();
}

// 창 닫기는 `quit` 이다 (concurrency.md 7장 GUI 빌드의 종료). 바로 닫지 않고 정리가 끝난 항목이 오면 끝낸다.
void MainWindow::closeEvent(QCloseEvent* event) {
    event->ignore();
    if (closing_) {
        return;
    }
    closing_ = true;
    ui_.request_close();
    if (state_.phase == Phase::kLoading || state_.phase == Phase::kReady) {
        state_.phase = Phase::kClosing;
    }
    render();
}

// 로그오프와 시스템 종료. 이 메시지가 돌아가면 Windows 가 프로세스를 끝낼 수 있으므로 여기서 정리를 기다린다. 메시지는
// Qt 에도 그대로 넘긴다.
bool MainWindow::nativeEvent(const QByteArray& event_type, void* message, qintptr* result) {
    if (event_type == "windows_generic_MSG" && platform::is_session_ending(message)) {
        ui_.end_session();
    }
    return QWidget::nativeEvent(event_type, message, result);
}

}  // namespace sangtachi::ui
