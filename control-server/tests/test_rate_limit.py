"""6.4 속도 제한과 7.2 요청 처리 한 바퀴(파싱 뒤)의 시험.

CASES 가 control_plane.md 6.4 속도 제한의 "케이스 표" 다. 그 표의 단일 출처는 이 파일이다. 문서의
한 행이 원소 하나다. 순서 번호가 있는 행(1~14)은 이어지는 시나리오라서 각 원소의 after 가 앞
행들을 다시 돌린 뒤 그 행을 판정한다. at 은 시나리오 시작부터의 초다.

요청은 전부 HTTP 바이트로 handle_request 를 지난다. dispatch 대역이 연산 결과를 흉내 내고 불린
횟수를 센다. dispatch 가 불리지 않았으면 저장소를 읽지 않은 것이다 (7.3: 저장소 읽기는 dispatch 안).

RULE_CASES 는 표 밖의 규칙이다. 6.4 의 항목 표와 본문에서 왔고 출처를 note 에 적었다.
"""

from __future__ import annotations

import asyncio
import json
from collections import Counter

import pytest

from controlplane.constants import MAX_RATE_ENTRIES, RATE_LIMIT_BUCKET
from controlplane.errors import OpError
from controlplane.server import RateTable, Request, handle_request

A = "198.51.100.1"
B = "198.51.100.2"
T0 = 1000.0

JOIN_MISSING = ("join_room", {"room_id": "missing"})  # 없는 방 -> room_not_found
PEERS_OK = ("get_peers", {"room_id": "exists"})  # 있는 방에 정상 get_peers -> 성공
BAD = ("get_peers", {})  # 필드 누락 -> dispatch 가 bad_request
NO_ENTRY = None  # 표에 항목이 없다. 토큰이 가득한 것과 같다 (6.4 항목 생성)

# control_plane.md 6.4 속도 제한 "케이스 표". 8행 -> 8원소.
# tokens 는 그 행 뒤 그 출발지의 저장된 토큰이다. NO_ENTRY 는 문서의 10 또는 - 이고 항목이 없어야 한다.
CASES = [
    dict(order="1~10", id="rows-1-10", after=[], at=0.0, src=A, req=JOIN_MISSING, repeat=10,
         expect="room_not_found", tokens=0, store_reads=10,
         note="없는 방 join_room 10회 -> room_not_found 10회. 남은 토큰 0"),
    dict(order="11", id="row-11", after=["rows-1-10"], at=0.0, src=A, req=JOIN_MISSING, repeat=1,
         expect="rate_limited", tokens=0, store_reads=0,
         note="없는 방 join_room -> rate_limited. 저장소 읽기 없음. 남은 토큰 0"),
    dict(order="12", id="row-12", after=["rows-1-10", "row-11"], at=5.0, src=A, req=PEERS_OK, repeat=1,
         expect="rate_limited", tokens=0, store_reads=0,
         note="있는 방에 정상 get_peers -> rate_limited. 예산은 출발지 단위라 정상 요청도 막힌다. 남은 토큰 0. "
              "시각 5초는 문서가 정하지 않았다. rate_limited 가 보충 기준을 미루면 13번이 떨어지도록 0 이 아닌 값을 골랐다"),
    dict(order="13", id="row-13", after=["rows-1-10", "row-11", "row-12"], at=6.0, src=A, req=JOIN_MISSING, repeat=1,
         expect="room_not_found", tokens=0, store_reads=1,
         note="11번 응답으로부터 6초 뒤, 없는 방 join_room -> 토큰 1 보충 후 소모. room_not_found. 남은 토큰 0"),
    dict(order="14", id="row-14", after=["rows-1-10", "row-11", "row-12", "row-13"], at=76.0, src=A, req=PEERS_OK,
         repeat=1, expect="ok", tokens=NO_ENTRY, store_reads=1,
         note="13번의 보충 시각(6초)으로부터 70초 뒤 (그 사이 아무 요청 없음) -> 토큰이 상한 10 에 닿아 항목이 "
              "지워진다. 그 뒤 첫 요청은 항목이 없는 출발지로 처리된다. 남은 토큰 -. "
              "14번이 60초가 아니라 70초인 이유: 정확히 60초를 기대값으로 적으면 시계 해상도에 따라 9개에서 멈춘 "
              "구현과 10개를 채운 구현이 갈린다. 경계를 판정에 쓰지 않는다. 그 요청의 종류는 문서가 정하지 않았다. "
              "성공 요청을 써서 토큰 소모 없이 항목 제거만 보이게 했다"),
    dict(order="-", id="other-source", after=["rows-1-10"], at=0.0, src=B, req=JOIN_MISSING, repeat=1,
         expect="room_not_found", tokens=9, store_reads=1,
         note="다른 출발지에서 없는 방 join_room -> room_not_found. 표가 출발지별이다. 그 출발지 9. "
              "A 가 예산을 다 쓴 뒤에 돌린다"),
    dict(order="-", id="success-100", after=[], at=0.0, src=A, req=PEERS_OK, repeat=100,
         expect="ok", tokens=NO_ENTRY, store_reads=100,
         note="정상 get_peers 100회 (성공) -> 전부 처리. 10. 성공은 세지 않는다"),
    dict(order="-", id="bad-request-100", after=[], at=0.0, src=A, req=BAD, repeat=100,
         expect="bad_request", tokens=NO_ENTRY, store_reads=100,
         note="bad_request 100회 -> 전부 bad_request. 10. 세지 않는다. "
              "bad_request 는 연산의 필드 검사(7.3 1단계)에서 나는 것으로 흉내 냈다"),
]

