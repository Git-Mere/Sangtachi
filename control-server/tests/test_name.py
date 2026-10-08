"""control_plane.md 2.7 name 의 케이스 표.

NAME_CASES 가 2.7 형식 검사 케이스 표의 원본이다. 통과/거부 행은 roadmap.md Phase 4 검증의 "이름 형식
검사" 목록을 모두 담는다. 변이는 tests/mutants/name.py 다.

판정 함수는 ids.check_name 이고 비교 키는 ids.name_key 다. 둘 다 저장소를 부르지 않는다 (7.3 의 1번).
방 안 유일성과 회수는 저장소가 있어야 돌므로 test_ops.py 의 NAME_SCENARIOS 에 있다.
"""

import pytest

from controlplane import ids
from controlplane.constants import MAX_NAME_LEN
from controlplane.errors import OpError

MISSING = object()  # "필드 없음". 본문에 name 키가 없다

# 출처: control_plane.md 2.7 name 과 roadmap.md Phase 4 검증의 "이름 형식 검사".
#   input   본문의 name 값. MISSING 이면 키가 없다
#   result  통과면 돌려받는 값(등록한 그대로). None 이면 거부
#   error   거부일 때의 오류 코드
#   note    근거
NAME_CASES = [
    # 통과. roadmap: 1자, 16자, 숫자만, 밑줄만, 대소문자가 섞인 것
    dict(id="one-char", input="a", result="a", error=None, note="길이 하한 1자"),
    dict(id="sixteen-chars", input="Abcdefghij012345", result="Abcdefghij012345", error=None,
         note="길이 상한 MAX_NAME_LEN(16)자. 상한을 >= 로 잘못 쓴 구현이 이 행에서 걸린다"),
    dict(id="digits-only", input="0123456789", result="0123456789", error=None, note="숫자만. 63자 집합 안이다"),
    dict(id="underscore-only", input="___", result="___", error=None,
         note="밑줄만. str.isalnum() 은 밑줄을 받지 않는다"),
    dict(id="mixed-case", input="AliceZ_z9", result="AliceZ_z9", error=None,
         note="대소문자를 등록한 그대로 돌려준다. 바꾸지 않는다"),
    # 거부. roadmap: 빈 문자열, 17자, 가운데 공백, 앞이나 뒤의 공백, 하이픈, 한글, 전각 숫자, 끝 줄바꿈,
    # 제어 문자, 문자열이 아닌 값
    dict(id="empty", input="", result=None, error="bad_request", note="길이가 1 미만"),
    dict(id="seventeen-chars", input="Abcdefghij0123456", result=None, error="bad_request",
         note="MAX_NAME_LEN 을 넘는다"),
    dict(id="inner-space", input="Al ice", result=None, error="bad_request", note="가운데 공백. 63자 밖"),
    dict(id="leading-space", input=" Alice", result=None, error="bad_request",
         note="앞 공백. 앞뒤 공백을 지워 주지 않는다"),
    dict(id="trailing-space", input="Alice ", result=None, error="bad_request",
         note="뒤 공백. 앞뒤 공백을 지워 주지 않는다"),
    dict(id="hyphen", input="Al-ice", result=None, error="bad_request", note="하이픈. 63자 밖"),
    dict(id="hangul", input="\uc568\ub9ac\uc2a4", result=None, error="bad_request",
         note="한글 '앨리스'. str.isalnum() 과 정규식 \\w 는 받는다"),
    dict(id="fullwidth-digits", input="\uff11\uff12\uff13", result=None, error="bad_request",
         note="전각 숫자 '１２３'. str.isalnum() 과 정규식 \\w 는 받는다"),
    dict(id="trailing-newline", input="Alice\n", result=None, error="bad_request",
         note="끝 줄바꿈. 정규식 $ 는 끝 줄바꿈 앞에서도 맞는다 (9장)"),
    dict(id="control-char", input="Al\x01ice", result=None, error="bad_request", note="제어 문자 U+0001"),
    dict(id="tab", input="Al\tice", result=None, error="bad_request", note="제어 문자 탭"),
    dict(id="not-string-number", input=12345, result=None, error="bad_request", note="JSON 문자열이 아니다. 숫자"),
    dict(id="not-string-null", input=None, result=None, error="bad_request", note="JSON 문자열이 아니다. null"),
    dict(id="not-string-bool", input=True, result=None, error="bad_request", note="JSON 문자열이 아니다. true"),
    dict(id="not-string-list", input=["Alice"], result=None, error="bad_request", note="JSON 문자열이 아니다. 배열"),
    dict(id="missing", input=MISSING, result=None, error="bad_request", note="4.2, 4.3 요청 표. name 은 필수다"),
]

