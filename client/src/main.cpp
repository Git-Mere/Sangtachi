#include "hamychi/args.hpp"
#include "hamychi/counters.hpp"
#include "hamychi/log.hpp"
#include "hamychi/network/wsa.hpp"

#include <span>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string_view> raw;
    raw.reserve(argc > 0 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) {
        raw.emplace_back(argv[i]);
    }

    const auto parsed = hamychi::parse_args(std::span<const std::string_view>(raw));
    if (!parsed.ok()) {
        const hamychi::LogField fields[] = {
            hamychi::field("reason", hamychi::to_token(parsed.error)),
            hamychi::field("arg", parsed.offending),
        };
        hamychi::emit(hamychi::LogLevel::Error, "args.invalid", fields);
        // architecture.md 3.5: 인자 오류는 종료 코드 2 로 기동 실패다.
        return 2;
    }

    hamychi::Counters counters;

    try {
        const hamychi::network::WsaContext wsa;
        const hamychi::LogField fields[] = {
            hamychi::field("version", wsa.negotiated_version()),
        };
        hamychi::emit(hamychi::LogLevel::Info, "wsa.init", fields);
    } catch (const hamychi::network::WsaStartupError& e) {
        const hamychi::LogField fields[] = {
            hamychi::field("op", std::string_view("WSAStartup")),
            hamychi::field("code", static_cast<std::uint64_t>(e.code())),
        };
        hamychi::emit(hamychi::LogLevel::Error, "socket.error", fields);
        return 2;
    }

    // 프로세스 종료 절차에서 카운터 전량을 낸다 (architecture.md 9장, 3.2.7 (4)).
    // 종료 절차 자체는 [loop] 가 들어올 때 붙는다.
    hamychi::emit_all(counters);
    return 0;
}
