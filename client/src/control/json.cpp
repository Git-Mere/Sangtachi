#include "sangtachi/control/json.hpp"

#include "sangtachi/control/constants.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sangtachi::control::json {

// ---------------------------------------------------------------- Value

Value Value::null() {
    return Value{};
}

Value Value::boolean(bool value) {
    Value v;
    v.kind_ = Kind::kBool;
    v.bool_ = value;
    return v;
}

Value Value::integer(std::int64_t value) {
    Value v;
    v.kind_ = Kind::kInteger;
    v.integer_ = value;
    return v;
}

Value Value::other_number() {
    Value v;
    v.kind_ = Kind::kOtherNumber;
    return v;
}

Value Value::string(std::string value) {
    Value v;
    v.kind_ = Kind::kString;
    v.string_ = std::move(value);
    return v;
}

Value Value::array() {
    Value v;
    v.kind_ = Kind::kArray;
    return v;
}

Value Value::object() {
    Value v;
    v.kind_ = Kind::kObject;
    return v;
}

std::optional<bool> Value::as_bool() const noexcept {
    if (kind_ != Kind::kBool) {
        return std::nullopt;
    }
    return bool_;
}

std::optional<std::int64_t> Value::as_integer() const noexcept {
    if (kind_ != Kind::kInteger) {
        return std::nullopt;
    }
    return integer_;
}

const std::string* Value::as_string() const noexcept {
    return kind_ == Kind::kString ? &string_ : nullptr;
}

const std::vector<Value>* Value::as_array() const noexcept {
    return kind_ == Kind::kArray ? &items_ : nullptr;
}

const std::vector<Member>* Value::as_object() const noexcept {
    return kind_ == Kind::kObject ? &members_ : nullptr;
}

const Value* Value::find(std::string_view key) const noexcept {
    if (kind_ != Kind::kObject) {
        return nullptr;
    }
    for (const auto& m : members_) {
        if (m.key == key) {
            return &m.value;
        }
    }
    return nullptr;
}

bool Value::push_back(Value item) {
    if (kind_ != Kind::kArray) {
        return false;
    }
    items_.push_back(std::move(item));
    return true;
}

bool Value::insert(std::string key, Value item) {
    if (kind_ != Kind::kObject) {
        return false;
    }
    if (find(key) != nullptr) {
        return false;
    }
    members_.push_back(Member{std::move(key), std::move(item)});
    return true;
}

// ---------------------------------------------------------------- UTF-8

namespace {

constexpr bool is_continuation(unsigned char b) noexcept {
    return (b & 0xC0u) == 0x80u;
}

// text[pos] 에서 시작하는 UTF-8 한 글자의 길이. 잘못되었으면 0 이다.
//
// 허용 목록이다 (RFC 3629 4장의 표). 첫 바이트마다 둘째 바이트의 범위가 다르다. 그 범위가
// 과대 표현(C0, C1, E0 80..9F, F0 80..8F)과 서로게이트(ED A0..BF)와 U+10FFFF 초과(F4 90.., F5..)
// 를 막는다.
std::size_t utf8_length(std::string_view text, std::size_t pos) noexcept {
    const auto at = [&](std::size_t i) -> int {
        return (pos + i < text.size()) ? static_cast<unsigned char>(text[pos + i]) : -1;
    };
    const int b0 = at(0);
    if (b0 < 0) {
        return 0;
    }
    if (b0 < 0x80) {
        return 1;
    }
    unsigned char lo = 0x80;
    unsigned char hi = 0xBF;
    std::size_t length = 0;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        length = 2;
    } else if (b0 == 0xE0) {
        length = 3;
        lo = 0xA0;
    } else if ((b0 >= 0xE1 && b0 <= 0xEC) || b0 == 0xEE || b0 == 0xEF) {
        length = 3;
    } else if (b0 == 0xED) {
        length = 3;
        hi = 0x9F;  // U+D800 부터는 서로게이트다
    } else if (b0 == 0xF0) {
        length = 4;
        lo = 0x90;
    } else if (b0 >= 0xF1 && b0 <= 0xF3) {
        length = 4;
    } else if (b0 == 0xF4) {
        length = 4;
        hi = 0x8F;  // U+10FFFF 까지
    } else {
        return 0;
    }
    const int b1 = at(1);
    if (b1 < lo || b1 > hi) {
        return 0;
    }
    for (std::size_t i = 2; i < length; ++i) {
        const int b = at(i);
        if (b < 0 || !is_continuation(static_cast<unsigned char>(b))) {
            return 0;
        }
    }
    return length;
}

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

}  // namespace

bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t n = utf8_length(text, pos);
        if (n == 0) {
            return false;
        }
        pos += n;
    }
    return true;
}

// ---------------------------------------------------------------- 파서

std::string_view to_token(ParseError error) noexcept {
    switch (error) {
        case ParseError::kNone:             return "none";
        case ParseError::kTooLarge:         return "too_large";
        case ParseError::kTooDeep:          return "too_deep";
        case ParseError::kSyntax:           return "syntax";
        case ParseError::kControlCharacter: return "control_character";
        case ParseError::kBadEscape:        return "bad_escape";
        case ParseError::kBadSurrogate:     return "bad_surrogate";
        case ParseError::kBadUtf8:          return "bad_utf8";
        case ParseError::kDuplicateKey:     return "duplicate_key";
        case ParseError::kTrailingData:     return "trailing_data";
    }
    return "syntax";
}

namespace {

constexpr bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

class Parser {
public:
    explicit Parser(std::string_view text) noexcept : text_(text) {}

    ParseResult run() {
        skip_ws();
        Value value;
        if (!parse_value(value, 0)) {
            return {std::nullopt, error_};
        }
        skip_ws();
        if (pos_ != text_.size()) {
            return {std::nullopt, ParseError::kTrailingData};
        }
        return {std::move(value), ParseError::kNone};
    }

private:
    bool fail(ParseError e) noexcept {
        error_ = e;
        return false;
    }

    [[nodiscard]] bool at_end() const noexcept { return pos_ >= text_.size(); }
    [[nodiscard]] char peek() const noexcept { return text_[pos_]; }

    void skip_ws() noexcept {
        while (!at_end()) {
            const char c = peek();
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                return;
            }
            ++pos_;
        }
    }

    bool literal(std::string_view word) noexcept {
        if (text_.substr(pos_, word.size()) != word) {
            return fail(ParseError::kSyntax);
        }
        pos_ += word.size();
        return true;
    }

    // depth 는 이 값을 감싼 배열과 객체의 수다.
    bool parse_value(Value& out, std::size_t depth) {
        if (at_end()) {
            return fail(ParseError::kSyntax);
        }
        switch (peek()) {
            case '{': return parse_object(out, depth + 1);
            case '[': return parse_array(out, depth + 1);
            case '"': {
                std::string s;
                if (!parse_string(s)) {
                    return false;
                }
                out = Value::string(std::move(s));
                return true;
            }
            case 't':
                out = Value::boolean(true);
                return literal("true");
            case 'f':
                out = Value::boolean(false);
                return literal("false");
            case 'n':
                out = Value::null();
                return literal("null");
            default:
                return parse_number(out);
        }
    }

