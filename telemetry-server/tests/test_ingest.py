"""레코드 검사와 방 대조의 케이스 표 (architecture.md 6.3 서버의 검사, 3.4 방 대조, 저장).

CASES 는 필드마다 맞는 형과 틀린 형이다. 그 필드가 있는 kind 마다 한 행씩 돈다. ROOM_KNOWN 은 room_known 의 네 경우다.
저장(PutItem)은 store 표시 행이 DynamoDB local 에서 본다.
"""

import copy

import pytest
from botocore.exceptions import ClientError, EndpointConnectionError

from controlplane.errors import OpError
from controlplane.server import Request
from telemetry import ingest

MISSING = object()
HEX = "0123456789abcdef" * 2
MAX = 2**53 - 1
U32 = 2**32 - 1

COMMON = {"v": 1, "instance_id": HEX, "attempt": 1, "seq": 0, "role": "player", "room_id": "ABCDEF",
          "peer_id": 7, "peer": 3021551877, "os": "windows", "ui": "console", "build_kind": "release"}
BASE = {
    "establish": {**COMMON, "kind": "establish", "result": "OK", "setup_ms": 2410, "local_differs_public": True,
                  "stun_same_endpoint": True, "same_lan": False},
    "interval": {**COMMON, "kind": "interval", "seq": 1, "interval_ms": 60000, "rtt_us": [23410, 24000],
                 "rtt_overflow": 0, "jitter_us": 350, "expected": 12, "accepted": 12, "bytes_out": 5000,
                 "bytes_in": 4000},
}
BASE["end"] = {**BASE["interval"], "kind": "end", "seq": 2, "result": "CLOSED", "duration_ms": 120000,
               "total_expected": 24, "total_accepted": 23, "total_bytes_out": 10000, "total_bytes_in": 8000,
               "total_tx_err_send": 0}

INT0 = ([0, 1, MAX], [-1, MAX + 1, 1.0, 1.5, "1", True, None, MISSING])
NULL_INT0 = ([None, 0, MAX], [-1, MAX + 1, 1.0, "1", True, MISSING])
NULL_BOOL = ([None, True, False], [0, 1, "true", MISSING])
# 필드 -> (맞는 값, 틀린 값). 6.3 의 표와 서버의 검사 네 줄이 출처다.
CASES = {
    "v": ([1], [0, 2, "1", 1.0, True, None, MISSING]),
    "instance_id": ([HEX], [HEX.upper(), HEX[:31], HEX + "0", "g" * 32, 32, None, MISSING]),
    "attempt": ([1, MAX], [0, -1, MAX + 1, 1.0, "1", True, None, MISSING]),
    "seq": INT0,
    "role": (["host", "player"], ["Host", "client", "", 1, None, MISSING]),
    "room_id": ([None, "ABCDEF", "Z23456"], ["abcdef", "ABCDE", "ABCDEFG", "ABCDE0", "ABCDEI", "", 123456, MISSING]),
    "peer_id": ([None, 1, U32], [0, U32 + 1, -1, 1.0, "7", True, MISSING]),
    "peer": ([None, 1, U32], [0, U32 + 1, -1, 1.0, "7", True, MISSING]),
    "os": (["windows", "linux"], ["macos", "Windows", None, 1, MISSING]),
    "ui": (["gui", "console"], ["cli", "GUI", None, MISSING]),
    "build_kind": (["release", "test"], ["debug", "Test", None, MISSING]),
    "setup_ms": NULL_INT0,
    "local_differs_public": NULL_BOOL,
    "stun_same_endpoint": NULL_BOOL,
    "same_lan": NULL_BOOL,
    "interval_ms": INT0,
    "rtt_us": ([[], [0], [MAX], [1] * 16], [[1] * 17, [-1], [1.5], ["1"], [True], [None], 1, None, MISSING]),
    "rtt_overflow": INT0,
    "jitter_us": NULL_INT0,
    "expected": INT0,
    "accepted": INT0,
    "bytes_out": INT0,
    "bytes_in": INT0,
    "duration_ms": INT0,
    "total_expected": ([MAX], [-1, MAX + 1, 1.0, "1", True, None, MISSING]),
    "total_accepted": ([0, 24], [25, -1, 1.0, "1", True, None, MISSING]),
    "total_bytes_out": INT0,
    "total_bytes_in": INT0,
    "total_tx_err_send": INT0,
}
RESULTS = {
    "establish": (["OK", "STUN_DISCOVERY_FAILED", "CONTROL_PLANE_EXCHANGE_FAILED", "HOLE_PUNCH_TIMEOUT",
                   "PEER_HANDSHAKE_FAILED", "TUNNEL_DROPPED", "ABORTED", "PEER_FAILED"],
                  ["CLOSED", "ok", "", None, 1, MISSING]),
    "end": (["CLOSED", "STUN_DISCOVERY_FAILED", "CONTROL_PLANE_EXCHANGE_FAILED", "HOLE_PUNCH_TIMEOUT",
             "PEER_HANDSHAKE_FAILED", "TUNNEL_DROPPED", "PEER_FAILED"],
            ["OK", "ABORTED", "closed", None, MISSING]),
}


