"""control_plane.md 2.1 room_id 케이스 표와 2.2 peer_id, 2.3 peer_token, 2.4 client_nonce 의 생성과 형식.

ROOM_ID_CASES 가 2.1 케이스 표의 원본이다. 문서는 이 파일을 가리킨다.
변이는 tests/mutants/room_id.py 다.
"""

import re

import pytest

from controlplane import ids
from controlplane.errors import OpError

MISSING = object()  # "필드 없음". 본문에 room_id 키가 없다

# 출처: control_plane.md 2.1 room_id 의 케이스 표. 문서 한 행이 여기 한 원소다.
# "A / B / C" 로 적힌 행은 대안마다 한 원소다.
#   input   본문의 room_id 값. MISSING 이면 키가 없다
#   result  정규화 결과. None 이면 거부
#   error   거부일 때의 오류 코드
#   note    문서의 판정 열 설명
ROOM_ID_CASES = [
    dict(id="upper", input="ABCDEF", result="ABCDEF", error=None, note="통과"),
    dict(id="lower", input="abcdef", result="ABCDEF", error=None, note="통과"),
    dict(id="mixed-case", input="aBcDeF", result="ABCDEF", error=None, note="통과"),
    dict(id="len-5", input="ABCDE", result=None, error="bad_request", note="5글자"),
    dict(id="len-7", input="ABCDEFG", result=None, error="bad_request", note="7글자"),
    dict(id="digit-zero", input="ABCDE0", result=None, error="bad_request", note="`0` 은 알파벳 밖"),
    dict(id="letter-o", input="ABCDEO", result=None, error="bad_request", note="`O` 는 알파벳 밖"),
    dict(id="digit-one", input="ABCDE1", result=None, error="bad_request", note="`ABCDE1` / `ABCDEI` 행. 거부"),
    dict(id="letter-i", input="ABCDEI", result=None, error="bad_request", note="`ABCDE1` / `ABCDEI` 행. 거부"),
    dict(id="leading-space", input=" ABCDEF", result=None, error="bad_request",
         note="앞 공백. 공백을 지워 주지 않는다"),
    dict(id="fullwidth", input="\uff21\uff22\uff23\uff24\uff25\uff26", result=None, error="bad_request",
         note="전각 `ＡＢＣＤＥＦ`. ASCII 만 받는다"),
    dict(id="empty", input="", result=None, error="bad_request", note="빈 문자열. 거부"),
    dict(id="missing", input=MISSING, result=None, error="bad_request", note="필드 없음. 거부"),
    dict(id="not-string", input=123456, result=None, error="bad_request",
         note="문자열 아님. 거부 (JSON number 로 보낸 경우)"),
]

# 문서 표에 없는 행. 2.1 본문의 규칙("ASCII 만", "대문자로 바꾼 뒤 알파벳 검사")에서 나온다.
# 전각 행은 대문자 변환 뒤에도 알파벳 밖이라 ASCII 검사를 지워도 거부된다. 그래서 그 검사를
# 지키는 행이 따로 필요하다. 'ſ'(U+017F LATIN SMALL LETTER LONG S) 는 upper() 로 'S' 가 된다.
ROOM_ID_EXTRA_CASES = [
    dict(id="long-s", input="ABCDE\u017f", result=None, error="bad_request",
         note="비 ASCII 가 대문자 변환으로 알파벳 글자가 되는 경우. ASCII 검사가 변환보다 먼저다"),
]


def _body(case):
    return {} if case["input"] is MISSING else {"room_id": case["input"]}


@pytest.mark.parametrize("case", ROOM_ID_CASES + ROOM_ID_EXTRA_CASES, ids=lambda c: c["id"])
def test_room_id(case):
    value = _body(case).get("room_id")
    if case["error"] is None:
        assert ids.normalize_room_id(value) == case["result"]
    else:
        with pytest.raises(OpError) as exc:
            ids.normalize_room_id(value)
        assert exc.value.code == case["error"]


def test_room_id_table_is_whole():
    # 문서 표 11행. 대안으로 나뉜 행이 셋(ABCDE1/ABCDEI, ""/필드 없음/문자열 아님)이라 14원소다.
    assert len(ROOM_ID_CASES) == 14
    assert len({c["id"] for c in ROOM_ID_CASES + ROOM_ID_EXTRA_CASES}) == 15


def test_alphabet():
    """2.1 알파벳. 32자, 겹침 없음, I O 0 1 없음, 대문자와 숫자만."""
    a = ids.ROOM_ID_ALPHABET
    assert len(a) == 32 and len(set(a)) == 32
    assert not set(a) & set("IO01")
    assert set(a) <= set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")