    bool parse_object(Value& out, std::size_t depth) {
        if (depth > kMaxDepth) {
            return fail(ParseError::kTooDeep);
        }
        ++pos_;  // '{'
        out = Value::object();
        skip_ws();
        if (!at_end() && peek() == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skip_ws();
            if (at_end() || peek() != '"') {
                return fail(ParseError::kSyntax);  // 키는 문자열이다. 끝 쉼표도 여기서 걸린다
            }
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            skip_ws();
            if (at_end() || peek() != ':') {
                return fail(ParseError::kSyntax);
            }
            ++pos_;
            skip_ws();
            Value item;
            if (!parse_value(item, depth)) {
                return false;
            }
            // 이스케이프를 푼 키로 비교한다. "a" 와 "\u0061" 은 같은 키다.
            if (!out.insert(std::move(key), std::move(item))) {
                return fail(ParseError::kDuplicateKey);
            }
            skip_ws();
            if (at_end()) {
                return fail(ParseError::kSyntax);
            }
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                return true;
            }
            return fail(ParseError::kSyntax);
        }
    }

    bool parse_array(Value& out, std::size_t depth) {
        if (depth > kMaxDepth) {
            return fail(ParseError::kTooDeep);
        }
        ++pos_;  // '['
        out = Value::array();
        skip_ws();
        if (!at_end() && peek() == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            skip_ws();
            Value item;
            if (!parse_value(item, depth)) {
                return false;  // 끝 쉼표 뒤의 ']' 는 값이 아니라서 여기서 걸린다
            }
            out.push_back(std::move(item));
            skip_ws();
            if (at_end()) {
                return fail(ParseError::kSyntax);
            }
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                return true;
            }
            return fail(ParseError::kSyntax);
        }
    }

    // \u 뒤의 16진 4자리.
    bool hex4(std::uint32_t& out) noexcept {
        if (text_.size() - pos_ < 4) {
            return fail(ParseError::kBadEscape);
        }
        std::uint32_t v = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const int h = hex_value(text_[pos_ + i]);
            if (h < 0) {
                return fail(ParseError::kBadEscape);
            }
            v = (v << 4) | static_cast<std::uint32_t>(h);
        }
        pos_ += 4;
        out = v;
        return true;
    }

    bool parse_escape(std::string& out) {
        if (at_end()) {
            return fail(ParseError::kSyntax);
        }
        const char c = peek();
        ++pos_;
        switch (c) {
            case '"':  out.push_back('"');  return true;
            case '\\': out.push_back('\\'); return true;
            case '/':  out.push_back('/');  return true;
            case 'b':  out.push_back('\b'); return true;
            case 'f':  out.push_back('\f'); return true;
            case 'n':  out.push_back('\n'); return true;
            case 'r':  out.push_back('\r'); return true;
            case 't':  out.push_back('\t'); return true;
            case 'u':  break;
            default:   return fail(ParseError::kBadEscape);
        }
        std::uint32_t cp = 0;
        if (!hex4(cp)) {
            return false;
        }
        if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return fail(ParseError::kBadSurrogate);  // 앞 짝 없는 하위 서로게이트
        }
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            // 상위 서로게이트. 바로 뒤에 \u 하위 서로게이트가 와야 한다.
            if (text_.substr(pos_, 2) != "\\u") {
                return fail(ParseError::kBadSurrogate);
            }
            pos_ += 2;
            std::uint32_t low = 0;
            if (!hex4(low)) {
                return false;
            }
            if (low < 0xDC00 || low > 0xDFFF) {
                return fail(ParseError::kBadSurrogate);
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        }
        append_utf8(out, cp);
        return true;
    }

    bool parse_string(std::string& out) {
        ++pos_;  // '"'
        while (true) {
            if (at_end()) {
                return fail(ParseError::kSyntax);  // 닫는 따옴표가 없다
            }
            const auto b = static_cast<unsigned char>(peek());
            if (b == '"') {
                ++pos_;
                return true;
            }
            if (b == '\\') {
                ++pos_;
                if (!parse_escape(out)) {
                    return false;
                }
                continue;
            }
            if (b < 0x20) {
                return fail(ParseError::kControlCharacter);
            }
            const std::size_t n = utf8_length(text_, pos_);
            if (n == 0) {
                return fail(ParseError::kBadUtf8);
            }
            out.append(text_.substr(pos_, n));
            pos_ += n;
        }
    }

    // RFC 8259 6 의 문법 전체를 본다. 정수이고 int64 에 들어가면 값을 담는다.
    bool parse_number(Value& out) noexcept {
        const bool negative = (peek() == '-');
        if (negative) {
            ++pos_;
        }
        if (at_end() || !is_digit(peek())) {
            return fail(ParseError::kSyntax);  // '+1', '-', '.5', 모르는 낱말
        }

        // 크기를 uint64 로 센다. 음수는 2^63 까지 담을 수 있다.
        const std::uint64_t limit = negative ? (std::uint64_t{1} << 63)
                                             : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        std::uint64_t magnitude = 0;
        bool fits = true;
        if (peek() == '0') {
            ++pos_;
            if (!at_end() && is_digit(peek())) {
                return fail(ParseError::kSyntax);  // 앞의 0 은 한 자리만 (RFC 8259 6)
            }
        } else {
            while (!at_end() && is_digit(peek())) {
                const auto d = static_cast<std::uint64_t>(peek() - '0');
                if (fits && magnitude > (limit - d) / 10) {
                    fits = false;
                }
                if (fits) {
                    magnitude = magnitude * 10 + d;
                }
                ++pos_;
            }
        }

        bool integral = true;
        if (!at_end() && peek() == '.') {
            integral = false;
            ++pos_;
            if (at_end() || !is_digit(peek())) {
                return fail(ParseError::kSyntax);  // '1.'
            }
            while (!at_end() && is_digit(peek())) {
                ++pos_;
            }
        }
        if (!at_end() && (peek() == 'e' || peek() == 'E')) {
            integral = false;
            ++pos_;
            if (!at_end() && (peek() == '+' || peek() == '-')) {
                ++pos_;
            }
            if (at_end() || !is_digit(peek())) {
                return fail(ParseError::kSyntax);  // '1e', '1e+'
            }
            while (!at_end() && is_digit(peek())) {
                ++pos_;
            }
        }

        if (!integral || !fits) {
            out = Value::other_number();
            return true;
        }
        if (negative) {
            // magnitude <= 2^63. 2^63 이면 INT64_MIN 이다.
            out = Value::integer(magnitude == (std::uint64_t{1} << 63)
                                     ? std::numeric_limits<std::int64_t>::min()
                                     : -static_cast<std::int64_t>(magnitude));
        } else {
            out = Value::integer(static_cast<std::int64_t>(magnitude));
        }
        return true;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    ParseError error_ = ParseError::kSyntax;
};

}  // namespace