def _rows():
    rows = []
    for kind, base in BASE.items():
        fields = dict(CASES)
        if kind in RESULTS:
            fields["result"] = RESULTS[kind]
        for name, (good, bad) in fields.items():
            if name not in base:
                continue
            for i, value in enumerate(good):
                rows.append(pytest.param(kind, name, value, True, id=f"{kind}-{name}-ok{i}"))
            for i, value in enumerate(bad):
                rows.append(pytest.param(kind, name, value, False, id=f"{kind}-{name}-bad{i}"))
    return rows


def record(base, **over):
    r = copy.deepcopy(BASE[base])
    for k, v in over.items():
        if v is MISSING:
            r.pop(k, None)
        else:
            r[k] = v
    return r


@pytest.mark.parametrize("kind,name,value,ok", _rows())
def test_check_record(kind, name, value, ok):
    r = record(kind, **{name: value})
    if ok:
        assert ingest.check_record(r)[name] == value
    else:
        with pytest.raises(OpError) as e:
            ingest.check_record(r)
        assert e.value.code == "bad_request"


KIND_CASES = [
    ("kind-unknown", {"kind": "report"}),
    ("kind-upper", {"kind": "END"}),
    ("kind-missing", {"kind": MISSING}),
    ("kind-null", {"kind": None}),
    ("kind-list", {"kind": ["end"]}),
]


@pytest.mark.parametrize("over", [c[1] for c in KIND_CASES], ids=[c[0] for c in KIND_CASES])
def test_kind(over):
    with pytest.raises(OpError) as e:
        ingest.check_record(record("end", **over))
    assert e.value.code == "bad_request"


def test_each_kind_requires_its_own_fields():
    # interval 레코드에 establish 의 결과를 붙여도 interval 의 필드가 빠지면 거부한다
    r = record("establish", kind="interval")
    with pytest.raises(OpError):
        ingest.check_record(r)


def test_unknown_keys_dropped():
    r = record("establish", extra=1, total_expected=5)
    out = ingest.check_record(r)
    assert "extra" not in out and "total_expected" not in out
    assert set(out) == set(BASE["establish"])


def test_message_has_no_value():
    with pytest.raises(OpError) as e:
        ingest.check_record(record("establish", room_id="secret"))
    assert "secret" not in e.value.message


# ---------------------------------------------------------------- 방 대조와 저장 (가짜 클라이언트)

class FakeDdb:
    def __init__(self, room=None, get_error=None, put_error=None):
        self.room, self.get_error, self.put_error = room, get_error, put_error
        self.gets, self.puts = [], []

    def get_item(self, **kw):
        self.gets.append(kw)
        if self.get_error:
            raise self.get_error
        return {"Item": {"pk": {"S": "x"}}} if self.room else {}

    def put_item(self, **kw):
        self.puts.append(kw)
        if self.put_error:
            raise self.put_error
        return {}


class FakeMetrics:
    def __init__(self):
        self.added = []

    def add(self, r):
        self.added.append(r)


def _client_error(code):
    return ClientError({"Error": {"Code": code, "Message": "x"}}, "Op")


ROOM_KNOWN = [
    ("room-present", "ABCDEF", dict(room=True), True),
    ("room-absent", "ABCDEF", dict(room=False), False),
    ("no-room-id", None, dict(room=True), None),
    ("read-failed-client", "ABCDEF", dict(get_error=_client_error("ProvisionedThroughputExceededException")), None),
    ("read-failed-connect", "ABCDEF", dict(get_error=EndpointConnectionError(endpoint_url="http://x")), None),
]