def test_new_room_id_is_valid():
    for _ in range(200):
        rid = ids.new_room_id()
        assert len(rid) == ids.ROOM_ID_LEN
        assert ids.normalize_room_id(rid) == rid


def test_new_room_id_uses_draws(monkeypatch):
    """2.1. CSPRNG 가 알파벳에서 뽑은 글자를 그대로 6개 쓴다. 고정값을 돌려주는 구현이 여기서 걸린다."""
    picks = iter([0, 31, 7, 8, 30, 15])
    seen = []

    def fake_choice(seq):
        seen.append(seq)
        return seq[next(picks)]

    monkeypatch.setattr(ids.secrets, "choice", fake_choice)
    a = ids.ROOM_ID_ALPHABET
    assert ids.new_room_id() == a[0] + a[31] + a[7] + a[8] + a[30] + a[15]
    assert seen == [a] * ids.ROOM_ID_LEN


def test_new_peer_id_redraws_zero(monkeypatch):
    """2.2. secrets.randbits(32) 로 뽑고 0 이면 다시 뽑는다. 뽑은 값을 그대로 쓴다."""
    draws = iter([0, 0, 0xDEADBEEF])
    asked = []

    def fake(bits):
        asked.append(bits)
        return next(draws)

    monkeypatch.setattr(ids.secrets, "randbits", fake)
    assert ids.new_peer_id() == 0xDEADBEEF
    assert asked == [32, 32, 32]


def test_new_peer_id_range():
    for _ in range(200):
        assert 1 <= ids.new_peer_id() <= 0xFFFFFFFF


def test_new_peer_token_format():
    """2.3. 소문자 16진수 32자."""
    for _ in range(50):
        assert re.fullmatch(r"[0-9a-f]{32}", ids.new_peer_token())


def test_new_peer_token_uses_draw(monkeypatch):
    """2.3. secrets.token_hex(16), 즉 16바이트를 뽑아 그 값을 그대로 쓴다."""
    asked = []

    def fake(nbytes):
        asked.append(nbytes)
        return bytes(range(nbytes)).hex()

    monkeypatch.setattr(ids.secrets, "token_hex", fake)
    assert ids.new_peer_token() == "000102030405060708090a0b0c0d0e0f"
    assert asked == [16]


# 출처: control_plane.md 2.3 peer_token, 2.4 client_nonce 본문 "소문자 16진수 32자". 문서에 케이스 표는 없다.
# 두 필드 모두 요청에서도 이 형식만 받고 위반은 bad_request 다 (2.3, 2.4. 7.3 의 1번, 저장소 전).
# 한 표를 두 검사 함수에 돌린다.
HEX32_CASES = [
    dict(id="ok", input="0123456789abcdef0123456789abcdef", ok=True, note="소문자 16진수 32자"),
    dict(id="upper", input="0123456789ABCDEF0123456789ABCDEF", ok=False,
         note="소문자만 받는다. 대문자를 받으면 같은 값이 두 표기를 갖는다"),
    dict(id="len-31", input="0123456789abcdef0123456789abcde", ok=False, note="31자"),
    dict(id="len-33", input="0123456789abcdef0123456789abcdef0", ok=False, note="33자"),
    dict(id="non-hex", input="0123456789abcdef0123456789abcdeg", ok=False, note="g 는 16진수 밖"),
    dict(id="trailing-newline", input="0123456789abcdef0123456789abcdef\n", ok=False,
         note="정규식 $ 는 끝 줄바꿈 앞에서도 맞는다. fullmatch 로 본다"),
    dict(id="fullwidth-digit", input="\uff10123456789abcdef0123456789abcdef", ok=False,
         note="전각 숫자. ASCII 만"),
    dict(id="not-string", input=12345678901234567890123456789012, ok=False, note="문자열 아님"),
    dict(id="missing", input=None, ok=False, note="필드 없음"),
]

HEX32_CHECKS = {"client_nonce": ids.check_client_nonce, "peer_token": ids.check_peer_token}


@pytest.mark.parametrize("field", list(HEX32_CHECKS))
@pytest.mark.parametrize("case", HEX32_CASES, ids=lambda c: c["id"])
def test_hex32_field(case, field):
    check = HEX32_CHECKS[field]
    if case["ok"]:
        assert check(case["input"]) == case["input"]
    else:
        with pytest.raises(OpError) as exc:
            check(case["input"])
        assert exc.value.code == "bad_request"
        assert not (isinstance(case["input"], str) and case["input"] and case["input"] in exc.value.message)
