#pragma once

// 제어 평면 본문의 최소 JSON (control_plane.md 3.3 HTTP 부분집합의 본문·인코딩 행).
//
// spec.md NFR-5 때문에 서드파티 JSON 라이브러리를 쓰지 않는다. 연산 다섯의 요청과 응답에
// 필요한 만큼만 둔다. **순수 코드다.** 입출력도 로그도 없다.
//
// ## 판정 자세
//
// 바깥(제어 서버)에서 온 바이트를 판정하는 코드다. RFC 8259 문법을 엄격히 따르고, 문법이
// 허용하지 않는 것은 받지 않는다. 너그럽게 고쳐 읽지 않는다.
//
// | 자리 | 정한 것 | 출처 또는 이유 |
// |------|---------|----------------|
// | 입력 크기 | `kMaxBodyBytes`(4096) 를 넘으면 거부 | control_plane.md 2.6 MAX_BODY_BYTES |
// | 중첩 깊이 | 배열과 객체를 합쳐 `kMaxDepth`(`CLIENT_JSON_MAX_DEPTH`, 32) 단계를 넘으면 거부 | control_plane.md 3.5 의 5, 2.6 상수 |
// | 중복 키 | 한 객체 안에서 같은 키(이스케이프를 푼 뒤 바이트 비교)면 중첩 객체까지 거부 | 3.3 본문 행, 3.5 의 5. 어느 쪽 값을 믿을지 고르지 않는다 |
// | `NaN`, `Infinity`, 주석, 끝 쉼표, 작은따옴표 | 거부 | JSON 이 아니다 (3.3) |
// | BOM | 거부 | RFC 8259 8.1 은 보내지 말라고 한다. 서버는 보내지 않는다 |
// | 문자열 안의 제어 바이트(0x00~0x1F) | 거부. `\u0000` 같은 이스케이프는 받는다 | RFC 8259 7 |
// | 문자열 안의 원시 바이트 0x80 이상 | 엄격한 UTF-8 이어야 한다. 과대 표현, 서로게이트(U+D800~DFFF), U+10FFFF 초과는 거부 | 3.3 "문자열은 UTF-8" |
// | `\uXXXX` | 16진 4자리. 상위 서로게이트 뒤에 하위 서로게이트 `\u` 가 와야 하고 둘을 한 코드 포인트로 묶어 UTF-8 로 적는다. 짝 없는 서로게이트는 거부 | RFC 8259 7 |
// | 정수 | `-?(0|[1-9][0-9]*)` 이고 int64 에 들어가면 `kInteger` | 3.3 "정수는 JSON number 이고 소수점 없음" |
// | 소수점이나 지수가 있는 수, int64 밖의 정수 | 문법이 맞으면 **받되** `kOtherNumber` 로 둔다. 값은 담지 않는다 | 아래 "다른 수를 받는 이유" |
// | `-0` | 정수 0 | 문법상 정수이고 Python json 도 0 으로 읽는다 |
// | 공백 | 0x20, 0x09, 0x0A, 0x0D 만 | RFC 8259 2 |
// | 값 뒤의 나머지 | 공백이 아닌 것이 남으면 거부 | 본문이 값 하나여야 한다 (3.3) |
//
// **다른 수를 받는 이유.** 응답의 알 수 없는 키는 무시한다(3.3 전방 호환). 서버가 나중에
// 소수 값을 가진 키를 더해도 본문 전체가 거부되면 안 된다. 그래서 파서는 문법만 보고, 정수
// 필드를 읽는 쪽(ops.hpp 의 해석기)이 `kInteger` 가 아니면 형 위반으로 거부한다. `1.0` 은
// 정수 필드에서 거부된다. 3.3 이 "소수점 없음" 이라고 적었기 때문이다.
//
// 오류는 이유만 돌려주고 입력 내용을 싣지 않는다. 본문에는 `peer_token` 이 들어 있다
// (architecture.md 3.5 기동 입력).

#include "sangtachi/control/constants.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sangtachi::control::json {

inline constexpr std::size_t kMaxDepth = kClientJsonMaxDepth;