DOC_ROWS = 8
BY_ID = {c["id"]: c for c in CASES}


class Clock:
    def __init__(self):
        self.t = T0

    def __call__(self) -> float:
        return self.t


class FakeOps:
    """dispatch 대역. 불린 횟수가 저장소 읽기 횟수다."""

    def __init__(self):
        self.calls = 0

    def __call__(self, req: Request) -> dict:
        self.calls += 1
        if "room_id" not in req.body:
            raise OpError("bad_request")
        if req.body["room_id"] != "exists":
            raise OpError("room_not_found")
        return {"peers": []}


class World:
    def __init__(self):
        self.clock = Clock()
        self.counters: Counter = Counter()
        self.rate = RateTable(clock=self.clock, counters=self.counters)
        self.ops = FakeOps()

    def send(self, src: str, op: str, body: dict) -> str:
        raw_body = json.dumps(body).encode()
        raw = (f"POST /v1/{op} HTTP/1.1\r\nContent-Length: {len(raw_body)}\r\n\r\n").encode() + raw_body
        return self.send_raw(src, raw)

    def send_raw(self, src: str, raw: bytes) -> str:
        async def go():
            reader = asyncio.StreamReader()
            reader.feed_data(raw)
            return await handle_request(reader, src, rate=self.rate, dispatch=self.ops, counters=self.counters,
                                        read_timeout_s=1.0)

        out = asyncio.run(go())
        payload = json.loads(out.partition(b"\r\n\r\n")[2])
        return "ok" if payload["ok"] else payload["error"]

    def play(self, case) -> list[str]:
        self.clock.t = T0 + case["at"]
        return [self.send(case["src"], *case["req"]) for _ in range(case["repeat"])]


def test_table_shape():
    ids = [c["id"] for c in CASES + RULE_CASES]
    assert len(ids) == len(set(ids)) and len(CASES) == DOC_ROWS
    assert all(c["note"].strip() for c in CASES + RULE_CASES)
    assert all(a in BY_ID for c in CASES for a in c["after"])


@pytest.mark.parametrize("case", CASES, ids=[c["id"] for c in CASES])
def test_rate_table(case):
    w = World()
    for prev in case["after"]:
        w.play(BY_ID[prev])
    before = w.ops.calls
    got = w.play(case)
    assert got == [case["expect"]] * case["repeat"]
    assert w.ops.calls - before == case["store_reads"]
    assert w.rate.peek(case["src"]) == case["tokens"]


# ---------------------------------------------------------------- 표 밖 규칙

def spend_n(rate: RateTable, src: str, n: int) -> None:
    for _ in range(n):
        rate.spend(src)


def rule_spend_keeps_refill_time():
    w = World()
    w.rate.spend(A)  # 0초. 항목 생성, 9
    w.clock.t = T0 + 5.0
    w.rate.spend(A)  # 5초. 8. 보충 기준은 0초 그대로
    w.clock.t = T0 + 6.0
    w.rate.exhausted(A)  # 6초. 기준 0초에서 6초 -> 1 보충
    assert w.rate.peek(A) == 9