ParseResult parse(std::string_view text) {
    if (text.size() > kMaxBodyBytes) {
        return {std::nullopt, ParseError::kTooLarge};
    }
    return Parser(text).run();
}

// ---------------------------------------------------------------- 직렬화

namespace {

bool write_string(std::string& out, std::string_view s) {
    if (!is_valid_utf8(s)) {
        return false;
    }
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    for (const char c : s) {
        const auto b = static_cast<unsigned char>(c);
        switch (c) {
            case '"':  out.append("\\\""); continue;
            case '\\': out.append("\\\\"); continue;
            case '\b': out.append("\\b");  continue;
            case '\f': out.append("\\f");  continue;
            case '\n': out.append("\\n");  continue;
            case '\r': out.append("\\r");  continue;
            case '\t': out.append("\\t");  continue;
            default:   break;
        }
        if (b < 0x20) {
            out.append("\\u00");
            out.push_back(kHex[b >> 4]);
            out.push_back(kHex[b & 0x0F]);
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return true;
}

bool write_value(std::string& out, const Value& v, std::size_t depth) {
    switch (v.kind()) {
        case Kind::kNull:
            out.append("null");
            return true;
        case Kind::kBool:
            out.append(*v.as_bool() ? "true" : "false");
            return true;
        case Kind::kInteger:
            out.append(std::to_string(*v.as_integer()));
            return true;
        case Kind::kOtherNumber:
            return false;  // 값을 들고 있지 않다
        case Kind::kString:
            return write_string(out, *v.as_string());
        case Kind::kArray: {
            if (depth + 1 > kMaxDepth) {
                return false;
            }
            out.push_back('[');
            bool first = true;
            for (const auto& item : *v.as_array()) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                if (!write_value(out, item, depth + 1)) {
                    return false;
                }
            }
            out.push_back(']');
            return true;
        }
        case Kind::kObject: {
            if (depth + 1 > kMaxDepth) {
                return false;
            }
            out.push_back('{');
            bool first = true;
            for (const auto& m : *v.as_object()) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                if (!write_string(out, m.key)) {
                    return false;
                }
                out.push_back(':');
                if (!write_value(out, m.value, depth + 1)) {
                    return false;
                }
            }
            out.push_back('}');
            return true;
        }
    }
    return false;
}

}  // namespace

std::optional<std::string> serialize(const Value& value) {
    std::string out;
    if (!write_value(out, value, 0)) {
        return std::nullopt;
    }
    return out;
}

}  // namespace sangtachi::control::json
