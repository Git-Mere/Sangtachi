#include "hamychi/log.hpp"

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

namespace hamychi {

std::string_view to_token(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "ERROR";
}

std::string sanitize_value(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        // 0x20 은 공백이고 0x7F 는 DEL 이다. 둘 다 싣지 않는다.
        // 0x80 이상은 UTF-8 의 이어지는 바이트라 건드리지 않는다. 그것은 줄을 끊지도
        // 않고 이름과 값의 경계를 흐리지도 않는다.
        if (byte <= 0x20 || byte == 0x7F) {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

LogField field(std::string_view name, std::string_view value) {
    return LogField{name, sanitize_value(value)};
}

LogField field(std::string_view name, std::uint64_t value) {
    // 10진 정수에는 공백도 제어문자도 없다. 위생 처리를 거치지 않는다.
    return LogField{name, std::to_string(value)};
}

std::string format_line(LogLevel level, std::string_view event,
                        std::span<const LogField> fields) {
    std::string line;
    line.append(to_token(level));
    line.push_back(' ');
    line.append(event);
    for (const auto& f : fields) {
        line.push_back(' ');
        line.append(f.name);
        line.push_back('=');
        line.append(f.value);
    }
    return line;
}

void emit(LogLevel level, std::string_view event, std::span<const LogField> fields) {
    std::string line = format_line(level, event, fields);
    line.push_back('\n');
    // 한 번의 쓰기로 낸다. 두 번에 나누면 다른 스레드의 줄이 사이에 끼어들 수 있다.
    std::fwrite(line.data(), 1, line.size(), stderr);
}

}  // namespace hamychi
