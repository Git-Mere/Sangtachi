"""control_plane.md 4장 연산 다섯을 ops.Service 로 돌린다. 저장소는 DynamoDB local 이다.

표는 이 파일이 출처다. 행을 고치려면 여기를 고친다. 변이는 tests/mutants/ops.py.

store 표시가 있는 시험은 --store 를 줄 때만 돈다. 표시 없는 것(FIELDS)은 저장소를 부르면 실패하는 가짜
저장소로 돈다. 형식 검사가 저장소 읽기보다 먼저라는 7.3 의 규칙을 그 가짜가 본다.

시각은 주입한다. now_ms, mono_ns, boot_id 를 Clock 이 준다. 유예와 임대를 실제로 기다리지 않는다.
쓰기 순서는 store.on_write 의 "before" 에서 다른 연산을 끼워 넣어 만든다.
"""

import hmac
import threading
import time
from dataclasses import dataclass

import pytest

from conftest import isolated_session, no_proxy_config
from controlplane import ids, log
from controlplane import store as S
from controlplane.constants import (
    JOIN_REGISTER_GRACE_S,
    MAX_PEER_ID_ATTEMPTS,
    MAX_PEERS,
    MAX_ROOM_ID_ATTEMPTS,
    PUNCH_DELAY_MS,
    ROOM_LEASE_S,
)
from controlplane.errors import OpError
from controlplane.ops import Service

