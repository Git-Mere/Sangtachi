"""3.3 HTTP 부분집합과 3.4 서버 쪽 시간 제한의 시험.

CASES 가 control_plane.md 3.3 HTTP 부분집합의 "서버 파싱 케이스 표" 다. 그 표의 단일 출처는 이
파일이다. 문서의 한 행이 원소 하나이고, "A / B / C" 처럼 대안이 나열된 행은 대안마다 원소 하나다.
order 는 문서 표의 행 번호(1부터)다.

RULE_CASES 는 표 밖의 규칙이다. 3.3 의 항목/규칙 표와 검사 순서에서 왔고 출처를 note 에 적었다.
"""

from __future__ import annotations

import asyncio
import json
from collections import Counter

import pytest

from controlplane.constants import MAX_BODY_BYTES, MAX_HEADER_BYTES
from controlplane.server import RateTable, Request, handle_request, response

BODY = b'{"room_id":"abc"}'  # 17 바이트. 문서 표의 Content-Length: 17 과 맞춘다
assert len(BODY) == 17
LINE = b"POST /v1/get_peers HTTP/1.1"
STD = [b"Host: cp.example", b"Content-Type: application/json", b"Content-Length: 17", b"Connection: close"]


def http(line: bytes = LINE, headers: list[bytes] | None = None, body: bytes = BODY) -> bytes:
    hs = STD if headers is None else headers
    return b"\r\n".join([line, *hs]) + b"\r\n\r\n" + body


def without(name: bytes) -> list[bytes]:
    return [h for h in STD if not h.lower().startswith(name.lower() + b":")]


def with_cl(value: bytes) -> list[bytes]:
    return without(b"Content-Length") + [b"Content-Length: " + value]


def sized(body: bytes, extra: list[bytes] | None = None) -> bytes:
    """Content-Length 를 본문 길이에 맞춘 요청."""
    return http(headers=with_cl(str(len(body)).encode()) + (extra or []), body=body)


def head_of_size(size: int) -> bytes:
    """요청 줄부터 빈 줄의 CRLF 까지가 정확히 size 바이트인 정상 요청."""
    base = http(body=b"")
    pad = size - len(base) - len(b"X-Pad: \r\n")
    out = http(headers=STD + [b"X-Pad: " + b"a" * pad])
    assert out.index(b"\r\n\r\n") + 4 == size
    return out


BODY_4096 = b'{"a":"' + b"x" * (MAX_BODY_BYTES - 8) + b'"}'
assert len(BODY_4096) == MAX_BODY_BYTES

RESULT = ("result",)  # 연산 결과. dispatch 가 불리고 200 이다
CLOSE = ("close",)  # 응답 없이 닫음


def err(status: int, code: str) -> tuple:
    return ("error", status, code)


