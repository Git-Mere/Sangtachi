#include "hamychi/args.hpp"
#include "hamychi/network/wsa.hpp"

#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

// 로그 줄 하나에 이벤트 하나 (architecture.md 9장 로그 출력). 줄바꿈과 제어문자를
// 공백 하나로 바꿔 싣는다.
//
// 값을 따옴표로 감싼다. 사용자가 준 문자열에는 공백이 들어갈 수 있고, 감싸지 않으면
// `이름=값` 의 경계가 사라져 한 줄에 필드가 몇 개인지 읽을 수 없다. 9장은 이 필드의 값
// 형식을 정하지 않았다. 형식을 확정하는 것은 로그 모듈의 일이다 (plan.md 문서 부채).
//
// 이 함수는 임시다. 로그 모듈이 들어오면 그쪽으로 옮긴다.
std::string quote_for_log(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const char c : text) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
            out.push_back(' ');
        } else if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> raw;
    raw.reserve(argc > 0 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) {
        raw.emplace_back(argv[i]);
    }

    const auto parsed = hamychi::parse_args(std::span<const std::string_view>(raw));
    if (!parsed.ok()) {
        const std::string_view reason = hamychi::to_token(parsed.error);
        const std::string offending = quote_for_log(parsed.offending);
        std::fprintf(stderr, "ERROR args.invalid reason=%.*s arg=%s\n",
                     static_cast<int>(reason.size()), reason.data(), offending.c_str());
        // architecture.md 3.5: 인자 오류는 종료 코드 2 로 기동 실패다.
        return 2;
    }

    try {
        const hamychi::network::WsaContext wsa;
        std::fprintf(stderr, "INFO wsa.init version=%s\n",
                     wsa.negotiated_version().c_str());
    } catch (const hamychi::network::WsaStartupError& e) {
        std::fprintf(stderr, "ERROR wsa.init code=%d\n", e.code());
        return 2;
    }

    return 0;
}