# 문서 목록에 없는 행. 2.7 본문의 규칙과 9장 "코드를 읽어서만 판정할 수 있는 것" 의 이름 행에서 나온다.
#   long-s / kelvin: 비 ASCII 가 소문자 변환으로 63자 안의 글자가 된다. 문자 집합 검사가 소문자 변환보다
#   먼저라는 규칙(2.7)을 지킨다. 'K'(U+212A KELVIN SIGN) 는 lower() 로 'k' 가 된다.
#   latin-letter / arabic-digit: 유니코드 술어(isalnum, \w, \d)가 받는 글자다.
NAME_EXTRA_CASES = [
    dict(id="kelvin-sign", input="\u212aate", result=None, error="bad_request",
         note="U+212A 는 lower() 로 'k' 가 된다. 문자 집합 검사가 소문자 변환보다 먼저다"),
    dict(id="latin-e-acute", input="Ren\u00e9", result=None, error="bad_request",
         note="ASCII 밖 라틴 글자. isalnum() 과 \\w 가 받는다"),
    dict(id="arabic-indic-digit", input="p\u0661", result=None, error="bad_request",
         note="ASCII 밖 숫자. isalnum(), \\w, \\d 가 받는다"),
    dict(id="del-char", input="Alice\x7f", result=None, error="bad_request", note="제어 문자 DEL"),
    dict(id="sixteen-underscores-and-digits", input="_" * 8 + "9" * 8, result="_" * 8 + "9" * 8, error=None,
         note="상한 16자를 63자 집합의 비영문자로 채운다"),
]

ALL_CASES = NAME_CASES + NAME_EXTRA_CASES


def _value(case):
    body = {} if case["input"] is MISSING else {"name": case["input"]}
    return body.get("name")


@pytest.mark.parametrize("case", ALL_CASES, ids=lambda c: c["id"])
def test_name(case):
    value = _value(case)
    if case["error"] is None:
        assert ids.check_name(value) == case["result"]
    else:
        with pytest.raises(OpError) as exc:
            ids.check_name(value)
        assert exc.value.code == case["error"]


def test_name_table_is_whole():
    # roadmap Phase 4 의 "이름 형식 검사" 목록: 통과 5, 거부 10. 거부 가운데 "앞이나 뒤의 공백" 이 둘,
    # "제어 문자" 가 둘(U+0001, 탭), "문자열이 아닌 값" 이 다섯(숫자, null, true, 배열, 필드 없음)이다.
    # 5 + (10 + 1 + 1 + 4) = 21.
    assert len(NAME_CASES) == 21
    assert sum(c["error"] is None for c in NAME_CASES) == 5
    assert len({c["id"] for c in ALL_CASES}) == len(ALL_CASES)


def test_max_name_len_value():
    # 2.6 상수. protocol.md 3장과 같은 값이다. 16자/17자 행이 이 값에 기댄다.
    assert MAX_NAME_LEN == 16


def test_name_alphabet():
    """2.7 의 63자. 겹침 없음, 대문자 26, 소문자 26, 숫자 10, 밑줄 1."""
    a = ids.NAME_ALPHABET
    assert len(a) == 63
    assert a == frozenset("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_")


# 출처: control_plane.md 2.7 "방 안 유일성은 대소문자를 무시하고 판정한다. 비교 키는 ASCII 소문자로 바꾼 값이다"
# 와 6.3 NAME#<이름 키>.
KEY_CASES = [
    dict(id="upper", name="ALICE", key="alice"),
    dict(id="mixed", name="AliCe_9", key="alice_9"),
    dict(id="already-lower", name="bob", key="bob"),
    dict(id="digits-underscore", name="_42_", key="_42_"),
]


@pytest.mark.parametrize("case", KEY_CASES, ids=lambda c: c["id"])
def test_name_key(case):
    assert ids.name_key(case["name"]) == case["key"]
    assert ids.name_key(case["name"]) == ids.name_key(case["name"].swapcase())
