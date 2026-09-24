#include "sangtachi/args.hpp"

#include "sangtachi/network/endpoint.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace sangtachi {
namespace {

using network::Endpoint;
using network::parse_ipv4;
using network::parse_port;

constexpr bool is_ascii_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

constexpr bool is_ascii_lower_hex(char c) noexcept {
    return is_ascii_digit(c) || (c >= 'a' && c <= 'f');
}

constexpr bool is_ascii_letter(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// control_plane.md 2.1 room_id 의 알파벳. I, O, 0, 1 이 없다.
constexpr std::string_view kRoomAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
constexpr std::size_t kRoomLength = 6;
constexpr std::size_t kPeerTokenLength = 32;

// ASCII 소문자만 대문자로 바꾼다. ASCII 밖 바이트는 그대로 두고, 그러면 아래 알파벳
// 검사에서 걸린다. 로캘에 기대는 변환을 쓰지 않는다.
std::string to_ascii_upper(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

// control_plane.md 2.1 room_id 의 정규화와 검사.
std::optional<std::string> normalize_room(std::string_view text) {
    if (text.size() != kRoomLength) {
        return std::nullopt;
    }
    std::string upper = to_ascii_upper(text);
    for (const char c : upper) {
        if (kRoomAlphabet.find(c) == std::string_view::npos) {
            return std::nullopt;
        }
    }
    return upper;
}

// 호스트. IPv4 리터럴이거나 이름이다.
//
// 이름 규칙은 라벨마다 1~63자, 전체 253자 이하, 글자·숫자·붙임표만이다. 거기에 더해
// **마지막 라벨이 전부 숫자면 거부한다.** 그런 값은 리터럴로도 실패하고 이름으로도 풀 수
// 없다. 이 검사가 없으면 999.999.999.999 가 이름으로 통과해 Phase 3 의 해석 실패로
// 미뤄진다.
bool is_valid_host(std::string_view text) {
    if (text.empty() || text.size() > 253) {
        return false;
    }
    if (parse_ipv4(text)) {
        return true;  // 리터럴이면 아래 이름 규칙을 보지 않는다
    }

    // 마지막 라벨. 점이 없으면 문자열 전체다.
    const std::size_t last_dot = text.rfind('.');
    const std::string_view last_label =
        (last_dot == std::string_view::npos) ? text : text.substr(last_dot + 1);
    if (!last_label.empty()) {
        bool all_digits = true;
        for (const char c : last_label) {
            if (!is_ascii_digit(c)) {
                all_digits = false;
                break;
            }
        }
        if (all_digits) {
            return false;
        }
    }
    std::size_t label_len = 0;
    char previous = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '.') {
            if (label_len == 0 || previous == '-') {
                return false;  // 빈 라벨이거나 붙임표로 끝났다
            }
            label_len = 0;
            previous = c;
            continue;
        }
        if (!is_ascii_letter(c) && !is_ascii_digit(c) && c != '-') {
            return false;
        }
        if (label_len == 0 && c == '-') {
            return false;  // 라벨이 붙임표로 시작했다
        }
        ++label_len;
        if (label_len > 63) {
            return false;
        }
        previous = c;
    }
    return label_len != 0 && previous != '-';
}

struct HostPortParse {
    std::optional<HostPort> value;
    ArgError error = ArgError::None;
};

// "<호스트>[:<포트>]" 를 읽는다. 포트가 없으면 default_port 를 쓰고, require_port 가
// 참이면 포트 없는 입력을 거부한다.
HostPortParse parse_host_port(std::string_view text, std::uint16_t default_port,
                              bool require_port) {
    const std::size_t colon = text.find(':');
    const std::string_view host = text.substr(0, colon);

    if (!is_valid_host(host)) {
        return {std::nullopt, ArgError::BadHost};
    }
    if (colon == std::string_view::npos) {
        if (require_port) {
            return {std::nullopt, ArgError::MissingPort};
        }
        return {HostPort{std::string(host), default_port}, ArgError::None};
    }

    const auto port = parse_port(text.substr(colon + 1));
    if (!port || *port == 0) {
        return {std::nullopt, ArgError::BadPort};  // 인자의 포트는 1~65535 다
    }
    return {HostPort{std::string(host), *port}, ArgError::None};
}

// "<peer_id>:<peer_token>".
//
// 콜론 개수를 따로 세지 않는다. 둘째 콜론은 토큰 쪽에 들어가고 16진수 검사가 거부한다.
std::optional<Rejoin> parse_rejoin(std::string_view text) {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view id_text = text.substr(0, colon);
    const std::string_view token = text.substr(colon + 1);

    // control_plane.md 2.2 peer_id. 32비트, 0 제외. 10진수로 적는다.
    if (id_text.empty() || id_text.size() > 10) {
        return std::nullopt;
    }
    std::uint64_t id = 0;
    for (const char c : id_text) {
        if (!is_ascii_digit(c)) {
            return std::nullopt;
        }
        id = id * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (id == 0 || id > 0xFFFFFFFFull) {
        return std::nullopt;
    }

    // control_plane.md 2.3 peer_token. 소문자 16진수 32자.
    if (token.size() != kPeerTokenLength) {
        return std::nullopt;
    }
    for (const char c : token) {
        if (!is_ascii_lower_hex(c)) {
            return std::nullopt;
        }
    }

    return Rejoin{static_cast<std::uint32_t>(id), std::string(token)};
}

ParseResult fail(ArgError error, std::string_view offending) {
    ParseResult result;
    result.error = error;
    result.offending = std::string(offending);
    return result;
}

}  // namespace

std::string_view to_token(ArgError error) noexcept {
    switch (error) {
        case ArgError::None:            return "none";
        case ArgError::UnknownOption:   return "unknown_option";
        case ArgError::MissingValue:    return "missing_value";
        case ArgError::BadRole:         return "bad_role";
        case ArgError::ExtraPositional: return "extra_positional";
        case ArgError::BadRoom:         return "bad_room";
        case ArgError::BadRejoin:       return "bad_rejoin";
        case ArgError::BadHost:         return "bad_host";
        case ArgError::BadPort:         return "bad_port";
        case ArgError::MissingPort:     return "missing_port";
    }
    return "unknown";
}

ParseResult parse_args(std::span<const std::string_view> argv) {
    Args args;
    bool saw_positional = false;

    for (std::size_t i = 0; i < argv.size(); ++i) {
        const std::string_view token = argv[i];

        if (token.rfind("--", 0) != 0) {
            if (saw_positional) {
                return fail(ArgError::ExtraPositional, token);
            }
            if (token == "host") {
                args.role = Role::Host;
            } else if (token == "player") {
                args.role = Role::Player;
            } else {
                return fail(ArgError::BadRole, token);
            }
            saw_positional = true;
            continue;
        }

        const bool takes_value =
            token == "--server" || token == "--room" || token == "--rejoin" ||
            token == "--stun" || token == "--peer";
        if (!takes_value) {
            return fail(ArgError::UnknownOption, token);
        }
        if (i + 1 >= argv.size()) {
            return fail(ArgError::MissingValue, token);
        }

        const std::string_view value = argv[++i];

        if (token == "--server") {
            auto parsed = parse_host_port(value, kControlPort, false);
            if (!parsed.value) {
                return fail(parsed.error, value);
            }
            args.server = std::move(*parsed.value);
        } else if (token == "--stun") {
            auto parsed = parse_host_port(value, 0, true);
            if (!parsed.value) {
                return fail(parsed.error, value);
            }
            args.stun.push_back(std::move(*parsed.value));
        } else if (token == "--room") {
            auto room = normalize_room(value);
            if (!room) {
                return fail(ArgError::BadRoom, value);
            }
            args.room = std::move(*room);
        } else if (token == "--rejoin") {
            auto rejoin = parse_rejoin(value);
            if (!rejoin) {
                return fail(ArgError::BadRejoin, value);
            }
            args.rejoin = std::move(*rejoin);
        } else {  // --peer
            // Endpoint::parse 를 쓰지 않는다. 그것은 성공과 실패만 돌려주므로 주소가
            // 틀린 것인지 포트가 틀린 것인지를 오류 코드로 가를 수 없다.
            const std::size_t colon = value.find(':');
            if (colon == std::string_view::npos) {
                return fail(ArgError::MissingPort, value);
            }
            const auto address = parse_ipv4(value.substr(0, colon));
            if (!address) {
                return fail(ArgError::BadHost, value);
            }
            const auto port = parse_port(value.substr(colon + 1));
            if (!port || *port == 0) {
                return fail(ArgError::BadPort, value);
            }
            args.peer = Endpoint(*address, *port);
        }
    }

    ParseResult result;
    result.args = std::move(args);
    return result;
}

}  // namespace sangtachi
