#include "sangtachi/control/json.hpp"

#include "sangtachi/control/constants.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// 규칙의 출처는 client/include/sangtachi/control/json.hpp 머리의 표다. 이 표가 그 규칙을
// 행으로 편다. 행마다 기대하는 오류 이유가 있어서, 규칙 하나를 지운 구현은 그 이유를 기대한
// 행에서 떨어진다.

using sangtachi::control::kMaxBodyBytes;
using sangtachi::control::json::Kind;
using sangtachi::control::json::kMaxDepth;
using sangtachi::control::json::parse;
using sangtachi::control::json::ParseError;
using sangtachi::control::json::serialize;
using sangtachi::control::json::to_token;
using sangtachi::control::json::Value;

namespace {

struct Row {
    std::string_view text;
    ParseError error;  // kNone 이면 받는다
    Kind kind;         // 받을 때 최상위 값의 형
    std::string_view why;
};

constexpr auto N = ParseError::kNone;

// 원시 바이트는 "\x.." 로 적고, 뒤 글자가 16진으로 읽히지 않게 문자열을 끊는다.
const Row kRows[] = {
    // 받는다
    {"{}",                                   N, Kind::kObject, "empty object"},
    {"[]",                                   N, Kind::kArray,  "empty array; top-level kind is http's job"},
    {" \t\r\n{\"a\":1} \t\r\n",              N, Kind::kObject, "the four whitespace bytes around"},
    {"{\"a\":[1,2,{\"b\":null}]}",           N, Kind::kObject, "nesting"},
    {"true",                                 N, Kind::kBool,   "literal"},
    {"false",                                N, Kind::kBool,   "literal"},
    {"null",                                 N, Kind::kNull,   "literal"},
    {"0",                                    N, Kind::kInteger, "zero"},
    {"-0",                                   N, Kind::kInteger, "negative zero is integer 0"},
    {"9223372036854775807",                  N, Kind::kInteger, "int64 max"},
    {"-9223372036854775808",                 N, Kind::kInteger, "int64 min"},
    {"9223372036854775808",                  N, Kind::kOtherNumber, "int64 max + 1 is a number, not an integer"},
    {"-9223372036854775809",                 N, Kind::kOtherNumber, "int64 min - 1"},
    {"1.5",                                  N, Kind::kOtherNumber, "fraction is legal json for unknown keys"},
    {"1.0",                                  N, Kind::kOtherNumber, "3.3: integers have no decimal point"},
    {"1e3",                                  N, Kind::kOtherNumber, "exponent"},
    {"-1E-2",                                N, Kind::kOtherNumber, "signed exponent"},
    {"0.0e+0",                               N, Kind::kOtherNumber, "all parts"},
    {"\"x\"",                                N, Kind::kString, "string"},
    {"\"\\u00e9\"",                          N, Kind::kString, "escape to two-byte utf-8"},
    {"\"\\uD83D\\uDE00\"",                   N, Kind::kString, "surrogate pair, upper-case hex"},
    {"\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"",       N, Kind::kString, "the eight short escapes"},
    {"\"\\u0000\"",                          N, Kind::kString, "escaped NUL is legal"},
    {"\"\xC3\xA9\"",                         N, Kind::kString, "raw two-byte utf-8"},
    {"\"\xE2\x82\xAC\"",                     N, Kind::kString, "raw three-byte utf-8"},
    {"\"\xF0\x9F\x98\x80\"",                 N, Kind::kString, "raw four-byte utf-8"},
    {"\"\xED\x9F\xBF\"",                     N, Kind::kString, "U+D7FF, last before surrogates"},
    {"\"\xF4\x8F\xBF\xBF\"",                 N, Kind::kString, "U+10FFFF, highest"},
    {"\"\x7F\"",                             N, Kind::kString, "DEL is not a control byte in json"},
    {"{\"a\":1,\"b\":{\"a\":2}}",            N, Kind::kObject, "same key in different objects"},

    // 문법
    {"",                                     ParseError::kSyntax, Kind::kNull, "empty"},
    {" ",                                    ParseError::kSyntax, Kind::kNull, "whitespace only"},
    {"\xEF\xBB\xBF{}",                       ParseError::kSyntax, Kind::kNull, "BOM"},
    {"{\"a\":1,}",                           ParseError::kSyntax, Kind::kNull, "trailing comma in object"},
    {"[1,]",                                 ParseError::kSyntax, Kind::kNull, "trailing comma in array"},
    {"{\"a\" 1}",                            ParseError::kSyntax, Kind::kNull, "missing colon"},
    {"{\"a\":1 \"b\":2}",                    ParseError::kSyntax, Kind::kNull, "missing comma"},
    {"{a:1}",                                ParseError::kSyntax, Kind::kNull, "bare key"},
    {"{'a':1}",                              ParseError::kSyntax, Kind::kNull, "single quotes"},
    {"NaN",                                  ParseError::kSyntax, Kind::kNull, "3.3: NaN is not json"},
    {"Infinity",                             ParseError::kSyntax, Kind::kNull, "3.3: Infinity is not json"},
    {"-Infinity",                            ParseError::kSyntax, Kind::kNull, "negative infinity"},
    {"01",                                   ParseError::kSyntax, Kind::kNull, "leading zero"},
    {"-01",                                  ParseError::kSyntax, Kind::kNull, "leading zero after sign"},
    {"-",                                    ParseError::kSyntax, Kind::kNull, "sign only"},
    {"+1",                                   ParseError::kSyntax, Kind::kNull, "plus sign"},
    {".5",                                   ParseError::kSyntax, Kind::kNull, "no integer part"},
    {"1.",                                   ParseError::kSyntax, Kind::kNull, "no fraction digits"},
    {"1e",                                   ParseError::kSyntax, Kind::kNull, "no exponent digits"},
    {"1e+",                                  ParseError::kSyntax, Kind::kNull, "no exponent digits after sign"},
    {"tru",                                  ParseError::kSyntax, Kind::kNull, "truncated literal"},
    {"True",                                 ParseError::kSyntax, Kind::kNull, "literals are lower case"},
    {"\"abc",                                ParseError::kSyntax, Kind::kNull, "unterminated string"},
    {"{\"a\":",                              ParseError::kSyntax, Kind::kNull, "truncated object"},
    {"[1",                                   ParseError::kSyntax, Kind::kNull, "truncated array"},
    {"/*c*/{}",                              ParseError::kSyntax, Kind::kNull, "comment"},
    {"\xC3\xA9",                             ParseError::kSyntax, Kind::kNull, "non-ascii outside a string"},

    // 값 뒤
    {"{}x",                                  ParseError::kTrailingData, Kind::kNull, "garbage after the value"},
    {"{} {}",                                ParseError::kTrailingData, Kind::kNull, "two values"},
    {"0x10",                                 ParseError::kTrailingData, Kind::kNull, "hex is 0 then garbage"},
    {"{}//",                                 ParseError::kTrailingData, Kind::kNull, "line comment after"},

    // 문자열
    {"\"a\tb\"",                             ParseError::kControlCharacter, Kind::kNull, "raw tab"},
    {"\"a\nb\"",                             ParseError::kControlCharacter, Kind::kNull, "raw newline"},
    {std::string_view("\"a\0b\"", 5),        ParseError::kControlCharacter, Kind::kNull, "raw NUL"},
    {"\"\\x41\"",                            ParseError::kBadEscape, Kind::kNull, "unknown escape"},
    {"\"\\U0041\"",                          ParseError::kBadEscape, Kind::kNull, "upper-case U"},
    {"\"\\u12\"",                            ParseError::kBadEscape, Kind::kNull, "short \\u"},
    {"\"\\u12G4\"",                          ParseError::kBadEscape, Kind::kNull, "non-hex digit"},
    {"\"\\'\"",                              ParseError::kBadEscape, Kind::kNull, "escaped single quote"},
    {"\"\\uD800\"",                          ParseError::kBadSurrogate, Kind::kNull, "lone high surrogate at end"},
    {"\"\\uD800x\"",                         ParseError::kBadSurrogate, Kind::kNull, "high surrogate then a plain byte"},
    {"\"\\uD800\\u0041\"",                   ParseError::kBadSurrogate, Kind::kNull, "high surrogate then a non-surrogate"},
    {"\"\\uD800\\uD800\"",                   ParseError::kBadSurrogate, Kind::kNull, "two high surrogates"},
    {"\"\\uDC00\"",                          ParseError::kBadSurrogate, Kind::kNull, "lone low surrogate"},
    {"\"\xC0\x80\"",                         ParseError::kBadUtf8, Kind::kNull, "overlong NUL"},
    {"\"\xC1\xBF\"",                         ParseError::kBadUtf8, Kind::kNull, "overlong two-byte"},
    {"\"\xE0\x80\x80\"",                     ParseError::kBadUtf8, Kind::kNull, "overlong three-byte"},
    {"\"\xF0\x80\x80\x80\"",                 ParseError::kBadUtf8, Kind::kNull, "overlong four-byte"},
    {"\"\xED\xA0\x80\"",                     ParseError::kBadUtf8, Kind::kNull, "U+D800 encoded raw"},
    {"\"\xF4\x90\x80\x80\"",                 ParseError::kBadUtf8, Kind::kNull, "U+110000"},
    {"\"\xF5\x80\x80\x80\"",                 ParseError::kBadUtf8, Kind::kNull, "F5 lead byte"},
    {"\"\xC3\"",                             ParseError::kBadUtf8, Kind::kNull, "truncated sequence"},
    {"\"\x80\"",                             ParseError::kBadUtf8, Kind::kNull, "lone continuation"},
    {"\"\xE2\x82" "A\"",                     ParseError::kBadUtf8, Kind::kNull, "bad third byte"},

    // 중복 키 (3.3. 중첩 객체까지)
    {"{\"a\":1,\"a\":2}",                    ParseError::kDuplicateKey, Kind::kNull, "same key twice"},
    {"{\"a\":1,\"a\":1}",                    ParseError::kDuplicateKey, Kind::kNull, "same value does not help"},
    {"{\"a\":1,\"\\u0061\":2}",              ParseError::kDuplicateKey, Kind::kNull, "compared after unescaping"},
    {"{\"x\":{\"a\":1,\"a\":2}}",            ParseError::kDuplicateKey, Kind::kNull, "nested object"},
    {"[{\"a\":1,\"a\":2}]",                  ParseError::kDuplicateKey, Kind::kNull, "object inside an array"},
};

std::string nested(std::size_t depth) {
    return std::string(depth, '[') + std::string(depth, ']');
}

}  // namespace

TEST_CASE("control_json: parse case table", "[control][json]") {
    for (const auto& row : kRows) {
        INFO("why=" << row.why);
        const auto got = parse(row.text);
        CHECK(got.error == row.error);
        CHECK(got.value.has_value() == (row.error == ParseError::kNone));
        if (got.value && row.error == ParseError::kNone) {
            CHECK(got.value->kind() == row.kind);
        }
    }
}

TEST_CASE("control_json: decoded values", "[control][json]") {
    REQUIRE(*parse("-0").value->as_integer() == 0);
    REQUIRE(*parse("9223372036854775807").value->as_integer() ==
            std::numeric_limits<std::int64_t>::max());
    REQUIRE(*parse("-9223372036854775808").value->as_integer() ==
            std::numeric_limits<std::int64_t>::min());
    REQUIRE(*parse("4294967295").value->as_integer() == 4294967295LL);
    REQUIRE(*parse("\"\\u00e9\"").value->as_string() == "\xC3\xA9");
    REQUIRE(*parse("\"\\uD83D\\uDE00\"").value->as_string() == "\xF0\x9F\x98\x80");
    REQUIRE(*parse("\"\\u20AC\"").value->as_string() == "\xE2\x82\xAC");
    REQUIRE(*parse("\"\\u0041\"").value->as_string() == "A");
    REQUIRE(*parse("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"").value->as_string() == "\"\\/\b\f\n\r\t");
    REQUIRE(*parse("\"\\u0000\"").value->as_string() == std::string_view("\0", 1));
    REQUIRE(*parse("true").value->as_bool() == true);

    const auto obj = parse("{\"b\":1,\"a\":\"x\"}");
    REQUIRE(obj.value);
    REQUIRE(obj.value->as_object()->size() == 2);
    REQUIRE((*obj.value->as_object())[0].key == "b");  // 순서를 지킨다
    REQUIRE(*obj.value->find("a")->as_string() == "x");
    REQUIRE(obj.value->find("c") == nullptr);
}

TEST_CASE("control_json: a bool is not an integer and an integer is not a bool", "[control][json]") {
    REQUIRE_FALSE(parse("true").value->as_integer().has_value());
    REQUIRE_FALSE(parse("1").value->as_bool().has_value());
    REQUIRE_FALSE(parse("1.0").value->as_integer().has_value());
    REQUIRE_FALSE(parse("\"1\"").value->as_integer().has_value());
}

TEST_CASE("control_json: nesting limit", "[control][json]") {
    // control_plane.md 2.6 CLIENT_JSON_MAX_DEPTH = 32. 값을 구현 상수에서 가져오지 않고 문서 값을
    // 직접 적는다. 상수가 문서와 어긋나면 여기서 떨어진다.
    constexpr std::size_t kDocumentedDepth = 32;
    CHECK(kMaxDepth == kDocumentedDepth);
    CHECK(parse(nested(32)).error == ParseError::kNone);
    CHECK(parse(nested(33)).error == ParseError::kTooDeep);

    std::string objects;
    for (std::size_t i = 0; i < kMaxDepth; ++i) {
        objects += "{\"a\":";
    }
    objects += "1" + std::string(kMaxDepth, '}');
    CHECK(parse(objects).error == ParseError::kNone);
    CHECK(parse("[" + objects + "]").error == ParseError::kTooDeep);

    // 본문 상한 안에서 재귀가 상한 없이 깊어지지 않는다.
    CHECK(parse(nested(kMaxBodyBytes / 2)).error == ParseError::kTooDeep);
}

TEST_CASE("control_json: size limit", "[control][json]") {
    // MAX_BODY_BYTES 바이트까지 받는다. 한 바이트 넘으면 거부한다.
    const std::string at_limit = "\"" + std::string(kMaxBodyBytes - 2, 'a') + "\"";
    REQUIRE(at_limit.size() == kMaxBodyBytes);
    CHECK(parse(at_limit).error == ParseError::kNone);
    const std::string over = "\"" + std::string(kMaxBodyBytes - 1, 'a') + "\"";
    CHECK(parse(over).error == ParseError::kTooLarge);
}

TEST_CASE("control_json: error tokens are fixed and carry no input", "[control][json]") {
    // 입력에 비밀이 들어 있어도 오류 표기에는 나오지 않는다. 이유만 돌려준다.
    const std::string secret = "0123456789abcdef0123456789abcdef";
    const auto got = parse("{\"peer_token\":\"" + secret + "\",\"peer_token\":1}");
    REQUIRE(got.error == ParseError::kDuplicateKey);
    REQUIRE(to_token(got.error) == "duplicate_key");
    REQUIRE(std::string(to_token(got.error)).find(secret) == std::string::npos);
}

TEST_CASE("control_json: serialize writes compact json in insertion order", "[control][json]") {
    Value obj = Value::object();
    REQUIRE(obj.insert("z", Value::integer(-5)));
    REQUIRE(obj.insert("a", Value::boolean(true)));
    Value arr = Value::array();
    REQUIRE(arr.push_back(Value::null()));
    REQUIRE(arr.push_back(Value::string("q\"\\\n\x01\x7F\xC3\xA9")));
    REQUIRE(obj.insert("list", std::move(arr)));
    REQUIRE_FALSE(obj.insert("a", Value::integer(1)));  // 같은 키는 넣지 않는다

    const auto text = serialize(obj);
    REQUIRE(text);
    REQUIRE(*text == "{\"z\":-5,\"a\":true,\"list\":[null,\"q\\\"\\\\\\n\\u0001\x7F\xC3\xA9\"]}");

    // 적은 것을 다시 읽으면 같은 값이다.
    const auto back = parse(*text);
    REQUIRE(back.value);
    REQUIRE(*back.value->find("list")->as_array()->at(1).as_string() ==
            "q\"\\\n\x01\x7F\xC3\xA9");
}

TEST_CASE("control_json: serialize refuses what it cannot write", "[control][json]") {
    CHECK_FALSE(serialize(Value::other_number()).has_value());
    CHECK_FALSE(serialize(Value::string("\xC3")).has_value());       // 잘린 UTF-8
    CHECK_FALSE(serialize(Value::string("\xED\xA0\x80")).has_value());  // 서로게이트

    Value deep = Value::array();
    for (std::size_t i = 0; i < kMaxDepth; ++i) {
        Value outer = Value::array();
        outer.push_back(std::move(deep));
        deep = std::move(outer);
    }
    CHECK_FALSE(serialize(deep).has_value());  // kMaxDepth + 1 단계
}