# 시각은 실제 벽시계 근처여야 한다. DynamoDB local 은 TTL 삭제를 실제로 돌린다 (test_store.py 와 같다).
NOW = (time.time_ns() // 1_000_000 // 1000) * 1000 + 999
LEASE = ROOM_LEASE_S * 1000
G = JOIN_REGISTER_GRACE_S * 1000
MONO = 7_000_000_000_000   # 주입한 단조 시계의 시작값 (ns)

TOKEN = "0" * 32            # 형식만 맞는 토큰. 어느 피어의 것도 아니다 (발급 토큰은 CSPRNG 이라 겹치지 않는다)
NONCES = [f"{i:032x}" for i in range(1, 40)]
CAND = [{"ip": "203.0.113.7", "port": 51000, "kind": "reflexive"}]
CAND_H = [{"ip": "198.51.100.20", "port": 40000, "kind": "reflexive"},
          {"ip": "192.168.0.10", "port": 40000, "kind": "local"}]
CAND_2 = [{"ip": "203.0.113.99", "port": 52000, "kind": "reflexive"}]
POOL = [f"10.100.0.{n}" for n in range(2, MAX_PEERS + 1)]


@dataclass(frozen=True)
class Req:
    """7.2 의 req. ops 는 .op 와 .body 만 본다 (server.py 를 import 하지 않는다)."""
    op: str
    body: object


class Clock:
    def __init__(self):
        self.now = NOW
        self.mono = MONO
        self.boot = "boot-a"


def make_service(st, clk):
    return Service(st, now_ms=lambda: clk.now, mono_ns=lambda: clk.mono, boot_id=lambda: clk.boot)


def code_of(call):
    try:
        call()
    except OpError as exc:
        return exc.code
    return "ok"


class Env:
    """서비스 하나와 그 저장소, 시계, 로그, 쓰기 기록."""

    def __init__(self, st, lines):
        self.store = st
        self.logs = lines
        self.clock = Clock()
        self.writes = []      # 시도한 쓰기의 (label, sk). "before" 마다 하나
        self.hooks = []
        st.on_write = self._hook
        self.svc = make_service(st, self.clock)
        self._nonce = iter(NONCES)

    def _hook(self, phase, label, sk):
        if phase == "before":
            self.writes.append((label, sk))
        for hook in list(self.hooks):
            hook(phase, label, sk)

    def once(self, label, action, phase="before"):
        """label 의 첫 phase 에서 action 을 한 번 돌린다. action 안의 쓰기는 다시 끼어들지 않는다."""
        state = {"done": False}

        def hook(ph, lab, sk):
            if ph == phase and lab == label and not state["done"]:
                state["done"] = True
                action()
        self.hooks.append(hook)
        return state

    def nonce(self):
        return next(self._nonce)

    def call(self, op, **body):
        return self.svc.dispatch(Req(op, body))

    def outcome(self, op, **body):
        try:
            return self.call(op, **body)
        except OpError as exc:
            return exc.code

    # 연산
    def create(self, nonce=None):
        return self.call("create_room", client_nonce=nonce or self.nonce())

    def join(self, room, nonce=None):
        return self.call("join_room", room_id=room["room_id"], client_nonce=nonce or self.nonce())

    def register(self, who, room=None, cands=CAND):
        return self.call("register_candidate", room_id=(room or who)["room_id"], peer_id=who["peer_id"],
                         peer_token=who["peer_token"], candidates=cands)

    def get_peers(self, who):
        return self.call("get_peers", room_id=who["room_id"], peer_id=who["peer_id"], peer_token=who["peer_token"])

    def report(self, host, departed=None, confirm=None):
        body = dict(room_id=host["room_id"], peer_id=host["peer_id"], peer_token=host["peer_token"])
        if departed is not None:
            body["departed"] = departed
        if confirm is not None:
            body["confirm"] = confirm
        return self.call("host_report", **body)

    # 저장소 직접 읽기
    def raw(self, pk, sk):
        resp = self.store.client.get_item(TableName=self.store.table, Key={"pk": {"S": pk}, "sk": {"S": sk}},
                                          ConsistentRead=True)
        return self.store._plain(resp["Item"]) if "Item" in resp else None

    def sks(self, room_id):
        return sorted(self.store.query_room(room_id).sks)

    def events(self, name):
        return [line for line in self.logs if line.split(" ")[1] == name]

    def labels(self, label):
        return [sk for lab, sk in self.writes if lab == label]


@pytest.fixture
def logs():
    lines = []
    log.set_sink(lines.append)
    try:
        yield lines
    finally:
        log.set_sink(None)


@pytest.fixture
def env(store, logs):
    return Env(store, logs)


def absent_room_id(env, *rooms):
    """만든 방과 겹치지 않는 형식상 맞는 room_id."""
    for rid in ("ZZZZZZ", "YYYYYY", "XXXXXX"):
        if rid not in {r["room_id"] for r in rooms}:
            return rid
    raise AssertionError("빈 room_id 를 찾지 못했다")


def pair_of(env, room, a, b):
    """그 쌍의 PAIR# 항목. ttl 은 뺀다. 갱신이 ttl 만 민다 (6.3 host_report 갱신)."""
    lo, hi = sorted((a["peer_id"], b["peer_id"]))
    item = env.raw(room["room_id"], f"PAIR#{lo}-{hi}")
    return None if item is None else {k: v for k, v in item.items() if k != "ttl"}


# ---------------------------------------------------------------------------
# 출처: control_plane.md 4.2~4.6 의 요청 표, 3.3 HTTP 부분집합의 인코딩 행(정수는 JSON number 이고 소수점
# 없음, uint32 는 0 이상 4294967295 이하), 2.1 room_id, 2.3 peer_token, 2.4 client_nonce 의 형식, 4.4 의
# 거부 정책, 4.6 의 departed·confirm 길이(MAX_PEERS - 1 이하), 7.2 ops.dispatch 의 계약.
# 전부 7.3 의 1~3번에서 끝난다. 저장소를 부르지 않는다 (7.3 "형식 검사가 저장소 읽기보다 먼저다").
# set 은 유효한 본문에 덮어쓸 필드, drop 은 지울 필드다.
# ---------------------------------------------------------------------------
VALID = {
    "create_room": {"client_nonce": "a" * 32},
    "join_room": {"room_id": "QWERTY", "client_nonce": "a" * 32},
    "register_candidate": {"room_id": "QWERTY", "peer_id": 7, "peer_token": TOKEN, "candidates": CAND},
    "get_peers": {"room_id": "QWERTY", "peer_id": 7, "peer_token": TOKEN},
    "host_report": {"room_id": "QWERTY", "peer_id": 7, "peer_token": TOKEN, "departed": [], "confirm": []},
}
TOO_MANY = list(range(1, MAX_PEERS + 1))   # MAX_PEERS 개. 상한은 MAX_PEERS - 1

FIELDS = [
    {"id": "create-nonce-missing", "op": "create_room", "drop": "client_nonce",
     "note": "4.2 요청 표. client_nonce 는 필수다"},
    {"id": "create-nonce-upper", "op": "create_room", "set": {"client_nonce": "A" * 32},
     "note": "2.4. 서버는 소문자 16진수 32자만 받는다. 대문자가 섞이면 bad_request"},
    {"id": "create-nonce-short", "op": "create_room", "set": {"client_nonce": "a" * 31},
     "note": "2.4. 길이가 다르면 bad_request"},
    {"id": "create-nonce-int", "op": "create_room", "set": {"client_nonce": 12345},
     "note": "4.2 요청 표. 16진수 32자 문자열이다"},
    {"id": "join-room-id-missing", "op": "join_room", "drop": "room_id", "note": "4.3 요청 표. room_id 는 필수다"},
    {"id": "join-room-id-alphabet", "op": "join_room", "set": {"room_id": "QWERT0"},
     "note": "2.1. 알파벳에 없는 글자는 오류다. 닮은 글자로 바꿔 해석하지 않는다"},
    {"id": "join-room-id-not-str", "op": "join_room", "set": {"room_id": 123456},
     "note": "2.1 케이스 표 밖 행. 문자열이 아니면 bad_request"},
    {"id": "join-nonce-missing", "op": "join_room", "drop": "client_nonce", "note": "4.3 요청 표. 필수"},
    {"id": "register-peer-id-missing", "op": "register_candidate", "drop": "peer_id",
     "note": "4.4 요청 표. peer_id 는 필수다"},
    {"id": "register-peer-id-negative", "op": "register_candidate", "set": {"peer_id": -1},
     "note": "3.3 인코딩. uint32 는 0 이상"},
    {"id": "register-peer-id-over", "op": "register_candidate", "set": {"peer_id": 2**32},
     "note": "3.3 인코딩. uint32 는 4294967295 이하"},
    {"id": "register-peer-id-bool", "op": "register_candidate", "set": {"peer_id": True},
     "note": "JSON true 는 정수가 아니다. Python 에서 bool 은 int 의 하위형이다"},
    {"id": "register-peer-id-float", "op": "register_candidate", "set": {"peer_id": 7.0},
     "note": "3.3 인코딩. 정수는 소수점 없음"},
    {"id": "register-peer-id-str", "op": "register_candidate", "set": {"peer_id": "7"},
     "note": "정수는 JSON number 다"},
    {"id": "register-token-upper", "op": "register_candidate", "set": {"peer_token": "A" * 32},
     "note": "2.3. 형식이 아니면 토큰을 비교하지 않고 bad_request 다"},
    {"id": "register-token-missing", "op": "register_candidate", "drop": "peer_token", "note": "4.4 요청 표. 필수"},
    {"id": "register-candidates-missing", "op": "register_candidate", "drop": "candidates",
     "note": "4.4 요청 표. 필수"},
    {"id": "register-candidates-empty", "op": "register_candidate", "set": {"candidates": []},
     "note": "4.4 거부 정책. 목록이 비면 요청 전체가 bad_request"},
    {"id": "register-candidates-all-rejected", "op": "register_candidate",
     "set": {"candidates": [{"ip": "127.0.0.1", "port": 51000, "kind": "local"}]},
     "note": "4.4. 전부 위생 거부되어 저장할 후보가 0개면 bad_request 다. 저장소 전에 끝난다"},
    {"id": "register-room-id-missing", "op": "register_candidate", "drop": "room_id", "note": "4.4 요청 표. 필수"},
    {"id": "get-peers-peer-id-over", "op": "get_peers", "set": {"peer_id": 2**32}, "note": "3.3 인코딩"},
    {"id": "get-peers-token-short", "op": "get_peers", "set": {"peer_token": "0" * 31}, "note": "2.3 형식"},
    {"id": "get-peers-room-id-missing", "op": "get_peers", "drop": "room_id", "note": "4.5 요청 표. 필수"},
    {"id": "host-report-peer-id-null", "op": "host_report", "set": {"peer_id": None}, "note": "4.6 요청 표. uint32"},
    {"id": "host-report-departed-not-list", "op": "host_report", "set": {"departed": "1"},
     "note": "4.6 요청 표. departed 는 배열이다"},
    {"id": "host-report-departed-null", "op": "host_report", "set": {"departed": None},
     "note": "4.6. 없으면 빈 배열이거나 생략이다. null 은 둘 다 아니다"},
    {"id": "host-report-departed-too-long", "op": "host_report", "set": {"departed": TOO_MANY},
     "note": "4.6 요청 표. 길이 MAX_PEERS - 1 이하"},
    {"id": "host-report-departed-elem-bool", "op": "host_report", "set": {"departed": [True]},
     "note": "원소는 peer_id(uint32) 다. bool 은 정수가 아니다"},
    {"id": "host-report-departed-elem-over", "op": "host_report", "set": {"departed": [2**32]},
     "note": "원소는 uint32 다"},
    {"id": "host-report-departed-elem-negative", "op": "host_report", "set": {"departed": [-1]},
     "note": "원소는 uint32 다"},
    {"id": "host-report-confirm-too-long", "op": "host_report", "set": {"confirm": TOO_MANY},
     "note": "4.6 요청 표. confirm 도 같은 제약"},
    {"id": "host-report-confirm-elem-str", "op": "host_report", "set": {"confirm": ["7"]},
     "note": "원소는 uint32 정수다"},
    {"id": "host-report-confirm-elem-float", "op": "host_report", "set": {"confirm": [7.0]},
     "note": "정수는 소수점 없음"},
    {"id": "host-report-confirm-not-list", "op": "host_report", "set": {"confirm": {"7": 1}},
     "note": "confirm 은 배열이다"},
    {"id": "dispatch-body-not-object", "op": "create_room", "body": ["a" * 32],
     "note": "3.3 본문은 JSON 객체 하나다. 7.2 ops.dispatch 가 받는 req.body"},
    {"id": "dispatch-unknown-op", "op": "leave_room", "expect": "unknown_op",
     "note": "4.1 unknown_op. 4장의 연산이 아니다. leave_room 은 없다 (4장 머리)"},
]


class NoStore:
    """부르면 실패하는 저장소. 7.3 의 1~3번이 저장소 전에 끝나는지 본다."""

    def __getattr__(self, name):
        raise AssertionError(f"형식 오류인데 저장소를 불렀다: {name}")


def fields_body(case):
    if "body" in case:
        return case["body"]
    body = dict(VALID.get(case["op"], {}))
    body.pop(case.get("drop"), None)
    body.update(case.get("set", {}))
    return body


@pytest.mark.parametrize("case", FIELDS, ids=[c["id"] for c in FIELDS])
def test_fields(case):
    svc = make_service(NoStore(), Clock())
    with pytest.raises(OpError) as err:
        svc.dispatch(Req(case["op"], fields_body(case)))
    assert err.value.code == case.get("expect", "bad_request")


def test_valid_bodies_reach_the_store():
    # 위 표의 기준 본문은 형식이 맞다. 그래서 각 행의 bad_request 는 그 행이 바꾼 필드에서 나온 것이다.
    for op, body in VALID.items():
        svc = make_service(NoStore(), Clock())
        with pytest.raises(AssertionError, match="저장소를 불렀다"):
            svc.dispatch(Req(op, dict(body)))


# ---------------------------------------------------------------------------
# 위 표의 반대쪽. 형식의 경계 안쪽 값이 bad_request 가 아닌지 본다. 저장소가 있어야 돈다.
# 출처는 위 표와 같고, 3.3 의 "알 수 없는 키는 무시한다", 4.3 응답의 "정규화된 값", 4.6 의 "생략" 이다.
# expect 는 결과 코드다. 경계 값이 형식을 통과하면 그 피어가 없으므로 unauthorized 다 (4.6 왜 이렇게 정했나).
# ---------------------------------------------------------------------------
FIELD_ACCEPT = [
    {"id": "peer-id-uint32-max", "op": "register_candidate", "set": {"peer_id": 2**32 - 1}, "expect": "unauthorized",
     "note": "uint32 의 상한 4294967295 는 형식이 맞다. 없는 피어라 unauthorized"},
    {"id": "peer-id-zero", "op": "get_peers", "set": {"peer_id": 0}, "expect": "unauthorized",
     "note": "uint32 의 하한 0 은 형식이 맞다. 2.2 가 발급하지 않는 값이라 없는 피어다"},
    {"id": "departed-max-length", "op": "host_report", "set": {"departed": TOO_MANY[:-1]}, "expect": "ok",
     "note": "길이 MAX_PEERS - 1 은 받는다. 없는 peer_id 는 released 에 담지 않고 오류가 아니다"},
    {"id": "confirm-max-length", "op": "host_report", "set": {"confirm": TOO_MANY[:-1]}, "expect": "ok",
     "note": "길이 MAX_PEERS - 1 은 받는다"},
    {"id": "departed-confirm-omitted", "op": "host_report", "drop": ("departed", "confirm"), "expect": "ok",
     "note": "4.6. 없으면 빈 배열이거나 생략"},
    {"id": "unknown-keys-ignored", "op": "create_room", "set": {"x_future": {"a": 1}}, "expect": "ok",
     "note": "3.3 본문. 알 수 없는 키는 무시한다 (전방 호환)"},
    {"id": "room-id-lowercase", "op": "join_room", "lower": True, "expect": "ok",
     "note": "2.1 입력은 대소문자를 무시한다. 4.3 응답의 room_id 는 정규화된 값이다"},
    {"id": "candidates-partly-rejected", "op": "register_candidate",
     "set": {"candidates": [{"ip": "127.0.0.1", "port": 1, "kind": "local"}, CAND[0], dict(CAND[0], kind="local")]},
     "expect": "ok",
     "note": "4.4 거부 정책과 응답. 위생 거부는 그 후보만 버리고 rejected 를 올린다. 중복은 rejected 에 세지 않고 "
             "먼저 온 것을 남긴다. accepted 는 저장한 후보 수다"},
]


@pytest.mark.store
@pytest.mark.parametrize("case", FIELD_ACCEPT, ids=[c["id"] for c in FIELD_ACCEPT])
def test_field_accept(env, case):
    host = env.create()
    body = {"create_room": {"client_nonce": env.nonce()},
            "join_room": {"room_id": host["room_id"], "client_nonce": env.nonce()},
            "register_candidate": {"room_id": host["room_id"], "peer_id": host["peer_id"],
                                   "peer_token": host["peer_token"], "candidates": CAND},
            "get_peers": {"room_id": host["room_id"], "peer_id": host["peer_id"], "peer_token": host["peer_token"]},
            "host_report": {"room_id": host["room_id"], "peer_id": host["peer_id"], "peer_token": host["peer_token"],
                            "departed": [], "confirm": []}}[case["op"]]
    for name in case.get("drop", ()):
        body.pop(name)
    body.update(case.get("set", {}))
    if case.get("lower"):
        body["room_id"] = host["room_id"].lower()
    result = env.outcome(case["op"], **body)
    assert (result if isinstance(result, str) else "ok") == case["expect"]
    if case["id"] == "room-id-lowercase":
        assert result["room_id"] == host["room_id"]
    if case["id"] == "candidates-partly-rejected":
        assert result == {"accepted": 1, "rejected": 1}
        assert env.raw(host["room_id"], f"PEER#{host['peer_id']}")["candidates"] == CAND
    if case["id"] == "departed-max-length":
        assert result["released"] == []
        assert env.labels("release_peer") == []   # 읽은 방에 없는 대상은 쓰지 않는다


# ---------------------------------------------------------------------------
# 출처: control_plane.md 5.1 방, 연산별 허용 상태 표 (연산 | open | expired | (없음)). 칸 하나가 한 행이다.
# join_room 의 open 칸은 두 결과를 적었으므로 두 행이다.
# expired 는 5.1 만료 판정의 경계 now == expires_at_ms 로 만든다. 경계는 만료 쪽이다.
# expect 는 결과 코드다. 성공의 세부는 시험 본문이 행 id 로 본다.
# ---------------------------------------------------------------------------
ALLOWED = [
    {"id": "create-open", "op": "create_room", "state": "open", "cell": "같은 nonce 면 기존 방(4.2)", "expect": "ok",
     "note": "이미 만든 방의 같은 응답. expires_in_s 만 줄어든다. 새 방을 만들지 않는다"},
    {"id": "create-expired", "op": "create_room", "state": "expired", "cell": "같은 nonce 면 `room_expired`(4.2)",
     "expect": "room_expired", "note": "그 방이 만료됐으면 room_expired 다. 새 방을 만들지 않는다"},
    {"id": "create-absent", "op": "create_room", "state": "absent", "cell": "생성", "expect": "ok",
     "note": "방을 만들고 호스트를 첫 피어로 등록하고 10.100.0.1 을 준다"},
    {"id": "join-open-claim", "op": "join_room", "state": "open", "cell": "선점 시도", "expect": "ok",
     "note": "풀의 낮은 주소부터 VIP# 를 선점한다"},
    {"id": "join-open-full", "op": "join_room", "state": "open", "cell": "빈 주소가 없으면 `room_full`",
     "expect": "room_full", "note": "풀을 한 바퀴 돌아 빈 주소가 하나도 없을 때만"},
    {"id": "join-expired", "op": "join_room", "state": "expired", "cell": "`room_expired`", "expect": "room_expired",
     "note": "만료된 방은 참가할 수 없다. TTL 삭제를 기다리지 않는다 (저장 4)"},
    {"id": "join-absent", "op": "join_room", "state": "absent", "cell": "`room_not_found`",
     "expect": "room_not_found", "note": "만료된 방과 다른 코드다 (4.1)"},
    {"id": "register-open", "op": "register_candidate", "state": "open", "cell": "저장", "expect": "ok",
     "note": "후보 목록을 통째로 교체한다"},
    {"id": "register-expired", "op": "register_candidate", "state": "expired", "cell": "`room_expired`",
     "expect": "room_expired", "note": ""},
    {"id": "register-absent", "op": "register_candidate", "state": "absent", "cell": "`room_not_found`",
     "expect": "room_not_found", "note": ""},
    {"id": "get-peers-open", "op": "get_peers", "state": "open", "cell": "쌍별 준비 여부를 담아 응답", "expect": "ok",
     "note": "not_ready 는 오류가 아니다. ok 에 ready: false"},
    {"id": "get-peers-expired", "op": "get_peers", "state": "expired", "cell": "`room_expired`",
     "expect": "room_expired", "note": ""},
    {"id": "get-peers-absent", "op": "get_peers", "state": "absent", "cell": "`room_not_found`",
     "expect": "room_not_found", "note": ""},
    {"id": "host-report-open", "op": "host_report", "state": "open", "cell": "회수·확인·갱신", "expect": "ok",
     "note": "갱신 뒤 남은 임대는 정상이면 ROOM_LEASE_S 다"},
    {"id": "host-report-expired", "op": "host_report", "state": "expired", "cell": "`room_expired`. 갱신하지 않는다",
     "expect": "room_expired", "note": "만료된 방은 되살리지 못한다. 임대가 끊긴 방은 끝난 방이다"},
    {"id": "host-report-absent", "op": "host_report", "state": "absent", "cell": "`room_not_found`",
     "expect": "room_not_found", "note": ""},
]


@pytest.mark.store
@pytest.mark.parametrize("case", ALLOWED, ids=[c["id"] for c in ALLOWED])
def test_allowed(env, case):
    host_nonce = env.nonce()
    host = env.create(host_nonce)
    player = env.join(host)
    env.clock.now = NOW + 30_000
    if case["id"] == "join-open-full":
        for _ in POOL[1:]:
            env.join(host)
    if case["state"] == "expired":
        env.clock.now = NOW + LEASE   # now == expires_at_ms
    room_id = host["room_id"] if case["state"] != "absent" else absent_room_id(env, host)
    env.writes.clear()
    op = case["op"]
    if op == "create_room":
        nonce = host_nonce if case["state"] != "absent" else env.nonce()
        result = env.outcome(op, client_nonce=nonce)
    elif op == "join_room":
        result = env.outcome(op, room_id=room_id, client_nonce=env.nonce())
    elif op == "register_candidate":
        result = env.outcome(op, room_id=room_id, peer_id=player["peer_id"], peer_token=player["peer_token"],
                             candidates=CAND)
    elif op == "get_peers":
        result = env.outcome(op, room_id=room_id, peer_id=player["peer_id"], peer_token=player["peer_token"])
    else:
        result = env.outcome(op, room_id=room_id, peer_id=host["peer_id"], peer_token=host["peer_token"])
    assert (result if isinstance(result, str) else "ok") == case["expect"]
    if case["expect"] != "ok":
        assert env.writes == [] or op == "join_room" and case["id"] == "join-open-full"
    rid = case["id"]
    if rid == "create-open":
        assert result == dict(host, expires_in_s=ROOM_LEASE_S - 30)
        assert env.labels("create_room") == []
    elif rid == "create-absent":
        assert result["virtual_ip"] == "10.100.0.1" and result["expires_in_s"] == ROOM_LEASE_S
        assert result["room_id"] != host["room_id"]
    elif rid == "join-open-claim":
        assert result["virtual_ip"] == POOL[1] and result["expires_in_s"] == ROOM_LEASE_S - 30
    elif rid == "register-open":
        assert result == {"accepted": 1, "rejected": 0}
    elif rid == "get-peers-open":
        assert result == {"ready": False, "peers": [{"peer_id": host["peer_id"], "virtual_ip": "10.100.0.1",
                                                      "ready": False}]}
    elif rid == "host-report-open":
        assert result["expires_in_s"] == ROOM_LEASE_S
    elif rid == "host-report-expired":
        assert env.raw(host["room_id"], "ROOM")["expires_at_ms"] == NOW + LEASE


# ---------------------------------------------------------------------------
# 출처: control_plane.md 7.3 연산 처리 순서. 복합 결함의 응답을 순서가 정한다.
#   "만료가 토큰보다 먼저다. 만료된 방에 토큰이 틀린 요청은 room_expired 다"
#   "토큰 비교는 ... 먼저 PEER# 를 읽어 애플리케이션에서 상수 시간 비교를 한다" (6.3, 쓰기 전)
#   "host_report 는 호출자가 호스트인지도 본다" (7번, 4.6 호출자 판정)
#   4.6 왜 이렇게 정했나: 없는 peer_id 로 부르면 unauthorized 다
#   6.3 읽기 표의 join_room: NONCE# GetItem 이 ROOM GetItem 보다 먼저다. 저장된 room_id 가 요청과 다르면
#   bad_request 다
# writes 는 그 요청이 시도한 쓰기의 수다.
# ---------------------------------------------------------------------------
ORDER = [
    {"id": "format-before-store", "op": "register_candidate", "room": "absent", "token": "bad-format",
     "expect": "bad_request", "writes": 0, "note": "형식 검사가 저장소 읽기보다 먼저다. 없는 방이어도 bad_request"},
    {"id": "absent-before-token", "op": "register_candidate", "room": "absent", "token": "wrong",
     "expect": "room_not_found", "writes": 0, "note": "5번 방 존재가 7번 토큰보다 먼저다"},
    {"id": "expired-before-token-register", "op": "register_candidate", "room": "expired", "token": "wrong",
     "expect": "room_expired", "writes": 0, "note": "만료가 토큰보다 먼저다"},
    {"id": "expired-before-token-get-peers", "op": "get_peers", "room": "expired", "token": "wrong",
     "expect": "room_expired", "writes": 0, "note": "만료가 토큰보다 먼저다"},
    {"id": "expired-before-token-host-report", "op": "host_report", "room": "expired", "token": "wrong",
     "expect": "room_expired", "writes": 0, "note": "만료가 토큰보다 먼저다"},
    {"id": "token-before-write-register", "op": "register_candidate", "room": "open", "token": "wrong",
     "expect": "unauthorized", "writes": 0,
     "note": "애플리케이션의 상수 시간 비교가 쓰기보다 먼저다. 조건식의 peer_token = :t 에 맡기지 않는다"},
    {"id": "token-before-write-host-report", "op": "host_report", "room": "open", "token": "wrong",
     "expect": "unauthorized", "writes": 0, "note": "호출자 판정이 1번이다. 회수·확인·갱신을 하지 않는다"},
    {"id": "token-wrong-get-peers", "op": "get_peers", "room": "open", "token": "wrong", "expect": "unauthorized",
     "writes": 0, "note": "토큰이 틀리면 상대 정보를 주지 않는다"},
    {"id": "host-check", "op": "host_report", "room": "open", "token": "player", "expect": "unauthorized",
     "writes": 0, "note": "호스트가 아닌 피어가 자기 토큰으로 부르면 unauthorized. 회수하지 않는다"},
    {"id": "unknown-peer-register", "op": "register_candidate", "room": "open", "token": "unknown-peer",
     "expect": "unauthorized", "writes": 0, "note": "없는 peer_id 와 토큰이 틀린 peer_id 를 같은 코드로 답한다"},
    {"id": "unknown-peer-get-peers", "op": "get_peers", "room": "open", "token": "unknown-peer",
     "expect": "unauthorized", "writes": 0, "note": "같음"},
    {"id": "unknown-peer-host-report", "op": "host_report", "room": "open", "token": "unknown-peer",
     "expect": "unauthorized", "writes": 0, "note": "같음"},
    {"id": "nonce-before-room", "op": "join_room", "room": "absent", "token": "other-room-nonce",
     "expect": "bad_request", "writes": 0,
     "note": "NONCE# 읽기가 ROOM 읽기보다 먼저다. 다른 방의 nonce 면 없는 방이어도 bad_request"},
    {"id": "nonce-other-open-room", "op": "join_room", "room": "open", "token": "other-room-nonce",
     "expect": "bad_request", "writes": 0, "note": "nonce 를 다른 방에 재사용한 것이다"},
]


@pytest.mark.store
@pytest.mark.parametrize("case", ORDER, ids=[c["id"] for c in ORDER])
def test_order(env, case):
    host = env.create()
    player = env.join(host)
    env.register(player)
    env.register(host)
    other_nonce = env.nonce()
    other = env.create(other_nonce)   # 다른 방. 그 호스트의 nonce 를 이 방에 쓴다
    if case["room"] == "expired":
        env.clock.now = NOW + LEASE
    room_id = host["room_id"] if case["room"] != "absent" else absent_room_id(env, host, other)
    who = host if case["op"] == "host_report" else player
    peer_id, token = who["peer_id"], who["peer_token"]
    kind = case["token"]
    if kind == "bad-format":
        token = "X" * 32
    elif kind == "wrong":
        token = TOKEN
    elif kind == "player":
        peer_id, token = player["peer_id"], player["peer_token"]
    elif kind == "unknown-peer":
        peer_id = 4_000_000_000 if who["peer_id"] != 4_000_000_000 else 4_000_000_001
    env.writes.clear()
    if case["op"] == "join_room":
        result = env.outcome("join_room", room_id=room_id, client_nonce=other_nonce)
    elif case["op"] == "register_candidate":
        result = env.outcome("register_candidate", room_id=room_id, peer_id=peer_id, peer_token=token, candidates=CAND_2)
    elif case["op"] == "get_peers":
        result = env.outcome("get_peers", room_id=room_id, peer_id=peer_id, peer_token=token)
    else:
        result = env.outcome("host_report", room_id=room_id, peer_id=peer_id, peer_token=token,
                             departed=[player["peer_id"]])
    assert result == case["expect"]
    assert len(env.writes) == case["writes"]
    assert env.raw(host["room_id"], f"PEER#{player['peer_id']}")["candidates"] == CAND


# ---------------------------------------------------------------------------
# 출처: control_plane.md 6.3 항목, 트랜잭션 취소 사유 표의 동작 열 (순서 | 실패한 조건 | 동작). 표의 판정
# (어느 사유를 먼저 보나)은 store.classify_cancel 이고 그 표는 test_store.py 의 CANCEL_ORDER 다. 여기는
# 그 판정을 받은 ops 가 하는 일이다. 표 아래 첫 불릿(create_room 은 1번 다음에 5번)도 한 행이다.
# 행마다 시도한 트랜잭션의 수(attempts)를 본다. 재추첨 상한과 풀 순회가 그 수로 드러난다.
# ---------------------------------------------------------------------------
CANCEL_ACTIONS = [
    {"id": "nonce-join", "order": "1", "failed": "`NONCE#`",
     "note": "다른 사유가 무엇이든 같은 nonce 의 요청이 먼저 성립한 것이다. NONCE# 를 다시 읽어 같은 응답을 "
             "돌려준다. room_full 이나 재추첨으로 가지 않는다"},
    {"id": "nonce-join-other-room", "order": "1", "failed": "`NONCE#`",
     "note": "다시 읽은 room_id 가 요청의 방과 다르면 bad_request 다. 사전 읽기 경로가 같은 입력에 내는 답과 같다"},
    {"id": "nonce-create", "order": "1", "failed": "`NONCE#`", "note": "create_room 도 같다 (4.2 멱등성)"},
    {"id": "nonce-then-room-put-create", "order": "1, 5", "failed": "`NONCE#` 와 `ROOM` put",
     "note": "create_room 은 1번 다음에 5번을 본다. room_id 가 부딪혀도 nonce 가 먼저다. 재추첨하지 않는다"},
    {"id": "room-check-absent", "order": "2", "failed": "`ROOM` ConditionCheck (`join_room`)",
     "note": "방을 다시 읽는다. 없으면 room_not_found. 다음 주소로 넘어가지 않는다"},
    {"id": "room-check-expired", "order": "2", "failed": "`ROOM` ConditionCheck (`join_room`)",
     "note": "있으면 room_expired. 다음 주소로 넘어가지 않는다"},
    {"id": "vip-next", "order": "3", "failed": "`VIP#`", "note": "다음 주소"},
    {"id": "vip-pool-end", "order": "3", "failed": "`VIP#`", "note": "풀이 끝나면 room_full. 무한히 돌지 않는다"},
    {"id": "peer-redraw", "order": "4", "failed": "`PEER#`", "note": "peer_id 재추첨"},
    {"id": "peer-cap", "order": "4", "failed": "`PEER#`", "note": "상한은 MAX_PEER_ID_ATTEMPTS. 넘기면 internal"},
    {"id": "room-put-redraw", "order": "5", "failed": "`ROOM` put (`create_room`)", "note": "room_id 재추첨"},
    {"id": "room-put-cap", "order": "5", "failed": "`ROOM` put (`create_room`)",
     "note": "상한은 MAX_ROOM_ID_ATTEMPTS. 넘기면 internal (2.1)"},
    {"id": "vip-race", "order": "3", "failed": "`VIP#`",
     "note": "다음 주소. 결정적 경합 판이다. 이 요청이 주소를 비었다고 본 뒤 트랜잭션 직전에 다른 참가가 그 주소를 "
             "먼저 선점한다 (리뷰 b3. 동시 시험 대신 변이가 기대는 행)"},
    {"id": "peer-budget-per-request", "order": "3, 4", "failed": "`PEER#` 와 `VIP#` 를 섞어",
     "note": "peer_id 재추첨 상한 MAX_PEER_ID_ATTEMPTS 는 요청 하나의 예산이다. 다음 주소로 넘어가도 새로 주지 않는다. "
             "2.2 는 '시도 상한은 4' 라고만 적었다. 요청 단위로 읽은 것은 구현의 선택이다 (보고서)"},
    {"id": "unknown-result", "order": "-", "failed": "조건 실패가 아닌 사유",
     "note": "internal. 판정은 store 가 하고 ops 는 모르는 결과를 판정 불가로 본다. 재시도하지 않는다"},
]


def draws(values):
    """ids 의 생성 함수를 대신하는 함수. values 를 차례로 내고 끝나면 마지막 값을 계속 낸다."""
    it = iter(values)
    last = []

    def draw():
        try:
            last[:] = [next(it)]
        except StopIteration:
            pass
        return last[0]
    return draw


@pytest.mark.store
@pytest.mark.parametrize("case", CANCEL_ACTIONS, ids=[c["id"] for c in CANCEL_ACTIONS])
def test_cancel_actions(env, case, monkeypatch):
    rid = case["id"]
    host = env.create()
    room_id = host["room_id"]
    env.writes.clear()
    if rid == "nonce-join":
        nonce = env.nonce()
        inner = {}
        env.once("join_room", lambda: inner.update(env.join(host, nonce)))
        result = env.join(host, nonce)
        assert result == inner
        assert result["virtual_ip"] == POOL[0]
        assert env.raw(room_id, f"VIP#{POOL[1]}") is None
        assert env.labels("join_room") == [f"VIP#{POOL[0]}", f"VIP#{POOL[0]}"]
    elif rid == "nonce-join-other-room":
        other = env.create()
        nonce = env.nonce()
        env.once("join_room", lambda: env.join(other, nonce))
        assert code_of(lambda: env.join(host, nonce)) == "bad_request"
    elif rid == "nonce-create":
        nonce = env.nonce()
        inner = {}
        env.once("create_room", lambda: inner.update(env.create(nonce)))
        result = env.create(nonce)
        assert result == inner
        assert len(env.labels("create_room")) == 2
    elif rid == "nonce-then-room-put-create":
        nonce = env.nonce()
        monkeypatch.setattr(ids, "new_room_id", lambda: "QWERTZ")
        inner = {}
        env.once("create_room", lambda: inner.update(env.create(nonce)))
        result = env.create(nonce)
        assert result == inner and result["room_id"] == "QWERTZ"
        assert len(env.labels("create_room")) == 2
    elif rid in ("room-check-absent", "room-check-expired"):
        if rid == "room-check-absent":
            action = lambda: env.store.client.delete_item(  # noqa: E731
                TableName=env.store.table, Key={"pk": {"S": room_id}, "sk": {"S": "ROOM"}})
        else:
            action = lambda: env.store.client.update_item(  # noqa: E731
                TableName=env.store.table, Key={"pk": {"S": room_id}, "sk": {"S": "ROOM"}},
                UpdateExpression="SET expires_at_ms = :e", ExpressionAttributeValues={":e": {"N": str(NOW)}})
        env.once("join_room", action)
        expect = "room_not_found" if rid == "room-check-absent" else "room_expired"
        assert code_of(lambda: env.join(host)) == expect
        assert len(env.labels("join_room")) == 1
    elif rid == "vip-next":
        env.join(host)
        env.writes.clear()
        assert env.join(host)["virtual_ip"] == POOL[1]
        assert env.labels("join_room") == [f"VIP#{POOL[0]}", f"VIP#{POOL[1]}"]
    elif rid == "vip-pool-end":
        for _ in POOL:
            env.join(host)
        env.writes.clear()
        assert code_of(lambda: env.join(host)) == "room_full"
        assert env.labels("join_room") == [f"VIP#{ip}" for ip in POOL]
    elif rid == "peer-redraw":
        monkeypatch.setattr(ids, "new_peer_id", draws([host["peer_id"], 77]))
        result = env.join(host)
        assert result["peer_id"] == 77 and result["virtual_ip"] == POOL[0]
        assert env.labels("join_room") == [f"VIP#{POOL[0]}", f"VIP#{POOL[0]}"]
    elif rid == "peer-cap":
        monkeypatch.setattr(ids, "new_peer_id", lambda: host["peer_id"])
        assert code_of(lambda: env.join(host)) == "internal"
        assert len(env.labels("join_room")) == MAX_PEER_ID_ATTEMPTS
        assert env.raw(room_id, f"VIP#{POOL[0]}") is None
    elif rid == "room-put-redraw":
        monkeypatch.setattr(ids, "new_room_id", draws([room_id, "QWERTZ"]))
        result = env.create()
        assert result["room_id"] == "QWERTZ"
        assert len(env.labels("create_room")) == 2
    elif rid == "room-put-cap":
        monkeypatch.setattr(ids, "new_room_id", lambda: room_id)
        assert code_of(lambda: env.create()) == "internal"
        assert len(env.labels("create_room")) == MAX_ROOM_ID_ATTEMPTS
    elif rid == "vip-race":
        env.once("join_room", lambda: env.join(host))
        result = env.join(host)
        assert result["virtual_ip"] == POOL[1]
        assert env.labels("join_room") == [f"VIP#{POOL[0]}", f"VIP#{POOL[0]}", f"VIP#{POOL[1]}"]
    elif rid == "peer-budget-per-request":
        # 첫 시도: .2 는 비었고 PEER# 가 부딪힌다 -> 재추첨(예산 2). 둘째 시도 직전에 .2 를 남이 선점한다.
        # 둘째 시도: .2 와 PEER# 가 함께 실패하고 3번이 이긴다 -> .3. 그 뒤 .3 에서 PEER# 만 부딪힌다.
        monkeypatch.setattr(ids, "new_peer_id", lambda: host["peer_id"])
        seen = {"n": 0}

        def occupy(phase, label, sk):
            if phase == "before" and label == "join_room":
                seen["n"] += 1
                if seen["n"] == 2:
                    env.store.client.put_item(TableName=env.store.table, Item=env.store._values(
                        {"pk": room_id, "sk": f"VIP#{POOL[0]}", "peer_id": 99, "ttl": 2_000_000_000}))
        env.hooks.append(occupy)
        assert code_of(lambda: env.join(host)) == "internal"
        assert env.labels("join_room") == [f"VIP#{POOL[0]}", f"VIP#{POOL[0]}"] + [f"VIP#{POOL[1]}"] * 3
        assert len(env.labels("join_room")) == MAX_PEER_ID_ATTEMPTS + 1   # VIP 실패 한 번은 예산을 쓰지 않는다
    elif rid == "unknown-result":
        calls = []

        def odd(*args, **kwargs):
            calls.append(args)
            return "something_else"
        monkeypatch.setattr(env.store, "join_room", odd)
        assert code_of(lambda: env.join(host)) == "internal"
        assert len(calls) == 1
    else:
        pytest.fail(f"모르는 행 {rid}")


# ---------------------------------------------------------------------------
# 출처: roadmap.md Phase 3 검증의 "기본 동작" 과 "저장 계약과 동시성" 가운데 연산이 로컬에서 판정할 수
# 있는 불릿. 규칙은 그 불릿이 가리키는 control_plane.md 의 절(2.5, 4.2~4.6, 5.1, 5.2, 6.1, 7.4, 7.5)이 갖는다.
# note 는 그 불릿이다. 확인 경합 표와 서버 회수 표의 store 행은 test_host_report.py 에 있다.
# 배포 인스턴스가 있어야 하는 것(단조 시계가 재시작에 이어지는가, ConsistentRead 는 코드 읽기)은 여기 없다.
# ---------------------------------------------------------------------------
SCENARIOS = [
    {"id": "vip-host-and-first-player", "note": "방 생성자가 10.100.0.1, 참가자가 10.100.0.2 를 받음. 로그의 "
     "room.created 와 vip.claimed 가 그 주소를 싣는다 (7.5)"},
    {"id": "join-twice-same-nonce", "note": "같은 client_nonce 로 join_room 을 두 번 부르면 두 번째가 첫 번째와 "
     "같은 응답을 돌려주고 VIP# 항목이 하나만 생긴다"},
    {"id": "join-new-nonce-new-seat", "note": "다른 client_nonce 로 부르면 새 참가다. 새 peer_id 와 다른 가상 IP 를 "
     "받는다"},
    {"id": "create-twice-same-nonce", "note": "create_room 도 같은 nonce 면 같은 방이다"},
    {"id": "get-peers-content", "note": "get_peers 응답에 상대의 가상 IP 와 공인 엔드포인트가 포함됨 (호스트의 확인 뒤)"},
    {"id": "not-ready-before-confirm", "note": "호스트가 확인하기 전에는 get_peers 가 그 쌍을 ready: false 로 주고 상대 "
     "후보를 싣지 않는다"},
    {"id": "restart-reads-store", "note": "서버 재시작 후에도 방 상태가 유지됨. 복원 절차 없이 재시작 직후 첫 요청이 "
     "저장소에서 바로 읽는다 (6.1 저장 5)"},
    {"id": "expiry-boundary", "note": "만료 시각이 지난 방은 room_expired, 없는 방은 room_not_found. now == "
     "expires_at_ms 경계가 만료 쪽이다. 1ms 전에는 참가된다"},
    {"id": "departed-reassign", "note": "호스트가 departed 로 알린 피어의 PEER# 와 VIP# 가 사라지고, 그 가상 IP 를 "
     "다음 참가자가 받는다. 로그의 peer.released 가 by=host 다"},
    {"id": "departed-unknown", "note": "이미 없는 peer_id 를 알리면 released 가 빈 배열이고 오류가 아니다"},
    {"id": "replay-peer-gone", "note": "호스트가 departed 로 회수한 피어가 같은 nonce 로 join_room 을 다시 보내면 새 "
     "참가다. departed 회수도 그 피어의 NONCE# 를 지운다(서버 회수와 같다. 디렉터 결정). 4.6 서버 회수 표의 "
     "join-retry-with-reclaimed-nonce 행과 같은 결과다"},
    {"id": "departed-confirm-race", "note": "4.6 2번이 3번보다 먼저다. departed 와 confirm 이 같은 peer_id 를 담으면 "
     "회수가 이긴다. 그 사이 다른 요청이 먼저 지워 이 요청의 released 에 없어도 3번이 그 피어를 다시 판정하지 "
     "않는다 (디렉터 결정, 리뷰 b1)"},
    {"id": "departed-race-released-once", "note": "4.6 released 는 실제로 지운 것이다. 읽은 뒤 지우기 전에 다른 요청이 "
     "먼저 지웠으면 담지 않는다"},
    {"id": "server-reclaim-pool-refill", "note": "join_room 만 하고 후보를 등록하지 않은 플레이어 넷으로 풀을 채운다. "
     "유예가 지난 뒤의 host_report 가 넷을 released 에 담고, 그 뒤의 join_room 이 room_full 이 아니다. "
     "peer.released 가 by=server 다"},
    {"id": "register-after-reclaim", "note": "register_candidate 가 토큰 검사를 통과한 뒤 쓰기 전에 회수가 먼저 커밋되면 "
     "unauthorized 이고, 지워진 PEER# 가 되살아나지 않는다"},
    {"id": "reclaim-skipped-logged", "note": "판정할 수 없는 피어는 지우지 않고 건너뛰고 peer.reclaim_skipped 를 WARN "
     "으로 남긴다. 임대 갱신은 막히지 않는다"},
    {"id": "lease-expiry", "note": "host_report 를 멈추면 ROOM_LEASE_S 안에 방이 만료되고 그 뒤의 join_room 이 "
     "room_expired 다. 만료된 방에 host_report 를 보내면 room_expired 이고 되살아나지 않는다"},
    {"id": "lease-renew", "note": "host_report 한 번이 만료 시각을 now + ROOM_LEASE_S 로 민다. 응답의 expires_in_s 는 "
     "갱신 뒤의 값이다 (4.6 의 4번이 5번보다 먼저). room.renewed 가 나온다"},
    {"id": "renew-skips-removed", "note": "4.6 갱신의 항목 목록은 첫 Query 결과에서 2번이 지운 항목을 뺀 것이다"},
    {"id": "response-after-writes", "note": "4.6 5번이 마지막이다. 방금 회수한 피어가 peers 에 없고 방금 확인한 쌍이 "
     "ready: true 로 나간다"},
    {"id": "reregister-keeps-baseline", "note": "준비 완료 뒤 어느 피어가 후보를 다시 등록해도 punch_delay_ms 와 "
     "elapsed_since_ready_ms 의 기준점이 바뀌지 않는다"},
    {"id": "late-joiner", "note": "늦은 참가. 이미 한 쌍이 준비된 방에 새 플레이어가 들어와도 기존 쌍의 기준점이 바뀌지 "
     "않고, 새 쌍만 따로 준비 완료가 된다"},
    {"id": "departed-before-reclaim", "note": "4.6 처리 순서 2번. departed 회수가 서버 회수보다 먼저다. 후보 없이 유예가 "
     "지난 참가자를 호스트가 departed 로 알리면 by=host 이고 서버 회수를 시도하지 않는다 (리뷰 b3)"},
    {"id": "departed-race-no-reclaim", "note": "위 행의 경합 판. 다른 요청이 먼저 지워 이 요청의 released 에 없어도, departed "
     "의 대상은 서버 회수가 다시 판정하지 않는다 (리뷰 b3)"},
    {"id": "elapsed-wall-fallback", "note": "7.4. boot_id 가 다르면 벽시계로 대체하고 elapsed_wall_fallback 을 "
     "올린다. 같은 부팅이면 단조 시계로 계산하고 올리지 않는다"},
    {"id": "host-calls-get-peers", "note": "4.5 는 호스트가 get_peers 를 부르지 않는다고만 적었다. 보고서의 DOC GAP. "
     "지금 구현은 4.6 응답과 같이 자신을 뺀 피어 전부를 준다"},
    {"id": "host-in-own-departed-confirm", "note": "4.6 은 departed 나 confirm 에 호출자 자신이 들어온 경우를 정하지 "
     "않았다. 보고서의 DOC GAP. 지금 구현은 그 원소를 건너뛴다"},
    {"id": "logs-no-secrets", "note": "서버 로그에 room_id 와 peer_token 이 없다 (7.5). 정상 경로와 오류 경로를 "
     "밟은 뒤 로그 전체에서 두 값을 찾는다. 0 건이어야 한다"},
]


def ready_pair(env):
    """호스트와 플레이어 하나. 둘 다 등록하고 호스트가 확인했다."""
    host = env.create()
    player = env.join(host)
    env.register(host, cands=CAND_H)
    env.register(player)
    env.report(host, confirm=[player["peer_id"]])
    return host, player


def run_threads(n, target):
    barrier = threading.Barrier(n)
    results = [None] * n
    errors = []

    def worker(i):
        barrier.wait()
        try:
            results[i] = target(i)
        except Exception as exc:  # noqa: BLE001 - 시험이 판정한다
            errors.append(exc)
    threads = [threading.Thread(target=worker, args=(i,)) for i in range(n)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(60)
    assert not errors, errors
    return results


def retry_internal(call, attempts=5, base_s=0.05, cap_s=0.4):
    """8.3 의 클라이언트처럼 internal 이면 같은 요청을 다시 보낸다. 동시 트랜잭션은 TransactionConflict
    로 internal 이 될 수 있다 (6.3 취소 사유 표의 마지막 행). 상한 있는 지수 백오프다. 스트레스 시험만 쓴다."""
    for attempt in range(attempts):
        try:
            return call()
        except OpError as exc:
            if exc.code != "internal":
                return exc.code
        time.sleep(min(cap_s, base_s * 2 ** attempt))
    return "internal"


@pytest.mark.store
@pytest.mark.parametrize("case", SCENARIOS, ids=[c["id"] for c in SCENARIOS])
def test_scenario(env, case):
    rid = case["id"]
    if rid == "vip-host-and-first-player":
        host = env.create()
        player = env.join(host)
        assert host["virtual_ip"] == "10.100.0.1" and player["virtual_ip"] == "10.100.0.2"
        assert env.events("room.created") == [f"INFO room.created peer_id={host['peer_id']} virtual_ip=10.100.0.1"]
        assert env.events("vip.claimed") == [f"INFO vip.claimed virtual_ip=10.100.0.2 peer_id={player['peer_id']}"]
    elif rid == "join-twice-same-nonce":
        host = env.create()
        nonce = env.nonce()
        first = env.join(host, nonce)
        env.clock.now += 5_000
        env.writes.clear()
        second = env.join(host, nonce)
        assert second == dict(first, expires_in_s=first["expires_in_s"] - 5)
        assert env.writes == []
        assert [sk for sk in env.sks(host["room_id"]) if sk.startswith("VIP#")] == ["VIP#10.100.0.1", "VIP#10.100.0.2"]
    elif rid == "join-new-nonce-new-seat":
        host = env.create()
        first = env.join(host)
        second = env.join(host)
        assert second["peer_id"] != first["peer_id"] and second["virtual_ip"] == "10.100.0.3"
        assert env.raw(host["room_id"], f"PEER#{first['peer_id']}") is not None
    elif rid == "create-twice-same-nonce":
        nonce = env.nonce()
        first = env.create(nonce)
        env.clock.now += 1_000
        env.writes.clear()
        assert env.create(nonce) == dict(first, expires_in_s=ROOM_LEASE_S - 1)
        assert env.writes == []
        assert env.events("room.created") == [env.events("room.created")[0]]
    elif rid == "get-peers-content":
        host, player = ready_pair(env)
        env.clock.mono += 250_000_000
        got = env.get_peers(player)
        assert got == {"ready": True, "peers": [{
            "peer_id": host["peer_id"], "virtual_ip": "10.100.0.1", "ready": True, "punch_delay_ms": PUNCH_DELAY_MS,
            "elapsed_since_ready_ms": 250, "candidates": CAND_H}]}
    elif rid == "not-ready-before-confirm":
        host = env.create()
        player = env.join(host)
        env.register(host, cands=CAND_H)
        env.register(player)
        assert env.get_peers(player) == {"ready": False, "peers": [
            {"peer_id": host["peer_id"], "virtual_ip": "10.100.0.1", "ready": False}]}
        assert env.raw(host["room_id"], f"PAIR#{min(host['peer_id'], player['peer_id'])}-"
                       f"{max(host['peer_id'], player['peer_id'])}") is None
    elif rid == "restart-reads-store":
        host, player = ready_pair(env)
        before = env.get_peers(player)
        fresh_store = S.Store.from_env(config=no_proxy_config(), session=isolated_session())
        assert fresh_store.client is not env.store.client
        fresh = make_service(fresh_store, env.clock)
        assert fresh.dispatch(Req("get_peers", {"room_id": player["room_id"], "peer_id": player["peer_id"],
                                                "peer_token": player["peer_token"]})) == before
        assert fresh.dispatch(Req("join_room", {"room_id": host["room_id"], "client_nonce": env.nonce()}))[
            "virtual_ip"] == "10.100.0.3"
    elif rid == "expiry-boundary":
        host = env.create()
        missing = absent_room_id(env, host)
        assert env.outcome("join_room", room_id=missing, client_nonce=env.nonce()) == "room_not_found"
        env.clock.now = NOW + LEASE
        assert code_of(lambda: env.join(host)) == "room_expired"
        env.clock.now = NOW + LEASE - 1
        assert env.join(host)["expires_in_s"] == 1
    elif rid == "departed-reassign":
        host = env.create()
        player = env.join(host)
        env.register(player)
        q = env.join(host)
        result = env.report(host, departed=[player["peer_id"]])
        assert result["released"] == [player["peer_id"]] and result["confirmed"] == []
        assert env.raw(host["room_id"], f"PEER#{player['peer_id']}") is None
        assert env.raw(host["room_id"], "VIP#10.100.0.2") is None
        assert env.raw(q["room_id"], f"PEER#{q['peer_id']}") is not None
        assert env.events("peer.released") == [
            f"INFO peer.released peer_id={player['peer_id']} virtual_ip=10.100.0.2 by=host"]
        assert env.join(host)["virtual_ip"] == "10.100.0.2"
    elif rid == "departed-unknown":
        host = env.create()
        env.writes.clear()
        result = env.report(host, departed=[4_000_000_000])
        assert result["released"] == [] and env.labels("release_peer") == []
    elif rid == "replay-peer-gone":
        host = env.create()
        nonce = env.nonce()
        player = env.join(host, nonce)
        env.join(host)
        assert env.report(host, departed=[player["peer_id"]])["released"] == [player["peer_id"]]
        assert env.raw(f"NONCE#{nonce}", "NONCE") is None
        again = env.join(host, nonce)
        assert again["peer_id"] != player["peer_id"] and again["peer_token"] != player["peer_token"]
        assert again["virtual_ip"] == player["virtual_ip"]   # 2.5 배정 순서. 비었으므로 같은 값이 나온다
        assert env.raw(f"NONCE#{nonce}", "NONCE")["peer_id"] == again["peer_id"]
    elif rid == "departed-confirm-race":
        host = env.create()
        player = env.join(host)
        env.register(host, cands=CAND_H)
        env.register(player)
        env.once("release_peer", lambda: env.report(host, departed=[player["peer_id"]]))
        outer = env.report(host, departed=[player["peer_id"]], confirm=[player["peer_id"]])
        assert outer["released"] == [] and outer["confirmed"] == [] and outer["peers"] == []
        assert env.labels("confirm_pair") == []
        assert env.events("pair.ready") == []
    elif rid == "departed-race-released-once":
        host = env.create()
        player = env.join(host)
        inner = {}
        env.once("release_peer", lambda: inner.update(env.report(host, departed=[player["peer_id"]])))
        outer = env.report(host, departed=[player["peer_id"]])
        assert inner["released"] == [player["peer_id"]]
        assert outer["released"] == []
        assert len(env.events("peer.released")) == 1
    elif rid == "server-reclaim-pool-refill":
        host = env.create()
        nonces = [env.nonce() for _ in POOL]
        players = [env.join(host, n) for n in nonces]
        assert code_of(lambda: env.join(host)) == "room_full"
        env.clock.now = NOW + G
        result = env.report(host)
        assert sorted(result["released"]) == sorted(p["peer_id"] for p in players)
        assert result["peers"] == []
        assert sorted(env.events("peer.released")) == sorted(
            f"INFO peer.released peer_id={p['peer_id']} virtual_ip={p['virtual_ip']} by=server" for p in players)
        for n in nonces:   # NONCE# 도 같이 지운다 (4.6 서버 회수)
            assert env.raw(f"NONCE#{n}", "NONCE") is None
        assert env.join(host)["virtual_ip"] == "10.100.0.2"
    elif rid == "register-after-reclaim":
        host = env.create()
        player = env.join(host)
        env.clock.now = NOW + G
        env.once("register_candidates", lambda: env.report(host))
        assert code_of(lambda: env.register(player)) == "unauthorized"
        assert env.raw(host["room_id"], f"PEER#{player['peer_id']}") is None
        assert env.events("peer.released") == [
            f"INFO peer.released peer_id={player['peer_id']} virtual_ip=10.100.0.2 by=server"]
    elif rid == "reclaim-skipped-logged":
        host = env.create()
        player = env.join(host)
        env.store.client.update_item(
            TableName=env.store.table, Key={"pk": {"S": host["room_id"]}, "sk": {"S": f"PEER#{player['peer_id']}"}},
            UpdateExpression="REMOVE joined_at_ms")
        env.clock.now = NOW + G
        assert code_of(lambda: env.report(host)) == "internal"   # 5번 다시 읽기는 판정 불가다 (DOC GAP)
        assert env.events("peer.reclaim_skipped") == [
            f"WARN peer.reclaim_skipped peer_id={player['peer_id']} reason=joined_at_unreadable"]
        assert env.raw(host["room_id"], f"PEER#{player['peer_id']}") is not None
        assert env.raw(host["room_id"], "ROOM")["expires_at_ms"] == NOW + G + LEASE
    elif rid == "lease-expiry":
        host = env.create()
        env.clock.now = NOW + LEASE
        assert code_of(lambda: env.join(host)) == "room_expired"
        assert code_of(lambda: env.report(host)) == "room_expired"
        assert env.raw(host["room_id"], "ROOM")["expires_at_ms"] == NOW + LEASE
        assert env.events("room.renewed") == []
        env.clock.now += 1
        assert code_of(lambda: env.join(host)) == "room_expired"
    elif rid == "lease-renew":
        host = env.create()
        env.clock.now = NOW + 60_000
        result = env.report(host)
        assert result["expires_in_s"] == ROOM_LEASE_S
        assert env.raw(host["room_id"], "ROOM")["expires_at_ms"] == NOW + 60_000 + LEASE
        assert env.events("room.renewed") == [
            f"INFO room.renewed host_peer_id={host['peer_id']} expires_in_s={ROOM_LEASE_S}"]
        env.clock.now = NOW + LEASE + 1
        assert env.join(host)["virtual_ip"] == "10.100.0.2"
    elif rid == "renew-skips-removed":
        host = env.create()
        player = env.join(host)
        env.register(player)
        env.register(host)
        env.report(host, confirm=[player["peer_id"]])
        env.writes.clear()
        env.report(host, departed=[player["peer_id"]])
        lo, hi = sorted((host["peer_id"], player["peer_id"]))
        renewed = env.labels("renew_item")
        assert f"PEER#{player['peer_id']}" not in renewed and "VIP#10.100.0.2" not in renewed
        assert f"PAIR#{lo}-{hi}" not in renewed
        assert sorted(renewed) == sorted([f"PEER#{host['peer_id']}", "VIP#10.100.0.1"])
    elif rid == "response-after-writes":
        host = env.create()
        gone = env.join(host)
        player = env.join(host)
        env.register(host)
        env.register(player)
        result = env.report(host, departed=[gone["peer_id"]], confirm=[player["peer_id"]])
        assert result["released"] == [gone["peer_id"]] and result["confirmed"] == [player["peer_id"]]
        assert [p["peer_id"] for p in result["peers"]] == [player["peer_id"]]
        assert result["peers"][0]["ready"] is True and result["peers"][0]["candidates"] == CAND
    elif rid == "reregister-keeps-baseline":
        host, player = ready_pair(env)
        pair = pair_of(env, host, host, player)
        env.clock.now += 3_000
        env.clock.mono += 3_000_000_000
        env.register(player, cands=CAND_2)
        env.register(host, cands=CAND_2)
        env.report(host, confirm=[player["peer_id"]])
        assert pair_of(env, host, host, player) == pair
        got = env.get_peers(player)["peers"][0]
        assert got["elapsed_since_ready_ms"] == 3_000 and got["punch_delay_ms"] == PUNCH_DELAY_MS
        assert got["candidates"] == CAND_2
        assert len(env.events("pair.ready")) == 1
    elif rid == "late-joiner":
        host, player = ready_pair(env)
        pair = pair_of(env, host, host, player)
        env.clock.now += 10_000
        env.clock.mono += 10_000_000_000
        late = env.join(host)
        env.register(late, cands=CAND_2)
        result = env.report(host, confirm=[late["peer_id"]])
        assert result["confirmed"] == [late["peer_id"]]
        assert pair_of(env, host, host, player) == pair
        assert pair_of(env, host, host, late)["ready_at_mono_ns"] == env.clock.mono
        assert env.get_peers(player)["peers"] == [dict(env.get_peers(player)["peers"][0], peer_id=host["peer_id"])]
        assert env.get_peers(player)["peers"][0]["elapsed_since_ready_ms"] == 10_000
        assert env.get_peers(late)["peers"][0]["elapsed_since_ready_ms"] == 0
        assert env.events("pair.ready") == [
            f"INFO pair.ready host_peer_id={host['peer_id']} peer_id={player['peer_id']} punch_delay_ms={PUNCH_DELAY_MS}",
            f"INFO pair.ready host_peer_id={host['peer_id']} peer_id={late['peer_id']} punch_delay_ms={PUNCH_DELAY_MS}"]
    elif rid == "departed-before-reclaim":
        host = env.create()
        player = env.join(host)
        env.clock.now = NOW + G
        env.writes.clear()
        result = env.report(host, departed=[player["peer_id"]])
        assert result["released"] == [player["peer_id"]]
        assert env.events("peer.released") == [
            f"INFO peer.released peer_id={player['peer_id']} virtual_ip={player['virtual_ip']} by=host"]
        assert env.labels("reclaim_peer") == []
    elif rid == "departed-race-no-reclaim":
        host = env.create()
        player = env.join(host)
        env.clock.now = NOW + G
        env.once("release_peer", lambda: env.report(host, departed=[player["peer_id"]]))
        env.writes.clear()
        outer = env.report(host, departed=[player["peer_id"]])
        assert outer["released"] == []
        assert env.labels("reclaim_peer") == []
        assert [line.rsplit(" ", 1)[1] for line in env.events("peer.released")] == ["by=host"]
    elif rid == "elapsed-wall-fallback":
        host, player = ready_pair(env)
        env.clock.now += 2_000
        env.clock.mono += 500_000_000
        assert env.get_peers(player)["peers"][0]["elapsed_since_ready_ms"] == 500
        assert env.svc.counters() == {"elapsed_wall_fallback": 0}
        env.clock.boot = "boot-b"
        assert env.get_peers(player)["peers"][0]["elapsed_since_ready_ms"] == 2_000
        assert env.svc.counters() == {"elapsed_wall_fallback": 1}
    elif rid == "host-calls-get-peers":
        host = env.create()
        a = env.join(host)
        b = env.join(host)
        got = env.get_peers(host)
        assert got == {"ready": False, "peers": [
            {"peer_id": p["peer_id"], "virtual_ip": p["virtual_ip"], "ready": False}
            for p in sorted((a, b), key=lambda p: str(p["peer_id"]))]}
    elif rid == "host-in-own-departed-confirm":
        host = env.create()
        env.register(host)
        env.writes.clear()
        # 둘을 따로 부른다. 한 요청에 담으면 departed 의 대상이라 3번이 건너뛰어 confirm 쪽을 볼 수 없다.
        result = env.report(host, departed=[host["peer_id"]])
        assert result["released"] == [] and result["confirmed"] == []
        result = env.report(host, confirm=[host["peer_id"]])
        assert result["released"] == [] and result["confirmed"] == []
        assert env.labels("release_peer") == [] and env.labels("confirm_pair") == []
        assert env.raw(host["room_id"], f"PEER#{host['peer_id']}") is not None
    elif rid == "logs-no-secrets":
        host, player = ready_pair(env)
        other = env.join(host)
        secrets_ = [host["room_id"], host["room_id"].lower(), host["peer_token"], player["peer_token"],
                    other["peer_token"]]
        env.get_peers(player)
        env.report(host, departed=[other["peer_id"]])
        env.clock.now += G
        lonely = env.join(host)
        env.report(host)
        for call in (lambda: env.join(host, env.nonce()),
                     lambda: env.call("get_peers", room_id=host["room_id"], peer_id=player["peer_id"], peer_token=TOKEN),
                     lambda: env.call("host_report", room_id=host["room_id"], peer_id=player["peer_id"],
                                      peer_token=player["peer_token"]),
                     lambda: env.call("join_room", room_id=absent_room_id(env, host), client_nonce=env.nonce())):
            code_of(call)
        env.clock.now = NOW + G + 10 * LEASE
        code_of(lambda: env.report(host))
        secrets_.append(lonely["peer_token"])
        assert env.logs
        text = "\n".join(env.logs)
        assert [s for s in secrets_ if s in text] == []
    else:
        pytest.fail(f"모르는 행 {rid}")


# ---------------------------------------------------------------------------
# 출처: control_plane.md 6.3 항목, 쓰기 표의 host_report 갱신 행 (조건 열). 저장소 쪽 판정은 test_store.py
# 의 WRITES 가 본다. 여기는 그 결과를 받은 연산이 내는 것이다.
# ---------------------------------------------------------------------------
RENEW = [
    {"id": "room-condition-fails", "note": "ROOM 에 attribute_exists(pk) AND expires_at_ms > :now. 실패는 room_expired "
     "이고 나머지를 쓰지 않는다. 1번에서 읽을 때는 살아 있었고 갱신 직전에 만료됐다"},
    {"id": "item-deleted-meanwhile", "note": "나머지 항목은 각각 attribute_exists(pk) 다. 조건이 실패하면 그 사이 지워진 "
     "항목이므로 무시한다. 요청은 성공하고 지워진 항목을 되살리지 않는다"},
]


@pytest.mark.store
@pytest.mark.parametrize("case", RENEW, ids=[c["id"] for c in RENEW])
def test_renew(env, case):
    host = env.create()
    player = env.join(host)
    room_id = host["room_id"]
    env.clock.now = NOW + 60_000
    env.writes.clear()
    if case["id"] == "room-condition-fails":
        env.once("renew_room", lambda: env.store.client.update_item(
            TableName=env.store.table, Key={"pk": {"S": room_id}, "sk": {"S": "ROOM"}},
            UpdateExpression="SET expires_at_ms = :e", ExpressionAttributeValues={":e": {"N": str(env.clock.now)}}))
        assert code_of(lambda: env.report(host)) == "room_expired"
        assert env.labels("renew_item") == []
        assert env.events("room.renewed") == []
    else:
        vip_sk = f"VIP#{player['virtual_ip']}"
        env.once("renew_item", lambda: env.store.client.delete_item(
            TableName=env.store.table, Key={"pk": {"S": room_id}, "sk": {"S": vip_sk}}))
        result = env.report(host)
        assert result["expires_in_s"] == ROOM_LEASE_S
        assert vip_sk in env.labels("renew_item") and env.raw(room_id, vip_sk) is None


# ---------------------------------------------------------------------------
# 출처: control_plane.md 6.3 항목의 형 변환 문단. 항목 표의 속성이 없거나 형이 다르면 판정 불가이고
# internal 이다. 예외는 둘이다(candidates 없음은 빈 목록, 4.6 서버 회수는 그 피어만 건너뛴다).
# 아래 행은 그 규칙이 연산에서 어떻게 나오는가다. corrupt 는 지우는 속성이다.
# replay-peer-gone 은 문서가 정하지 않은 자리이고 보고서의 DOC GAP 이다. 지금 구현을 적어 둔다.
# ---------------------------------------------------------------------------
UNREADABLE = [
    {"id": "caller-unreadable", "op": "host_report", "corrupt": ("host", "peer_token"), "expect": "internal",
     "note": "호출자 판정은 서버 회수가 아니다. 읽지 못한 호스트 PEER# 를 없는 피어(unauthorized)로 바꾸지 않는다"},
    {"id": "departed-target-unreadable", "op": "host_report", "corrupt": ("player", "peer_token"),
     "expect": "internal", "note": "departed 회수도 서버 회수가 아니다. 판정 불가는 internal 이다"},
    {"id": "get-peers-unreadable", "op": "get_peers", "corrupt": ("other", "virtual_ip"), "expect": "internal",
     "note": "get_peers 의 Query 는 서버 회수가 아니다. 방의 다른 항목도 읽을 수 없으면 internal 이다"},
    {"id": "reclaim-peer-id-unreadable", "op": "host_report", "corrupt": ("other", "peer_id"), "expect": "internal",
     "note": "서버 회수는 건너뛰고 peer.reclaim_skipped(reason=peer_id_unreadable) 를 남긴다. peer_id 를 읽지 못했으므로 "
             "그 필드는 '-' 다. 응답을 만드는 5번 다시 읽기는 서버 회수가 아니라 internal 이다 (DOC GAP)"},
    {"id": "reclaim-peer-id-list", "op": "host_report", "set": ("other", "peer_id", {"L": [{"N": "5"}]}),
     "expect": "internal",
     "note": "peer_id 가 리스트다. 4.6 판정 순서 1번(정수인가)이 먼저다. 해시할 수 없는 값으로 멤버십을 보다 "
             "터지지 않고 peer.reclaim_skipped(peer_id_unreadable)를 남기고 임대를 갱신한다 (리뷰 b5)"},
    {"id": "reclaim-peer-id-map", "op": "host_report", "set": ("other", "peer_id", {"M": {"a": {"N": "5"}}}),
     "expect": "internal", "note": "peer_id 가 맵이다. 위 행과 같다"},
    {"id": "nonce-orphan", "op": "join_room", "corrupt": None, "expect": "internal",
     "note": "NONCE# 는 남아 있는데 가리키는 PEER# 가 없다(회수 두 경로 밖에서 생긴 경우. 시험은 PEER# 를 직접 지워 "
             "만든다). 처음 발급한 것을 줄 수 없다. 판정 불가로 본다 (디렉터 결정)"},
    {"id": "departed-confirm-nonce-unreadable", "op": "host_report", "corrupt": ("player", "client_nonce"),
     "expect": "ok",
     "note": "위 행에 confirm 을 더했다. 2번이 지운 피어를 3번이 1번의 낡은 값으로 다시 읽으면 읽을 수 없는 PEER# "
             "라 internal 로 끝나고 4번 갱신까지 못 간다. 회수가 이긴다(4.6). 3번은 그 피어를 건너뛴다 (리뷰 b1)"},
    {"id": "departed-nonce-unreadable", "op": "host_report", "corrupt": ("player", "client_nonce"), "expect": "ok",
     "note": "departed 회수에서 PEER# 의 client_nonce 만 읽지 못했다. PEER#·VIP#·PAIR# 는 지우고 NONCE# 는 "
             "건너뛴다. 피어를 남기는 것보다 낫다 (디렉터 결정)"},
]


@pytest.mark.store
@pytest.mark.parametrize("case", UNREADABLE, ids=[c["id"] for c in UNREADABLE])
def test_unreadable(env, case):
    host = env.create()
    nonce = env.nonce()
    player = env.join(host, nonce)
    other = env.join(host)
    room_id = host["room_id"]
    if case.get("set") is not None:
        whose, attr, value = case["set"]
        target = {"host": host, "player": player, "other": other}[whose]
        env.store.client.update_item(
            TableName=env.store.table, Key={"pk": {"S": room_id}, "sk": {"S": f"PEER#{target['peer_id']}"}},
            UpdateExpression="SET #a = :v", ExpressionAttributeNames={"#a": attr}, ExpressionAttributeValues={":v": value})
    if case.get("corrupt") is not None:
        whose, attr = case["corrupt"]
        target = {"host": host, "player": player, "other": other}[whose]
        env.store.client.update_item(
            TableName=env.store.table, Key={"pk": {"S": room_id}, "sk": {"S": f"PEER#{target['peer_id']}"}},
            UpdateExpression="REMOVE #a", ExpressionAttributeNames={"#a": attr})
    if case["id"] == "nonce-orphan":
        env.store.client.delete_item(TableName=env.store.table,
                                     Key={"pk": {"S": room_id}, "sk": {"S": f"PEER#{player['peer_id']}"}})
        assert code_of(lambda: env.join(host, nonce)) == case["expect"]
        return
    if case["id"] in ("departed-nonce-unreadable", "departed-confirm-nonce-unreadable"):
        confirm = [player["peer_id"]] if case["id"] == "departed-confirm-nonce-unreadable" else None
        env.clock.now = NOW + 30_000
        result = env.report(host, departed=[player["peer_id"]], confirm=confirm)
        assert result["confirmed"] == [] and result["expires_in_s"] == ROOM_LEASE_S
        assert env.raw(room_id, "ROOM")["expires_at_ms"] == NOW + 30_000 + LEASE
        assert env.events("room.renewed") != []
        assert result["released"] == [player["peer_id"]]
        assert env.raw(room_id, f"PEER#{player['peer_id']}") is None
        assert env.raw(room_id, f"VIP#{player['virtual_ip']}") is None
        assert env.raw(f"NONCE#{nonce}", "NONCE") is not None
        return
    env.clock.now = NOW + G
    if case["op"] == "get_peers":
        result = code_of(lambda: env.get_peers(player))
    else:
        result = code_of(lambda: env.report(host, departed=[player["peer_id"]]
                                            if case["id"] == "departed-target-unreadable" else []))
    assert result == case["expect"]
    if case["id"] in ("reclaim-peer-id-unreadable", "reclaim-peer-id-list", "reclaim-peer-id-map"):
        assert env.events("peer.reclaim_skipped") == ["WARN peer.reclaim_skipped peer_id=- reason=peer_id_unreadable"]
        assert env.raw(room_id, f"PEER#{other['peer_id']}") is not None
        assert env.raw(room_id, "ROOM")["expires_at_ms"] == NOW + G + LEASE   # 4번 갱신까지 갔다
        assert len(env.events("room.renewed")) == 1
    else:
        assert env.events("peer.released") == []

# ---------------------------------------------------------------------------
# 출처: control_plane.md 6.3 항목의 토큰 비교 문단. "토큰 비교는 상수 시간 비교(hmac.compare_digest)로 한다.
# 먼저 PEER# 를 읽어 애플리케이션에서 상수 시간 비교를 한다." 시간은 재지 않는다. 비교가 그 함수를 거치는지,
# 저장값과 받은 값을 그 함수에 넘기는지를 감시 함수로 본다. 토큰이 필요한 연산은 셋이다.
# ---------------------------------------------------------------------------
TOKEN_COMPARE = [
    {"id": "register-right", "op": "register_candidate", "right": True, "note": "맞는 토큰도 그 함수로 비교한다"},
    {"id": "register-wrong", "op": "register_candidate", "right": False, "note": "틀린 토큰은 그 함수가 거짓을 내서 거부한다"},
    {"id": "get-peers-right", "op": "get_peers", "right": True, "note": "같음"},
    {"id": "get-peers-wrong", "op": "get_peers", "right": False, "note": "같음"},
    {"id": "host-report-right", "op": "host_report", "right": True, "note": "호출자 판정의 토큰 검사 (4.6)"},
    {"id": "host-report-wrong", "op": "host_report", "right": False, "note": "같음"},
]


@pytest.mark.store
@pytest.mark.parametrize("case", TOKEN_COMPARE, ids=[c["id"] for c in TOKEN_COMPARE])
def test_token_compare(env, case, monkeypatch):
    host = env.create()
    player = env.join(host)
    who = host if case["op"] == "host_report" else player
    token = who["peer_token"] if case["right"] else TOKEN
    real = hmac.compare_digest
    calls = []

    def spy(a, b):
        calls.append((a, b))
        return real(a, b)
    monkeypatch.setattr(hmac, "compare_digest", spy)
    body = {"room_id": who["room_id"], "peer_id": who["peer_id"], "peer_token": token}
    if case["op"] == "register_candidate":
        body["candidates"] = CAND
    result = env.outcome(case["op"], **body)
    assert (not isinstance(result, str)) is case["right"]
    if not case["right"]:
        assert result == "unauthorized"
    mine = [c for c in calls if token.encode() in c]
    assert mine == [(who["peer_token"].encode(), token.encode())]


# ---------------------------------------------------------------------------
# 출처: control_plane.md 7.3 연산 처리 순서의 시각 문단. "시각은 요청을 시작할 때 한 번 읽는다. 만료, 회수,
# 갱신의 판정이 모두 같은 now 를 쓴다." 요청의 첫 저장소 읽기가 끝난 직후(읽기 감싸기)나 앞선 쓰기 직전
# (store.on_write before)에 주입한 시계를 앞으로 돌린다. 쓰기 메서드의 before 에서 돌리면 늦다. 인자는 그 전에
# 이미 계산됐다. 처음 now 를 쓰는 구현만 기대값을 낸다. 예외(ready_at_* 와 elapsed)는 7.4 시나리오가 본다.
# ---------------------------------------------------------------------------
REQUEST_NOW = [
    {"id": "renew", "note": "갱신의 :now 와 새 만료 시각과 응답의 expires_in_s 가 처음 now 를 쓴다. 시계를 원래 만료 "
     "뒤로 돌려도 갱신이 성공하고 만료 시각은 처음 now + ROOM_LEASE_S 다"},
    {"id": "reclaim", "note": "서버 회수 판정이 처음 now 를 쓴다. 유예 1ms 전의 요청이 도중에 시계가 넘어가도 지우지 않는다"},
    {"id": "join", "note": "새 참가 트랜잭션의 ROOM 조건(:now)과 joined_at_ms 가 처음 now 를 쓴다. 만료 1ms 전의 참가가 "
     "도중에 시계가 만료를 넘어가도 성립한다"},
    {"id": "create", "note": "방의 created_at_ms 와 처음 만료 시각이 처음 now 를 쓴다"},
]


def jump_after_read(monkeypatch, env, name, to):
    """env.store.name 의 첫 호출이 끝난 직후 시계를 to 로 돌린다."""
    real = getattr(env.store, name)
    state = {"done": False}

    def wrapped(*args, **kwargs):
        out = real(*args, **kwargs)
        if not state["done"]:
            state["done"] = True
            env.clock.now = to
        return out
    monkeypatch.setattr(env.store, name, wrapped)


@pytest.mark.store
@pytest.mark.parametrize("case", REQUEST_NOW, ids=[c["id"] for c in REQUEST_NOW])
def test_request_now(env, case, monkeypatch):
    rid = case["id"]

    def jump(to):
        def action():
            env.clock.now = to
        return action
    if rid == "renew":
        host = env.create()
        env.clock.now = NOW + 60_000
        jump_after_read(monkeypatch, env, "query_room", NOW + LEASE + 5_000)
        result = env.report(host)
        assert result["expires_in_s"] == ROOM_LEASE_S
        assert env.raw(host["room_id"], "ROOM")["expires_at_ms"] == NOW + 60_000 + LEASE
        assert env.events("room.renewed")[-1].endswith(f"expires_in_s={ROOM_LEASE_S}")
    elif rid == "reclaim":
        host = env.create()
        lonely = env.join(host)
        gone = env.join(host)
        env.clock.now = NOW + G - 1
        env.once("release_peer", jump(NOW + G + 10_000))
        result = env.report(host, departed=[gone["peer_id"]])
        assert result["released"] == [gone["peer_id"]]
        assert env.raw(host["room_id"], f"PEER#{lonely['peer_id']}") is not None
        assert env.labels("reclaim_peer") == []
    elif rid == "join":
        host = env.create()
        env.clock.now = NOW + LEASE - 1
        jump_after_read(monkeypatch, env, "get_room", NOW + LEASE + 10_000)
        result = env.join(host)
        assert result["expires_in_s"] == 1
        assert env.raw(host["room_id"], f"PEER#{result['peer_id']}")["joined_at_ms"] == NOW + LEASE - 1
    elif rid == "create":
        jump_after_read(monkeypatch, env, "get_nonce", NOW + 7_000)
        result = env.create()
        room = env.raw(result["room_id"], "ROOM")
        assert room["created_at_ms"] == NOW and room["expires_at_ms"] == NOW + LEASE
        assert result["expires_in_s"] == ROOM_LEASE_S
    else:
        pytest.fail(f"모르는 행 {rid}")


# ---------------------------------------------------------------------------
# 출처: control_plane.md 4.3 join_room 의 멱등성 불릿과 4.2 create_room 의 멱등성. NONCE# 는 연산을 가리지 않고,
# NONCE# 는 있는데 방이 없으면 5.1 의 (없음)이라 room_not_found 다. 다시 읽는 경로가 둘이다. 사전 읽기와
# 트랜잭션 취소 뒤 다시 읽기(6.3 취소 사유 1번). writes 는 이 요청이 시도한 쓰기의 수다.
# ---------------------------------------------------------------------------
NONCE_REPLAY = [
    {"id": "create-with-player-nonce", "note": "create_room 에 같은 방 참가자의 nonce 를 주면 그 참가자의 기록을 돌려준다. "
     "남은 임대만 줄고 쓰지 않는다"},
    {"id": "join-with-host-nonce", "note": "join_room 에 같은 방 호스트의 nonce 를 주면 호스트의 기록(10.100.0.1)을 돌려준다. "
     "쓰지 않는다"},
    {"id": "create-room-gone", "note": "create_room 재시도. NONCE# 는 남고 ROOM 이 없다. room_not_found. 새 방을 만들지 않는다"},
    {"id": "join-room-gone", "note": "join_room 재시도. NONCE# 는 남고 ROOM 이 없다. room_not_found"},
    {"id": "create-room-gone-after-cancel", "note": "사전 읽기에는 NONCE# 가 없었다. 트랜잭션이 NONCE# 로 취소되고 다시 "
     "읽은 방이 없다. room_not_found"},
    {"id": "join-room-gone-after-cancel", "note": "같음. join_room 의 트랜잭션은 ROOM 조건과 NONCE# 가 함께 실패하고 "
     "NONCE# 가 이긴다. 다시 읽은 방이 없으니 room_not_found"},
]


def delete_room(env, room_id):
    env.store.client.delete_item(TableName=env.store.table, Key={"pk": {"S": room_id}, "sk": {"S": "ROOM"}})


@pytest.mark.store
@pytest.mark.parametrize("case", NONCE_REPLAY, ids=[c["id"] for c in NONCE_REPLAY])
def test_nonce_replay(env, case):
    rid = case["id"]
    host_nonce, player_nonce = env.nonce(), env.nonce()
    host = env.create(host_nonce)
    player = env.join(host, player_nonce)
    env.clock.now = NOW + 7_000
    env.writes.clear()
    if rid == "create-with-player-nonce":
        assert env.create(player_nonce) == dict(player, expires_in_s=ROOM_LEASE_S - 7)
        assert env.writes == []
    elif rid == "join-with-host-nonce":
        assert env.join(host, host_nonce) == dict(host, expires_in_s=ROOM_LEASE_S - 7)
        assert env.writes == []
    elif rid == "create-room-gone":
        delete_room(env, host["room_id"])
        assert code_of(lambda: env.create(host_nonce)) == "room_not_found"
        assert env.writes == []
    elif rid == "join-room-gone":
        delete_room(env, host["room_id"])
        assert code_of(lambda: env.join(host, player_nonce)) == "room_not_found"
        assert env.writes == []
    elif rid == "create-room-gone-after-cancel":
        nonce = env.nonce()
        env.once("create_room", lambda: delete_room(env, env.create(nonce)["room_id"]))
        assert code_of(lambda: env.create(nonce)) == "room_not_found"
        assert len(env.labels("create_room")) == 2
    elif rid == "join-room-gone-after-cancel":
        nonce = env.nonce()

        def inner():
            env.join(host, nonce)
            delete_room(env, host["room_id"])
        env.once("join_room", inner)
        assert code_of(lambda: env.join(host, nonce)) == "room_not_found"
        # 바깥 .2 한 번, 안쪽 .2 와 .3. 바깥은 다음 주소로 넘어가지 않는다
        assert env.labels("join_room") == [f"VIP#{POOL[0]}", f"VIP#{POOL[0]}", f"VIP#{POOL[1]}"]
    else:
        pytest.fail(f"모르는 행 {rid}")


# ---------------------------------------------------------------------------
# 스트레스 시험. 출처는 roadmap.md Phase 3 검증의 "두 피어가 동시에 참가해도 가상 IP 가 겹치지 않음" 과 "다섯이
# 동시에 참가하는 경우" 다. 겹침은 스레드 시작이 아니라 저장소 경계에서 만든다. 읽기를 마친 스레드가 장벽에서
# 서로를 기다린 뒤에야 쓴다. 그 겹침이 실제로 일어났는지(모두 같은 상태를 읽고 쓰기를 시도했는지)를 단언한다.
# internal 은 상한 있는 백오프로 다시 보낸다. 변이의 kills 는 이 표에 걸지 않는다. 결정적 경합은 CANCEL_ACTIONS
# 의 vip-race 와 nonce-join, test_host_report.py 의 test_confirm_race_concurrent_host_report 가 본다.
# ---------------------------------------------------------------------------
STRESS = [
    {"id": "five-joins", "note": "다섯이 같은 방 상태(ROOM)를 읽은 뒤 동시에 쓴다. 넷이 서로 다른 주소를 받고 하나가 "
     "room_full 이다. 다섯 모두 첫 시도가 10.100.0.2 였다"},
    {"id": "four-host-reports-confirm", "note": "넷이 PAIR# 가 하나도 없는 방을 읽은 뒤 동시에 네 쌍을 확인한다. pair.ready "
     "가 쌍마다 정확히 한 줄이다"},
]


def gate_first_calls(monkeypatch, obj, name, parties, when=lambda *a, **k: True):
    """obj.name 의 처음 parties 번 호출(when 이 참인 것)이 끝난 뒤 장벽에서 서로를 기다리게 한다."""
    real = getattr(obj, name)
    barrier = threading.Barrier(parties, timeout=20)
    lock = threading.Lock()
    state = {"n": 0}

    def gated(*args, **kwargs):
        out = real(*args, **kwargs)
        if when(*args, **kwargs):
            with lock:
                state["n"] += 1
                mine = state["n"] <= parties
            if mine:
                barrier.wait()
        return out
    monkeypatch.setattr(obj, name, gated)
    return barrier


@pytest.mark.store
@pytest.mark.parametrize("case", STRESS, ids=[c["id"] for c in STRESS])
def test_stress(env, case, monkeypatch):
    host = env.create()
    if case["id"] == "five-joins":
        nonces = [env.nonce() for _ in range(5)]
        env.writes.clear()
        gate_first_calls(monkeypatch, env.store, "get_room", 5)
        results = run_threads(5, lambda i: retry_internal(lambda: env.join(host, nonces[i])))
        joined = [r for r in results if isinstance(r, dict)]
        assert sorted(r["virtual_ip"] for r in joined) == POOL
        assert [r for r in results if not isinstance(r, dict)] == ["room_full"]
        assert len({r["peer_id"] for r in joined}) == len(POOL)
        assert env.labels("join_room").count(f"VIP#{POOL[0]}") >= 5      # 겹침이 실제로 일어났다
        vips = [sk for sk in env.sks(host["room_id"]) if sk.startswith("VIP#")]
        assert sorted(vips) == sorted(["VIP#10.100.0.1"] + [f"VIP#{ip}" for ip in POOL])
        assert len(env.events("vip.claimed")) == len(POOL)
    else:
        env.register(host, cands=CAND_H)
        players = [env.join(host) for _ in POOL]
        for p in players:
            env.register(p)
        ids_ = [p["peer_id"] for p in players]
        env.writes.clear()
        gate_first_calls(monkeypatch, env.store, "query_room", 4, when=lambda *a, **k: k.get("lenient") is True)
        results = run_threads(4, lambda i: retry_internal(lambda: env.report(host, confirm=ids_)))
        assert all(isinstance(r, dict) for r in results), results
        assert len(env.labels("confirm_pair")) >= 4 * len(POOL)           # 넷 모두 PAIR# 없음을 읽고 썼다
        confirmed = [x for r in results for x in r["confirmed"]]
        assert sorted(confirmed) == sorted(ids_)
        assert sorted(env.events("pair.ready")) == sorted(
            f"INFO pair.ready host_peer_id={host['peer_id']} peer_id={x} punch_delay_ms={PUNCH_DELAY_MS}" for x in ids_)
        final = env.report(host)
        assert all(p["ready"] for p in final["peers"]) and len(final["peers"]) == len(POOL)


def test_every_table_has_ids():
    for table in (FIELDS, FIELD_ACCEPT, ALLOWED, ORDER, CANCEL_ACTIONS, SCENARIOS, RENEW, UNREADABLE, TOKEN_COMPARE,
                  REQUEST_NOW, NONCE_REPLAY, STRESS):
        seen = [c["id"] for c in table]
        assert len(seen) == len(set(seen))
        assert all(c.get("note") is not None for c in table)
    assert len(ALLOWED) == 16 and len(CANCEL_ACTIONS) == 15
