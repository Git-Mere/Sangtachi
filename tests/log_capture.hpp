#pragma once

// 시험 안에서 로그 줄을 모은다 (log.hpp 의 LogSink).
//
// 만들 때 설치하고 사라질 때 앞의 것으로 되돌린다. 한 케이스 안에서만 쓴다. 다른 스레드가
// emit 하는 케이스에서는 쓰지 않는다 (log.hpp 의 설치 조건).

#include "sangtachi/log.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace sangtachi::testing {

class LogCapture final : public LogSink {
public:
    LogCapture() noexcept : previous_(set_log_sink(this)) {}
    ~LogCapture() override { (void)set_log_sink(previous_); }

    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
    LogCapture(LogCapture&&) = delete;
    LogCapture& operator=(LogCapture&&) = delete;

    void write(std::string_view line) override { lines.emplace_back(line); }

    // 이벤트 키가 event 인 줄. 줄 모양은 `<수준> <이벤트키> ...` 다 (log.hpp).
    [[nodiscard]] std::vector<std::string> with_event(std::string_view event) const {
        std::vector<std::string> out;
        for (const auto& line : lines) {
            const auto first = line.find(' ');
            if (first == std::string::npos) {
                continue;
            }
            const auto rest = std::string_view(line).substr(first + 1);
            const auto key = rest.substr(0, rest.find(' '));
            if (key == event) {
                out.push_back(line);
            }
        }
        return out;
    }

    std::vector<std::string> lines;

private:
    LogSink* previous_;
};

}  // namespace sangtachi::testing