# control_plane.md 3.3 HTTP 부분집합 "서버 파싱 케이스 표". 30행 -> 40원소 (대안 4행이 3개씩, 28·30행이 2개씩).
# 28~30행의 출처는 3.3 규칙 표의 '헤더 줄 문법' 과 '본문' 행이다 (헤더 이름 뒤 공백, 콜론 없는 줄, JSON 중복 키).
CASES = [
    dict(order=1, id="ok-get-peers", raw=http(), expect=RESULT,
         note="정상 POST /v1/get_peers, 올바른 Content-Length, JSON 객체 -> 연산 결과"),
    dict(order=2, id="get-method", raw=http(line=b"GET /v1/get_peers HTTP/1.1"), expect=err(405, "method_not_allowed"),
         note="GET /v1/get_peers -> 405 method_not_allowed"),
    dict(order=3, id="unknown-op", raw=http(line=b"POST /v1/unknown_op HTTP/1.1"), expect=err(404, "unknown_op"),
         note="POST /v1/unknown_op -> 404 unknown_op"),
    dict(order=4, id="no-prefix", raw=http(line=b"POST /get_peers HTTP/1.1"), expect=err(404, "unknown_op"),
         note="POST /get_peers (접두사 없음) -> 404 unknown_op"),
    dict(order=5, id="query-string", raw=http(line=b"POST /v1/get_peers?x=1 HTTP/1.1"), expect=err(404, "unknown_op"),
         note="POST /v1/get_peers?x=1 -> 404 unknown_op. 쿼리 문자열을 지원하지 않는다"),
    dict(order=6, id="no-content-length", raw=http(headers=without(b"Content-Length")), expect=err(411, "length_required"),
         note="Content-Length 없음 -> 411 length_required"),
    dict(order=7, id="cl-abc", raw=http(headers=with_cl(b"abc")), expect=err(400, "bad_request"),
         note="Content-Length: abc -> 400 bad_request"),
    dict(order=7, id="cl-negative", raw=http(headers=with_cl(b"-17"), body=BODY + b" " * 17), expect=err(400, "bad_request"),
         note="Content-Length 음수 -> 400 bad_request. 본문 뒤에 17 바이트를 더 붙여, 음수를 받아 끝에서 "
              "자르는 구현이 정상 JSON 을 얻고 이 행에서 떨어지게 했다"),
    dict(order=7, id="cl-twice", raw=http(headers=STD + [b"Content-Length: 17"]), expect=err(400, "bad_request"),
         note="Content-Length 두 번 -> 400 bad_request. 값이 같아도 거부한다 (3.3 본문, RFC 9112 6.3)"),
    dict(order=8, id="cl-4097", raw=http(headers=with_cl(b"4097"), body=b""), expect=err(413, "too_large"),
         note="Content-Length: 4097 -> 413 too_large. 본문을 읽지 않고 응답한 뒤 닫는다. "
              "본문을 보내지 않으므로 본문을 기다리는 구현은 시간 제한에 걸려 응답이 없다"),
    dict(order=9, id="te-chunked", raw=http(headers=STD + [b"Transfer-Encoding: chunked"]), expect=err(400, "bad_request"),
         note="Transfer-Encoding: chunked -> 400 bad_request. Transfer-Encoding 은 값이 무엇이든 거부"),
    dict(order=10, id="head-over-2048", raw=head_of_size(MAX_HEADER_BYTES + 1), expect=err(431, "too_large"),
         note="요청 줄 + 헤더가 2048 바이트 초과 -> 431 too_large"),
    dict(order=11, id="short-body-timeout", raw=http(body=BODY[:5]), expect=CLOSE,
         note="본문이 Content-Length 보다 짧고 5초 안에 더 오지 않음 -> 응답 없이 연결을 닫는다. "
              "카운터 http_read_timeout. 시험은 시간 제한을 짧게 주입한다"),
    dict(order=12, id="body-not-json", raw=sized(b"not json"), expect=err(400, "bad_request"),
         note="본문이 JSON 이 아님 -> 400 bad_request"),
    dict(order=12, id="body-array", raw=sized(b"[]"), expect=err(400, "bad_request"),
         note="본문이 배열 -> 400 bad_request"),
    dict(order=12, id="body-scalar", raw=sized(b"17"), expect=err(400, "bad_request"),
         note="본문이 스칼라 -> 400 bad_request"),
    dict(order=13, id="body-not-utf8", raw=sized(b'{"a":"\xff"}'), expect=err(400, "bad_request"),
         note="본문이 UTF-8 이 아님 -> 400 bad_request"),
    dict(order=14, id="http-1-0", raw=http(line=b"POST /v1/get_peers HTTP/1.0"), expect=err(400, "bad_request"),
         note="HTTP/1.0 -> 400 bad_request. HTTP/1.1 만 받는다"),
    dict(order=15, id="unknown-header", raw=http(headers=STD + [b"X-Foo: bar"]), expect=RESULT,
         note="정상 요청에 알 수 없는 헤더 X-Foo: bar -> 연산 결과. 무시한다"),
    dict(order=16, id="unknown-header-twice", raw=http(headers=STD + [b"X-Foo: bar", b"X-Foo: baz"]), expect=RESULT,
         note="알 수 없는 헤더 X-Foo 가 두 번 -> 연산 결과. 표 밖 헤더는 중복도 무시한다"),
    dict(order=17, id="lowercase-cl", raw=http(headers=without(b"Content-Length") + [b"content-length: 17"]), expect=RESULT,
         note="content-length: 17 (소문자) -> 연산 결과. 이름은 대소문자 무시"),
    dict(order=18, id="cl-4096", raw=http(headers=with_cl(b"4096"), body=BODY_4096), expect=RESULT,
         note="Content-Length: 4096 -> 연산 결과. 상한 안쪽이다. >= 로 잘못 쓴 구현이 이 행에서 걸린다"),
    dict(order=19, id="head-exactly-2048", raw=head_of_size(MAX_HEADER_BYTES), expect=RESULT,
         note="요청 줄 + 헤더가 정확히 2048 바이트 -> 연산 결과. 상한 안쪽이다"),
    dict(order=20, id="cl-plus-17", raw=http(headers=with_cl(b"+17")), expect=err(400, "bad_request"),
         note="Content-Length: +17 -> 400 bad_request. 값은 ^[0-9]+$ 만"),
    dict(order=20, id="cl-1-7", raw=http(headers=with_cl(b"1_7")), expect=err(400, "bad_request"),
         note="Content-Length: 1_7 -> 400 bad_request. 값은 ^[0-9]+$ 만"),
    dict(order=20, id="cl-space-17", raw=http(headers=with_cl(b" 17")), expect=err(400, "bad_request"),
         note="Content-Length:  17 (값 앞 공백) -> 400 bad_request. 값은 ^[0-9]+$ 만"),
    dict(order=21, id="obs-fold", raw=http(headers=STD + [b"X-Foo: a", b" X-Bar: b"]), expect=err(400, "bad_request"),
         note="헤더 줄이 공백으로 시작 (obs-fold) -> 400 bad_request. 이어 붙이지 않는다"),
    dict(order=22, id="line-two-spaces", raw=http(line=b"POST  /v1/get_peers HTTP/1.1"), expect=err(400, "bad_request"),
         note="POST  /v1/get_peers HTTP/1.1 (공백 둘) -> 400 bad_request. 요청 줄은 바이트로 일치해야 한다"),
    dict(order=22, id="line-trailing-slash", raw=http(line=b"POST /v1/get_peers/ HTTP/1.1"), expect=err(400, "bad_request"),
         note="POST /v1/get_peers/ HTTP/1.1 -> 400 bad_request. 요청 줄은 바이트로 일치해야 한다"),
    dict(order=22, id="line-absolute-form", raw=http(line=b"POST http://h/v1/get_peers HTTP/1.1"), expect=err(400, "bad_request"),
         note="POST http://h/v1/get_peers HTTP/1.1 -> 400 bad_request. 요청 줄은 바이트로 일치해야 한다"),
    dict(order=23, id="no-host", raw=http(headers=without(b"Host")), expect=RESULT,
         note="Host 없음 -> 연산 결과. 검사하지 않는다"),
    dict(order=24, id="content-type-text-plain", raw=http(headers=without(b"Content-Type") + [b"Content-Type: text/plain"]),
         expect=RESULT, note="Content-Type: text/plain -> 연산 결과. 검사하지 않는다"),
    dict(order=25, id="body-longer-than-cl", raw=http(body=BODY + b"GET /v1/x HTTP/1.1\r\n\r\n"), expect=RESULT,
         note="본문이 Content-Length 보다 김 -> 연산 결과. 앞의 Content-Length 바이트만 읽고 닫는다"),
    dict(order=26, id="get-unknown-op", raw=http(line=b"GET /v1/unknown_op HTTP/1.1"), expect=err(405, "method_not_allowed"),
         note="GET /v1/unknown_op -> 405 method_not_allowed. 검사 순서대로 메서드가 경로보다 먼저다"),
    dict(order=27, id="te-without-cl", raw=http(headers=without(b"Content-Length") + [b"Transfer-Encoding: chunked"]),
         expect=err(400, "bad_request"),
         note="Transfer-Encoding: chunked 이고 Content-Length 없음 -> 400 bad_request. "
              "Transfer-Encoding 이 Content-Length 부재보다 먼저다"),
    dict(order=28, id="header-space-before-colon", raw=http(headers=without(b"Content-Length") + [b"Content-Length : 17"]),
         expect=err(400, "bad_request"),
         note="헤더 이름과 콜론 사이에 공백 -> 400 bad_request (RFC 9112 5.1). 무시하면 411 이 된다"),
    dict(order=28, id="header-tab-before-colon", raw=http(headers=STD + [b"X-Foo\t: bar"]), expect=err(400, "bad_request"),
         note="헤더 이름과 콜론 사이에 탭 -> 400 bad_request. 표 밖 헤더에도 적용한다"),
    dict(order=29, id="header-no-colon", raw=http(headers=STD + [b"X-Foo bar"]), expect=err(400, "bad_request"),
         note="콜론이 없는 헤더 줄 -> 400 bad_request"),
    dict(order=30, id="json-duplicate-key", raw=sized(b'{"room_id":"abc","room_id":"x"}'), expect=err(400, "bad_request"),
         note="JSON 객체 안의 중복 키 -> 400 bad_request"),
    dict(order=30, id="json-duplicate-key-nested", raw=sized(b'{"room_id":"abc","n":{"a":1,"a":2}}'),
         expect=err(400, "bad_request"), note="중첩 객체 안의 중복 키 -> 400 bad_request"),
]