def rule_refill_from_refill_time():
    w = World()
    spend_n(w.rate, A, RATE_LIMIT_BUCKET)  # 0초. 0
    w.clock.t = T0 + 11.0
    assert w.rate.exhausted(A) is False and w.rate.peek(A) == 1  # 6초 지점의 보충. 기준은 6초
    w.clock.t = T0 + 12.0
    w.rate.exhausted(A)
    assert w.rate.peek(A) == 2  # 기준 6초에서 6초 -> 12초에 1 더


def rule_not_dropped_before_60s():
    w = World()
    spend_n(w.rate, A, RATE_LIMIT_BUCKET)
    w.clock.t = T0 + 59.9
    assert w.rate.exhausted(A) is False
    assert w.rate.peek(A) == RATE_LIMIT_BUCKET - 1


def rule_lookup_does_not_create():
    w = World()
    assert w.rate.exhausted(A) is False
    assert len(w.rate) == 0


def rule_table_full_passes_new_source():
    w = World()
    for i in range(MAX_RATE_ENTRIES):
        w.rate.spend(f"10.0.{i // 256}.{i % 256}")
    assert len(w.rate) == MAX_RATE_ENTRIES
    spend_n(w.rate, B, RATE_LIMIT_BUCKET + 1)
    assert w.rate.exhausted(B) is False and w.rate.peek(B) is NO_ENTRY
    assert w.counters["rate_table_full"] == RATE_LIMIT_BUCKET + 1
    assert w.rate.peek("10.0.0.0") == RATE_LIMIT_BUCKET - 1  # 가장 오래된 항목을 밀어내지 않았다
    assert len(w.rate) == MAX_RATE_ENTRIES


def rule_table_full_drops_refilled_entries():
    w = World()
    for i in range(MAX_RATE_ENTRIES):
        w.rate.spend(f"10.0.{i // 256}.{i % 256}")
    w.clock.t = T0 + 60.0  # 모든 항목이 9 -> 10 에 닿을 시각. 그 출발지들은 다시 오지 않았다
    w.rate.spend(B)
    assert w.counters["rate_table_full"] == 0
    assert w.rate.peek(B) == RATE_LIMIT_BUCKET - 1


def rule_counted_errors():
    # 6.4: 세는 것은 room_not_found, room_expired, unauthorized 셋이다. 다른 오류는 세지 않는다.
    for code, counted in [("room_not_found", True), ("room_expired", True), ("unauthorized", True),
                          ("room_full", False), ("unknown_op", False)]:
        w = World()

        def ops(req, code=code):
            raise OpError(code)

        w.ops = ops
        w.send(A, *PEERS_OK)
        assert (w.rate.peek(A) == RATE_LIMIT_BUCKET - 1) is counted, code


def rule_rate_limited_counter():
    w = World()
    spend_n(w.rate, A, RATE_LIMIT_BUCKET)
    assert w.send(A, *PEERS_OK) == "rate_limited"
    assert w.counters["rate_limited"] == 1


def rule_internal_error_hides_detail():
    # 7.2: 예외는 500 internal. 예외 내용을 본문에 싣지 않는다. internal_error 카운터.
    w = World()

    def ops(req):
        raise RuntimeError("SECRET-DETAIL")

    w.ops = ops
    raw = b'POST /v1/get_peers HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}'

    async def go():
        reader = asyncio.StreamReader()
        reader.feed_data(raw)
        return await handle_request(reader, A, rate=w.rate, dispatch=ops, counters=w.counters, read_timeout_s=1.0)

    out = asyncio.run(go())
    assert out.startswith(b"HTTP/1.1 500 ")
    assert b"SECRET" not in out
    assert json.loads(out.partition(b"\r\n\r\n")[2])["error"] == "internal"
    assert w.counters["internal_error"] == 1 and len(w.rate) == 0