@pytest.mark.parametrize("room_id,fake,expected", [c[1:] for c in ROOM_KNOWN], ids=[c[0] for c in ROOM_KNOWN])
def test_room_known(room_id, fake, expected):
    ddb, m = FakeDdb(**fake), FakeMetrics()
    ing = ingest.Ingest(ddb, "tm", "cp", m, clock_ms=lambda: 1234)
    assert ing.dispatch(Request("report", record("establish", room_id=room_id))) == {}
    item = ddb.puts[0]["Item"]
    assert item["room_known"] == ({"NULL": True} if expected is None else {"BOOL": expected})
    assert m.added[0]["room_known"] is expected
    if room_id is None:
        assert ddb.gets == []
    else:
        get = ddb.gets[0]
        assert get["TableName"] == "cp" and get["Key"] == {"pk": {"S": room_id}, "sk": {"S": "ROOM"}}
        assert get["ConsistentRead"] is False


def test_put_item_shape():
    ddb, m = FakeDdb(room=True), FakeMetrics()
    ingest.Ingest(ddb, "tm", "cp", m, clock_ms=lambda: 1234).dispatch(Request("report", record("end", extra=1)))
    put = ddb.puts[0]
    assert put["TableName"] == "tm"
    assert put["ConditionExpression"] == "attribute_not_exists(pk)"
    item = put["Item"]
    assert item["pk"] == {"S": HEX + "#1"}
    assert item["seq"] == {"N": "2"}
    assert item["received_at_ms"] == {"N": "1234"}
    assert item["rtt_us"] == {"L": [{"N": "23410"}, {"N": "24000"}]}
    assert "extra" not in item


def test_bad_record_not_stored():
    ddb, m = FakeDdb(room=True), FakeMetrics()
    with pytest.raises(OpError):
        ingest.Ingest(ddb, "tm", "cp", m).dispatch(Request("report", record("end", v=2)))
    assert ddb.gets == [] and ddb.puts == [] and m.added == []


def test_duplicate_is_bad_request_and_not_counted():
    ddb, m = FakeDdb(room=True, put_error=_client_error("ConditionalCheckFailedException")), FakeMetrics()
    with pytest.raises(OpError) as e:
        ingest.Ingest(ddb, "tm", "cp", m).dispatch(Request("report", record("end")))
    assert e.value.code == "bad_request"
    assert m.added == []


def test_other_store_failure_raises():
    # 서버의 포괄 처리가 internal 로 답한다 (control_plane.md 7.2)
    ddb, m = FakeDdb(room=True, put_error=_client_error("ProvisionedThroughputExceededException")), FakeMetrics()
    with pytest.raises(ClientError):
        ingest.Ingest(ddb, "tm", "cp", m).dispatch(Request("report", record("end")))
    assert m.added == []


# ---------------------------------------------------------------- DynamoDB local

@pytest.mark.store
def test_store_roundtrip(tables):
    client, tm, cpt, admin = tables
    admin.put_item(TableName=cpt, Item={"pk": {"S": "ABCDEF"}, "sk": {"S": "ROOM"}, "expires_at_ms": {"N": "1"}})
    m = FakeMetrics()
    ing = ingest.Ingest(client, tm, cpt, m, clock_ms=lambda: 99)
    ing.dispatch(Request("report", record("establish")))
    ing.dispatch(Request("report", record("interval", room_id="ZZZZZZ")))
    with pytest.raises(OpError) as e:
        ing.dispatch(Request("report", record("establish", setup_ms=1)))
    assert e.value.code == "bad_request"
    items = admin.query(TableName=tm, KeyConditionExpression="pk = :p",
                        ExpressionAttributeValues={":p": {"S": HEX + "#1"}})["Items"]
    by_seq = {int(i["seq"]["N"]): i for i in items}
    assert set(by_seq) == {0, 1}
    assert by_seq[0]["room_known"] == {"BOOL": True}
    assert by_seq[0]["setup_ms"] == {"N": "2410"}
    assert by_seq[1]["room_known"] == {"BOOL": False}
    assert by_seq[1]["received_at_ms"] == {"N": "99"}
    assert [r["room_known"] for r in m.added] == [True, False]