DOC_ROWS = 30

# 표 밖 규칙. 출처는 note 에 적은 3.3 의 항목/규칙 표 행과 검사 순서다.
RULE_CASES = [
    dict(id="line-tab", raw=http(line=b"POST\t/v1/get_peers HTTP/1.1"), expect=err(400, "bad_request"),
         note="3.3 규칙 '바이트 정확 일치': 탭은 거부"),
    dict(id="line-percent", raw=http(line=b"POST /v1/get%5Fpeers HTTP/1.1"), expect=err(400, "bad_request"),
         note="3.3 규칙 '바이트 정확 일치': 퍼센트 인코딩(%5F)은 거부"),
    dict(id="path-case", raw=http(line=b"POST /v1/GET_PEERS HTTP/1.1"), expect=err(404, "unknown_op"),
         note="3.3 규칙 '바이트 정확 일치': 경로는 대소문자를 구분한다"),
    dict(id="cl-trailing-space", raw=http(headers=with_cl(b"17 ")), expect=err(400, "bad_request"),
         note="3.3 규칙 'Content-Length 값': 앞뒤 공백 거부"),
    dict(id="cl-fullwidth", raw=http(headers=with_cl("１７".encode())), expect=err(400, "bad_request"),
         note="3.3 규칙 'Content-Length 값': 전각 숫자 거부"),
    dict(id="cl-twice-different", raw=http(headers=STD + [b"Content-Length: 18"]), expect=err(400, "bad_request"),
         note="3.3 'Content-Length 가 두 번 오는 것을 특히 거부한다': 값이 다른 경우"),
    dict(id="cl-leading-zeros", raw=http(headers=with_cl(b"0017")), expect=RESULT,
         note="3.3 규칙 'Content-Length 값': ^[0-9]+$ 에 맞으므로 받는다. 값은 17"),
    dict(id="cl-huge", raw=http(headers=with_cl(b"9" * 400), body=b""), expect=err(413, "too_large"),
         note="3.3 검사 순서 7: 형식에 맞고 상한을 넘으면 413. 큰 수를 정수 변환에 맡기지 않는다"),
    dict(id="te-identity", raw=http(headers=STD + [b"Transfer-Encoding: identity"]), expect=err(400, "bad_request"),
         note="3.3 규칙 '본문 길이': Transfer-Encoding 이 있으면 값이 무엇이든 거부"),
    dict(id="host-twice", raw=http(headers=STD + [b"Host: other"]), expect=err(400, "bad_request"),
         note="3.3 규칙 '헤더': 표에 있는 헤더가 두 번 오면 거부. 표의 헤더 다섯에 Host 가 있다"),
    dict(id="order-version-before-method", raw=http(line=b"GET /v1/get_peers HTTP/1.0"), expect=err(400, "bad_request"),
         note="3.3 검사 순서 1 이 2 보다 먼저"),
    dict(id="order-path-before-size", raw=head_of_size(MAX_HEADER_BYTES + 1).replace(b"/v1/get_peers", b"/v1/get_peerz"),
         expect=err(404, "unknown_op"), note="3.3 검사 순서 3 이 4 보다 먼저"),
    dict(id="order-size-before-fold",
         raw=head_of_size(MAX_HEADER_BYTES + 1).replace(b"Connection: close", b" onnection: close"),
         expect=err(431, "too_large"), note="3.3 검사 순서 4 가 5 보다 먼저"),
    dict(id="order-te-before-cl-limit", raw=http(headers=with_cl(b"4097") + [b"Transfer-Encoding: chunked"], body=b""),
         expect=err(400, "bad_request"), note="3.3 검사 순서 6 이 7 보다 먼저"),
    dict(id="order-limit-before-body", raw=http(headers=with_cl(b"4097"), body=b"[]"), expect=err(413, "too_large"),
         note="3.3 검사 순서 7 이 9 보다 먼저"),
    dict(id="json-nan", raw=sized(b'{"a":NaN}'), expect=err(400, "bad_request"),
         note="3.3 규칙 '본문': UTF-8 JSON 객체. NaN 은 JSON 이 아니다"),
]

