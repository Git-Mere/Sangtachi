#pragma once

// 최소 GUI 의 창 (architecture.md 3.1 의 ui/main_window, 3.5 GUI 실행 파일과 멤버 목록). `[gui]` 에서만 돈다.
//
// 창에 보일 것은 전부 ui/view 의 함수가 만든다. 여기서는 그 결과를 위젯에 옮기고, 버튼을 명령 줄로 바꿔 GuiUi 에
// 넘긴다. GUI 실행 파일만 컴파일한다. sangtachi_core 에 넣지 않는다.

#include "sangtachi/ui/gui_ui.hpp"
#include "sangtachi/ui/view.hpp"

#include <QTimer>
#include <QWidget>

#include <cstdint>
#include <string>

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace sangtachi::ui {

class MainWindow final : public QWidget {
public:
    explicit MainWindow(GuiUi& ui);

protected:
    void closeEvent(QCloseEvent* event) override;
    bool nativeEvent(const QByteArray& event_type, void* message, qintptr* result) override;

private:
    void poll();
    void render();
    void submit(const Submit& submit);
    // 끝 단계에 왔다. 대화 상자가 있으면 보인 뒤 Qt 루프를 끝낸다.
    void finish();

    GuiUi& ui_;
    ScreenState state_;
    bool closing_ = false;
    std::uint32_t shown_fail_seq_ = 0;
    std::string shown_name_;
    std::string note_;  // 상태 줄. 마지막 FAIL 줄이나 칸이 틀린 사유

    QTimer timer_;
    QLineEdit* name_ = nullptr;
    QPushButton* host_ = nullptr;
    QLineEdit* room_input_ = nullptr;
    QPushButton* join_ = nullptr;
    QPushButton* leave_ = nullptr;
    QLabel* room_ = nullptr;
    QPushButton* copy_ = nullptr;
    QLabel* virtual_ip_ = nullptr;
    QTableWidget* members_ = nullptr;
    QLabel* status_ = nullptr;
};

}  // namespace sangtachi::ui
