"""레코드 검사, 방 대조, 저장. 출처는 architecture.md 6.3 텔레메트리 서비스 인터페이스(서버의 검사)와
3.4 텔레메트리 서비스의 방 대조, 저장이다. 결과 값의 출처는 9장 로컬 기록 파일의 결과 값이다.
"""

from __future__ import annotations

import re
import time
from collections.abc import Callable

from botocore.exceptions import BotoCoreError, ClientError
from boto3.dynamodb.types import TypeSerializer

from controlplane.errors import OpError
from controlplane.ids import ROOM_ID_ALPHABET, ROOM_ID_LEN

MAX_INT = 2**53 - 1
UINT32_MAX = 2**32 - 1
MAX_RTT_SAMPLES = 16
ROOM_SK = "ROOM"  # control_plane.md 6.3 항목

FR13 = ("STUN_DISCOVERY_FAILED", "CONTROL_PLANE_EXCHANGE_FAILED", "HOLE_PUNCH_TIMEOUT", "PEER_HANDSHAKE_FAILED",
        "TUNNEL_DROPPED")
ESTABLISH_RESULTS = frozenset({"OK", *FR13, "ABORTED", "PEER_FAILED"})
END_RESULTS = frozenset({"CLOSED", *FR13, "PEER_FAILED"})

_HEX32 = re.compile(r"[0-9a-f]{32}")
_ROOM_ID = re.compile(f"[{ROOM_ID_ALPHABET}]{{{ROOM_ID_LEN}}}")


def _int(lo: int = 0, hi: int = MAX_INT) -> Callable[[object], bool]:
    # bool 은 Python 에서 int 다. JSON 의 true 를 정수로 받지 않는다.
    return lambda v: type(v) is int and lo <= v <= hi


def _nullable(check: Callable[[object], bool]) -> Callable[[object], bool]:
    return lambda v: v is None or check(v)


def _one_of(*values: str) -> Callable[[object], bool]:
    allowed = frozenset(values)
    return lambda v: type(v) is str and v in allowed


def _bool(v: object) -> bool:
    return type(v) is bool


def _rtt(v: object) -> bool:
    return type(v) is list and len(v) <= MAX_RTT_SAMPLES and all(_int()(x) for x in v)


COMMON = {
    "v": lambda v: type(v) is int and v == 1,
    "kind": _one_of("establish", "interval", "end"),
    "instance_id": lambda v: type(v) is str and _HEX32.fullmatch(v) is not None,
    "attempt": _int(1),
    "seq": _int(),
    "role": _one_of("host", "player"),
    "room_id": _nullable(lambda v: type(v) is str and _ROOM_ID.fullmatch(v) is not None),
    "peer_id": _nullable(_int(1, UINT32_MAX)),
    "peer": _nullable(_int(1, UINT32_MAX)),
    "os": _one_of("windows", "linux"),
    "ui": _one_of("gui", "console"),
    "build_kind": _one_of("release", "test"),
}
ESTABLISH = {
    "result": lambda v: type(v) is str and v in ESTABLISH_RESULTS,
    "setup_ms": _nullable(_int()),
    "local_differs_public": _nullable(_bool),
    "stun_same_endpoint": _nullable(_bool),
    "same_lan": _nullable(_bool),
}
INTERVAL = {
    "interval_ms": _int(),
    "rtt_us": _rtt,
    "rtt_overflow": _int(),
    "jitter_us": _nullable(_int()),
    "expected": _int(),
    "accepted": _int(),
    "bytes_out": _int(),
    "bytes_in": _int(),
}
END = {
    **INTERVAL,
    "result": lambda v: type(v) is str and v in END_RESULTS,
    "duration_ms": _int(),
    "total_expected": _int(),
    "total_accepted": _int(),
    "total_bytes_out": _int(),
    "total_bytes_in": _int(),
    "total_tx_err_send": _int(),
}
FIELDS = {
    "establish": {**COMMON, **ESTABLISH},
    "interval": {**COMMON, **INTERVAL},
    "end": {**COMMON, **END},
}


def check_record(body: dict) -> dict:
    """6.3 서버의 검사. 그 kind 의 필드만 담은 사본을 돌려준다. 알 수 없는 키는 버린다.

    실패는 bad_request 다. 메시지에 값을 싣지 않는다 (control_plane.md 4.1).
    """
    table = FIELDS.get(body.get("kind")) if type(body.get("kind")) is str else None
    if table is None:
        raise OpError("bad_request", "kind")
    out = {}
    for name, ok in table.items():
        if name not in body or not ok(body[name]):
            raise OpError("bad_request", name)
        out[name] = body[name]
    if out["kind"] == "end" and out["total_accepted"] > out["total_expected"]:
        raise OpError("bad_request", "total_accepted")
    return out


class Ingest:
    """report 하나. 검사, 방 대조, PutItem, 지표. dispatch 는 controlplane.server 의 dispatch 계약이다."""

    def __init__(self, dynamodb, table: str, cp_table: str, metrics,
                 clock_ms: Callable[[], int] = lambda: time.time_ns() // 1_000_000) -> None:
        self.client = dynamodb
        self.table = table
        self.cp_table = cp_table
        self.metrics = metrics
        self._clock_ms = clock_ms
        self._ser = TypeSerializer()

    def room_known(self, room_id: str | None) -> bool | None:
        """3.4 방 대조. 강한 일관성 읽기가 아니다. 읽기가 실패하면 null 이다."""
        if room_id is None:
            return None
        try:
            resp = self.client.get_item(TableName=self.cp_table, Key={"pk": {"S": room_id}, "sk": {"S": ROOM_SK}},
                                        ProjectionExpression="pk", ConsistentRead=False)
        except (BotoCoreError, ClientError):
            return None
        return "Item" in resp

    def dispatch(self, req) -> dict:
        record = check_record(req.body)
        record["room_known"] = self.room_known(record["room_id"])
        record["received_at_ms"] = self._clock_ms()
        item = {"pk": f"{record['instance_id']}#{record['attempt']}", **record}
        try:
            self.client.put_item(TableName=self.table, Item={k: self._ser.serialize(v) for k, v in item.items()},
                                 ConditionExpression="attribute_not_exists(pk)")
        except ClientError as exc:
            if exc.response.get("Error", {}).get("Code") == "ConditionalCheckFailedException":
                raise OpError("bad_request", "duplicate") from None
            raise
        self.metrics.add(record)
        return {}