FAST_TIMEOUT_S = 0.3


class Spy:
    """dispatch 대역. 불린 요청을 센다."""

    def __init__(self):
        self.calls: list[Request] = []

    def __call__(self, req: Request) -> dict:
        self.calls.append(req)
        return {"echo": req.op}


def run(raw: bytes, timeout: float = FAST_TIMEOUT_S, dispatch=None, src: str = "198.51.100.7"):
    """raw 를 보내고 EOF 는 보내지 않는다(응답을 기다리는 클라이언트). (응답, spy, 카운터)."""
    spy = dispatch or Spy()
    counters: Counter = Counter()

    async def go():
        reader = asyncio.StreamReader()
        reader.feed_data(raw)
        return await handle_request(reader, src, rate=RateTable(counters=counters), dispatch=spy,
                                    counters=counters, read_timeout_s=timeout)

    return asyncio.run(go()), spy, counters


def parse_response(out: bytes) -> tuple[int, dict]:
    head, _, body = out.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    assert lines[0].startswith(b"HTTP/1.1 ")
    status = int(lines[0].split(b" ")[1])
    assert lines[1:] == [b"Content-Type: application/json", b"Content-Length: %d" % len(body), b"Connection: close"]
    return status, json.loads(body)


def check(case, out, spy, counters):
    kind = case["expect"][0]
    if kind == "close":
        assert out is None
        assert counters["http_read_timeout"] == 1
        assert spy.calls == []
        return
    assert out is not None
    status, payload = parse_response(out)
    if kind == "result":
        assert (status, payload) == (200, {"ok": True, "echo": "get_peers"})
        assert len(spy.calls) == 1
        expected_body = json.loads(case["raw"].partition(b"\r\n\r\n")[2][: len(BODY_4096)]) \
            if case["id"] == "cl-4096" else json.loads(BODY)
        assert spy.calls[0] == Request("get_peers", expected_body)
    else:
        _, want_status, want_code = case["expect"]
        assert status == want_status
        assert payload["ok"] is False and payload["error"] == want_code
        assert set(payload) == {"ok", "error", "message"}
        assert spy.calls == []
    assert counters["http_read_timeout"] == 0