def rule_table_full_sweep_drops_all_refilled():
    # 앞 절반은 0초에 1회 소모(9), 뒤 절반은 30초에 10회 소모(0). 60초에 앞 절반은 상한에 닿고 뒤 절반은 5다.
    w = World()
    half = MAX_RATE_ENTRIES // 2
    early = [f"10.0.{i // 256}.{i % 256}" for i in range(half)]
    late = [f"10.1.{i // 256}.{i % 256}" for i in range(half)]
    for src in early:
        w.rate.spend(src)
    w.clock.t = T0 + 30.0
    for src in late:
        spend_n(w.rate, src, RATE_LIMIT_BUCKET)
    assert len(w.rate) == MAX_RATE_ENTRIES
    w.clock.t = T0 + 60.0
    w.rate.spend(B)
    assert w.counters["rate_table_full"] == 0
    assert len(w.rate) == half + 1
    assert all(w.rate.peek(src) is NO_ENTRY for src in early)
    assert all(w.rate.peek(src) == 5 for src in late)
    assert w.rate.peek(B) == RATE_LIMIT_BUCKET - 1


def rule_parse_error_before_budget():
    # 7.2: 예산 검사는 파싱 뒤다. 소진된 출발지가 보낸 잘못된 HTTP 는 rate_limited 가 아니라 파싱 오류다.
    w = World()
    spend_n(w.rate, A, RATE_LIMIT_BUCKET)
    assert w.rate.exhausted(A) is True
    assert w.send_raw(A, b"POST /v1/get_peers HTTP/1.1\r\nContent-Length: 2\r\n\r\n[]") == "bad_request"
    assert w.send_raw(A, b"GET /v1/get_peers HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}") == "method_not_allowed"
    assert w.counters["rate_limited"] == 0 and w.ops.calls == 0
    assert w.rate.peek(A) == 0


RULE_CASES = [
    dict(id="spend-keeps-refill-time", fn=rule_spend_keeps_refill_time,
         note="6.4 '보충 시각의 기준은 보충이 일어난 시각이다. 토큰 소모도 그 시각을 바꾸지 않는다'"),
    dict(id="refill-from-refill-time", fn=rule_refill_from_refill_time,
         note="6.4 보충: 연속형. 마지막 보충 시각으로부터 6초가 지날 때마다 1개. 늦게 관측해도 보충 시각은 "
              "6초 지점이다 (6.4 '기준 시각은 6초씩 나아간다')"),
    dict(id="not-dropped-before-60s", fn=rule_not_dropped_before_60s,
         note="6.4 '경계 자체를 보려면 60초 직전에는 지워지지 않는다를 별도 행으로 둔다'"),
    dict(id="lookup-does-not-create", fn=rule_lookup_does_not_create,
         note="6.4 항목 생성: 토큰을 쓸 때만 만든다. 조회만 하고 만들지 않는다"),
    dict(id="table-full-passes-new-source", fn=rule_table_full_passes_new_source,
         note="6.4 표가 찼을 때: 새 출발지를 제한하지 않고 통과시킨다. rate_table_full 을 올린다. 밀어내지 않는다"),
    dict(id="table-full-drops-refilled", fn=rule_table_full_drops_refilled_entries,
         note="6.4 항목 제거: 보충으로 상한에 닿으면 지운다. 다시 오지 않는 출발지도 지워야 표가 단조 증가하지 "
              "않는다. 표가 찼을 때 훑는다 (6.4 항목 제거)"),
    dict(id="table-full-sweep-drops-all", fn=rule_table_full_sweep_drops_all_refilled,
         note="6.4 항목 제거: 표가 찼을 때 훑으면 상한에 닿은 항목은 전부 지우고 아직 닿지 않은 항목은 남긴다"),
    dict(id="parse-error-before-budget", fn=rule_parse_error_before_budget,
         note="6.4 예산 검사 시점은 파싱 뒤. 7.2 의 순서 (read_request 다음 rate.exhausted)"),
    dict(id="counted-errors", fn=rule_counted_errors,
         note="6.4 세는 것은 room_not_found, room_expired, unauthorized 셋이다"),
    dict(id="rate-limited-counter", fn=rule_rate_limited_counter,
         note="7.2 rate_limited 응답마다 카운터 rate_limited"),
    dict(id="internal-error-hides-detail", fn=rule_internal_error_hides_detail,
         note="7.2 except Exception: internal_error 카운터, 500 internal, 예외 내용을 싣지 않는다"),
]


@pytest.mark.parametrize("case", RULE_CASES, ids=[c["id"] for c in RULE_CASES])
def test_rate_rules(case):
    case["fn"]()