enum class Kind : std::uint8_t {
    kNull,
    kBool,
    kInteger,      // int64 에 들어가는 정수. 소수점도 지수도 없다
    kOtherNumber,  // 문법은 맞는 그 밖의 수. 값을 담지 않는다
    kString,       // UTF-8. 이스케이프를 푼 값
    kArray,
    kObject,       // 키의 순서를 지킨다. 직렬화가 그 순서로 적는다
};

struct Member;

class Value {
public:
    Value() = default;  // null

    [[nodiscard]] static Value null();
    [[nodiscard]] static Value boolean(bool value);
    [[nodiscard]] static Value integer(std::int64_t value);
    [[nodiscard]] static Value other_number();
    [[nodiscard]] static Value string(std::string value);
    [[nodiscard]] static Value array();
    [[nodiscard]] static Value object();

    [[nodiscard]] Kind kind() const noexcept { return kind_; }

    // 형이 맞을 때만 값이 있다. 불은 정수가 아니고 정수는 불이 아니다.
    [[nodiscard]] std::optional<bool> as_bool() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> as_integer() const noexcept;
    [[nodiscard]] const std::string* as_string() const noexcept;
    [[nodiscard]] const std::vector<Value>* as_array() const noexcept;
    [[nodiscard]] const std::vector<Member>* as_object() const noexcept;

    // 객체에서 키 하나를 찾는다. 객체가 아니거나 없으면 nullptr 이다.
    [[nodiscard]] const Value* find(std::string_view key) const noexcept;

    // 배열 끝에 붙인다. 배열이 아니면 거짓이다.
    bool push_back(Value item);

    // 객체에 키를 더한다. 객체가 아니거나 이미 있는 키면 거짓이고 아무것도 바꾸지 않는다.
    bool insert(std::string key, Value item);

private:
    Kind kind_ = Kind::kNull;
    bool bool_ = false;
    std::int64_t integer_ = 0;
    std::string string_;
    std::vector<Value> items_;
    std::vector<Member> members_;
};

struct Member {
    std::string key;
    Value value;
};

// 파싱 실패의 이유. 로그와 시험이 쓰는 고정 토큰이 있다 (to_token).
enum class ParseError : std::uint8_t {
    kNone = 0,
    kTooLarge,          // kMaxBodyBytes 초과
    kTooDeep,           // kMaxDepth 초과
    kSyntax,            // 문법 위반. 빈 입력, BOM, 끝 쉼표, 모르는 낱말, 잘린 입력을 포함한다
    kControlCharacter,  // 문자열 안의 원시 제어 바이트
    kBadEscape,         // 모르는 이스케이프, 16진 4자리가 아닌 \u
    kBadSurrogate,      // 짝 없는 서로게이트
    kBadUtf8,           // 문자열 안의 잘못된 UTF-8
    kDuplicateKey,      // 한 객체 안의 같은 키
    kTrailingData,      // 값 뒤에 공백이 아닌 것이 남았다
};

[[nodiscard]] std::string_view to_token(ParseError error) noexcept;

struct ParseResult {
    std::optional<Value> value;  // 성공일 때만 있다
    ParseError error = ParseError::kNone;
};

// 값 하나를 읽는다. 최상위 값의 형은 보지 않는다. 객체여야 한다는 판정은 http 쪽이 한다.
[[nodiscard]] ParseResult parse(std::string_view text);

// 바이트열이 엄격한 UTF-8 인가. 파서와 직렬화가 같은 규칙을 쓴다.
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

// 값을 공백 없는 JSON 텍스트로 적는다. 객체는 넣은 순서대로 적는다.
//
// 적을 수 없으면 nullopt 다. `kOtherNumber`(값을 들고 있지 않다), 잘못된 UTF-8 문자열,
// kMaxDepth 를 넘는 중첩이 그렇다. 문자열의 `"`, `\`, 제어 바이트는 이스케이프하고 0x80
// 이상은 UTF-8 그대로 적는다.
[[nodiscard]] std::optional<std::string> serialize(const Value& value);

}  // namespace sangtachi::control::json