def test_table_shape():
    ids = [c["id"] for c in CASES + RULE_CASES]
    assert len(ids) == len(set(ids))
    assert sorted({c["order"] for c in CASES}) == list(range(1, DOC_ROWS + 1))
    assert all(c["note"].strip() for c in CASES + RULE_CASES)
    assert all(b"".join(c["pieces"]) for c in STREAM_CASES)


@pytest.mark.parametrize("case", CASES, ids=[c["id"] for c in CASES])
def test_parse_table(case):
    check(case, *run(case["raw"]))


@pytest.mark.parametrize("case", RULE_CASES, ids=[c["id"] for c in RULE_CASES])
def test_parse_rules(case):
    check(case, *run(case["raw"]))


# 3.3 '파싱 실패 응답은 요청 내용을 되돌려 보내지 않는다'. 표지 문자열을 요청 곳곳에 넣는다.
MARK = b"ECHOMARK"
ECHO_CASES = [
    dict(id="echo-line", raw=http(line=b"POST /v1/ECHOMARK HTTP/1.1"), note="경로에 표지. 404"),
    dict(id="echo-method", raw=http(line=b"ECHOMARK /v1/get_peers HTTP/1.1"), note="메서드에 표지. 405"),
    dict(id="echo-version", raw=http(line=b"POST /v1/get_peers ECHOMARK"), note="버전 자리에 표지. 400"),
    dict(id="echo-cl", raw=http(headers=with_cl(MARK)), note="Content-Length 값에 표지. 400"),
    dict(id="echo-header", raw=http(headers=STD + [b" " + MARK]), note="접힌 헤더에 표지. 400"),
    dict(id="echo-body", raw=sized(b"[ECHOMARK]"), note="JSON 이 아닌 본문에 표지. 400"),
    dict(id="echo-body-array", raw=sized(b'["ECHOMARK"]'), note="배열 본문에 표지. 400"),
]


@pytest.mark.parametrize("case", ECHO_CASES, ids=[c["id"] for c in ECHO_CASES])
def test_no_echo(case):
    out, spy, _ = run(case["raw"])
    assert out is not None and spy.calls == []
    assert MARK not in out and MARK.lower() not in out.lower()


def test_response_format():
    # 3.3 응답 형식. 상태 줄, 세 헤더, 빈 줄, JSON 본문.
    out = response(404, {"ok": False, "error": "unknown_op", "message": "unknown_op"})
    body = b'{"ok":false,"error":"unknown_op","message":"unknown_op"}'
    assert out == (b"HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\n"
                   b"Content-Length: %d\r\nConnection: close\r\n\r\n" % len(body)) + body


def test_eof_before_head_closes_without_response():
    # 상대가 머리를 다 보내기 전에 닫음. 응답할 상대가 없다 (3.3 검사 순서 8번). 시간 제한 카운터와 섞지 않는다.
    async def go():
        reader = asyncio.StreamReader()
        reader.feed_data(b"POST /v1/get_peers HTTP/1.1\r\n")
        reader.feed_eof()
        counters: Counter = Counter()
        out = await handle_request(reader, "198.51.100.7", rate=RateTable(), dispatch=Spy(), counters=counters,
                                   read_timeout_s=FAST_TIMEOUT_S)
        return out, counters

    out, counters = asyncio.run(go())
    assert out is None and counters["http_read_timeout"] == 0


# ---------------------------------------------------------------- 나눠 들어오는 요청 (3.3, 3.4)

def split_at(raw: bytes, cuts: list[int]) -> list[bytes]:
    edges = [0, *cuts, len(raw)]
    return [raw[a:b] for a, b in zip(edges, edges[1:])]


_RAW = http()
_HEAD_END = _RAW.index(b"\r\n\r\n")

# pieces 를 차례로 넣고 그 사이에 gap 초만큼 이벤트 루프를 양보한다. eof 면 끝에 EOF 를 넣는다.
STREAM_CASES = [
    dict(id="split-pieces", pieces=split_at(_RAW, [10, _HEAD_END + 2, len(_RAW) - 5]), gap=0.0, eof=False,
         expect=RESULT,
         note="요청 줄 안, CRLFCRLF 안, 본문 안에서 쪼개 늦게 넣는다 -> 연산 결과. 한 번의 read 에 머리가 다 "
              "온다고 가정하는 구현이 이 행에서 떨어진다"),
    dict(id="drip-past-total-timeout", pieces=split_at(_RAW, list(range(10, len(_RAW), 10))), gap=0.05, eof=False,
         expect=CLOSE,
         note="3.4: 수락부터 요청 한 건을 다 읽기까지 SERVER_READ_TIMEOUT_S 하나다. 조각 사이는 0.05초로 "
              "시간 제한(0.3초)보다 짧지만 전체는 넘는다 -> 응답 없이 닫고 http_read_timeout. "
              "read 마다 시간 제한을 다시 거는 구현이 이 행에서 떨어진다"),
    dict(id="eof-mid-body", pieces=[http(body=BODY[:5])], gap=0.0, eof=True, expect=("eof",),
         note="정상 헤더와 짧은 본문 뒤에 EOF -> 응답 없음, dispatch 없음, 카운터 없음 (EOF 는 시간 제한이 아니다)"),
]


def run_stream(case, timeout: float = FAST_TIMEOUT_S):
    spy = Spy()
    counters: Counter = Counter()

    async def go():
        reader = asyncio.StreamReader()

        async def feeder():
            for piece in case["pieces"]:
                reader.feed_data(piece)
                if case["gap"]:
                    await asyncio.sleep(case["gap"])
                else:
                    for _ in range(3):
                        await asyncio.sleep(0)
            if case["eof"]:
                reader.feed_eof()

        task = asyncio.create_task(feeder())
        try:
            return await handle_request(reader, "198.51.100.7", rate=RateTable(counters=counters), dispatch=spy,
                                        counters=counters, read_timeout_s=timeout)
        finally:
            task.cancel()

    return asyncio.run(go()), spy, counters


@pytest.mark.parametrize("case", STREAM_CASES, ids=[c["id"] for c in STREAM_CASES])
def test_stream(case):
    out, spy, counters = run_stream(case)
    if case["expect"][0] == "eof":
        assert out is None and spy.calls == [] and sum(counters.values()) == 0
        return
    check(dict(case, raw=_RAW), out, spy, counters)
