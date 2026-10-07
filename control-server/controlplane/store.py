"""저장소. boto3 호출과 6.3 항목의 조건식은 전부 이 파일에만 있다 (control_plane.md 7.1 모듈).

출처는 control_plane.md 6.1 저장 계약, 6.2 테이블, 6.3 항목, 6.5 TTL, 7.6 설정과 배포다.
시험은 tests/test_store.py, 변이는 tests/mutants/store.py.

- 읽기는 전부 ConsistentRead=True 다 (저장 1). GSI 를 쓰지 않는다 (저장 2)
- 방·피어 상태를 메모리에 들고 있지 않는다 (저장 5). 이 객체가 갖는 것은 테이블 이름과 클라이언트뿐이다
- 쓰기 메서드 하나가 6.3 쓰기 표의 한 행이다. 한 번의 시도만 한다. 재시도와 재추첨(풀 순회, peer_id
  와 room_id 재추첨, NONCE# 다시 읽기)은 부르는 쪽(ops)이 결과를 보고 한다
- 결과는 작은 값으로 돌려준다. 문자열 상수, dict, RoomView. boto3 응답을 그대로 넘기지 않는다
- 저장소가 돌려준 숫자(Decimal)는 int 로 바꾼다. 항목 표의 속성이 없거나 형이 다르면 판정 불가이고
  OpError("internal") 이다. 예외는 6.3 의 둘이다. PEER# 의 candidates 가 없으면 빈 목록이고, 4.6 서버
  회수가 읽는 자리(query_room(lenient=True))는 읽지 못한 PEER# 를 따로 넘겨 부르는 쪽이 건너뛰게 한다

쓰기 주입점. on_write(phase, label, sk) 를 쓰기 직전("before")과 쓰기가 성공한 직후("after")에
부른다. 조건이 실패한 쓰기에는 "after" 를 부르지 않는다. 기본값은 아무것도 하지 않는다. 시험은
"before" 에서 다른 연산을 끼워 넣어 경합 순서를 만들고, "after" 에서 예외를 던져 커밋 뒤 응답 전
실패를 만든다. 주입점이 던진 예외는 그대로 올라간다.
"""

from __future__ import annotations

import os
import re
from collections.abc import Callable, Mapping
from dataclasses import dataclass, field
from decimal import Decimal

import boto3.session
from boto3.dynamodb.types import TypeDeserializer, TypeSerializer
from botocore.exceptions import ClientError

from controlplane import ops
from controlplane.constants import JOIN_REGISTER_GRACE_S, PUNCH_DELAY_MS, ROOM_LEASE_S, STORAGE_GRACE_S
from controlplane.errors import OpError

# 2.5 가상 IP 풀. 호스트(방 생성자)의 주소다. 참가자 풀의 순회는 부르는 쪽이 한다.
HOST_VIRTUAL_IP = "10.100.0.1"

ROOM_SK = "ROOM"
NONCE_SK = "NONCE"

# 6.3 항목 표의 sk 형식. 방 파티션의 sk 는 이 넷 가운데 하나와 전체 일치해야 한다. 아니면 판정 불가다.
# [0-9] 를 쓴다. \d 는 ASCII 밖의 숫자도 받는다. 선행 0 은 받지 않는다 (10진수 표기가 하나여야 한다).
_DEC = "(?:[1-9][0-9]{0,9})"
_OCTET = "(?:25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9][0-9]|[0-9])"
_PEER_SK = re.compile(f"PEER#({_DEC})")
_VIP_SK = re.compile(f"VIP#({_OCTET}(?:\\.{_OCTET}){{3}})")
_PAIR_SK = re.compile(f"PAIR#({_DEC})-({_DEC})")
_UINT32_MAX = 2**32 - 1

# ---------------------------------------------------------------- 결과 값

CREATED = "created"
JOINED = "joined"
NONCE_EXISTS = "nonce_exists"      # 취소 사유 1. NONCE# 를 다시 읽어 같은 응답을 조립한다
ROOM_GONE = "room_gone"            # 취소 사유 2. 방을 다시 읽어 room_not_found / room_expired
VIP_TAKEN = "vip_taken"            # 취소 사유 3. 다음 주소. 풀이 끝나면 room_full
PEER_ID_TAKEN = "peer_id_taken"    # 취소 사유 4. peer_id 재추첨 (MAX_PEER_ID_ATTEMPTS)
ROOM_ID_TAKEN = "room_id_taken"    # 취소 사유 5. room_id 재추첨 (MAX_ROOM_ID_ATTEMPTS)
CONFIRMED = "confirmed"
ALREADY_CONFIRMED = "already_confirmed"   # PAIR# put 조건만 실패. 오류가 아니다
NOT_ELIGIBLE = "not_eligible"             # 두 ConditionCheck 중 하나가 실패. 쓰지 않는다

# 트랜잭션 안 항목의 종류. 취소 사유를 위치로 읽을 때 쓴다.
_NONCE = "nonce"
_ROOM_CHECK = "room_check"
_VIP = "vip"
_PEER = "peer"
_ROOM_PUT = "room_put"
_PEER_CHECK = "peer_check"
_PAIR = "pair"
_OTHER = "other"   # 조건 없는 delete. 조건 실패가 날 수 없다

# 6.3 취소 사유 표의 2번부터의 순서. 1번(NONCE#)은 classify_cancel 이 다른 사유보다 먼저 본다.
# 연산마다 그 연산의 트랜잭션에 있는 행만 쓴다.
_JOIN_PRIORITY = (_ROOM_CHECK, _VIP, _PEER)
# create_room 은 6.3 쓰기 표의 행을 따른다. ROOM put 실패가 PEER#·VIP# 실패를 설명한다(앞선 방이
# 있다). ROOM 없이 PEER#·VIP# 만 실패하면 순서에 없으므로 internal 이다.
_CREATE_PRIORITY = (_ROOM_PUT,)
_CONFIRM_PRIORITY = (_PEER_CHECK, _PAIR)
_DELETE_PRIORITY = (_PEER,)

_CCF = "ConditionalCheckFailed"
_NO_FAILURE = "None"

WriteHook = Callable[[str, str, str], None]

# 6.3 쓰기 표의 조건식. 이 파일 밖에 두지 않는다 (7.1).
_IF_ABSENT = "attribute_not_exists(pk)"
_IF_EXISTS = "attribute_exists(pk)"
_JOIN_ROOM_IF = "attribute_exists(pk) AND expires_at_ms > :now"
_REGISTER_IF = "attribute_exists(pk) AND peer_token = :t"
_RECLAIM_IF = ("attribute_exists(pk) AND (attribute_not_exists(candidates) OR size(candidates) = :zero) AND joined_at_ms <= :cutoff")
_CONFIRM_HOST_IF = "attribute_exists(pk) AND size(candidates) > :zero"
_CONFIRM_OTHER_IF = "attribute_exists(pk) AND size(candidates) > :zero"
_RENEW_ROOM_IF = "attribute_exists(pk) AND expires_at_ms > :now"
_RENEW_ITEM_IF = "attribute_exists(pk)"


def _no_hook(phase: str, label: str, sk: str) -> None:
    return None


def _internal(message: str) -> OpError:
    return OpError("internal", message)


def classify_cancel(layout: tuple[str, ...], reasons: object, priority: tuple[str, ...]) -> str:
    """6.3 취소 사유 표. 트랜잭션에 넣은 항목의 종류(layout)와 CancellationReasons 를 위치로 맞춘다.

    돌려주는 것은 priority 에서 처음 맞는 종류다. 판정 불가는 전부 internal 이다.
    - layout 에 NONCE# 가 있고 그 조건이 실패했으면 다른 사유가 무엇이든 _NONCE 다 (1번)
    - 조건 실패가 아닌 사유(TransactionConflict, 스로틀, 모르는 코드)가 섞이면 internal
    - 사유 수가 항목 수와 다르면 위치로 읽을 수 없으므로 internal
    """
    if not isinstance(reasons, list) or len(reasons) != len(layout):
        raise _internal("트랜잭션 취소 사유를 위치로 읽을 수 없다")
    codes = []
    for reason in reasons:
        code = reason.get("Code") if isinstance(reason, dict) else None
        if code not in (_CCF, _NO_FAILURE):
            code = _OTHER
        codes.append(code)
    failed = {kind for kind, code in zip(layout, codes) if code == _CCF}
    if _NONCE in failed:
        return _NONCE
    if _OTHER in codes:
        raise _internal("트랜잭션이 조건 실패가 아닌 사유로 취소됐다")
    for kind in priority:
        if kind in failed:
            return kind
    raise _internal("트랜잭션 취소 사유를 판정할 수 없다")


def created_expires_at(now_ms: int) -> int:
    """6.3 ROOM 행. 방을 만들 때의 첫 만료 시각."""
    return now_ms + ROOM_LEASE_S * 1000


def ttl_for(expires_at_ms: int) -> int:
    """6.5 TTL. 방의 모든 항목이 같은 값을 갖는다."""
    return expires_at_ms // 1000 + STORAGE_GRACE_S


def peer_sk(peer_id: int) -> str:
    return f"PEER#{peer_id}"


def vip_sk(virtual_ip: str) -> str:
    return f"VIP#{virtual_ip}"


def nonce_pk(client_nonce: str) -> str:
    return f"NONCE#{client_nonce}"


# ---------------------------------------------------------------- 형 변환 (6.3 마지막 문단)

class _Unreadable(Exception):
    pass


def _to_int(value: object) -> int:
    # boto3 는 숫자를 Decimal 로 준다. 정수인 Decimal 만 받는다. bool 은 BOOL 형이라 여기 오지 않는다.
    if isinstance(value, Decimal) and value.is_finite() and value == value.to_integral_value():
        return int(value)
    raise _Unreadable


def _to_str(value: object) -> str:
    if isinstance(value, str):
        return value
    raise _Unreadable


def _to_candidates(value: object) -> list[dict]:
    if not isinstance(value, list):
        raise _Unreadable
    out = []
    for c in value:
        if not isinstance(c, dict):
            raise _Unreadable
        out.append({"ip": _to_str(c.get("ip")), "port": _to_int(c.get("port")), "kind": _to_str(c.get("kind"))})
    return out


_ROOM_ATTRS = {"created_at_ms": _to_int, "expires_at_ms": _to_int, "host_peer_id": _to_int, "ttl": _to_int}
_PEER_ATTRS = {"peer_id": _to_int, "peer_token": _to_str, "virtual_ip": _to_str, "client_nonce": _to_str,
               "joined_at_ms": _to_int, "ttl": _to_int}
_VIP_ATTRS = {"peer_id": _to_int, "ttl": _to_int}
_PAIR_ATTRS = {"ready_at_wall_ms": _to_int, "ready_at_mono_ns": _to_int, "ready_boot_id": _to_str,
               "punch_delay_ms": _to_int, "ttl": _to_int}
_NONCE_ATTRS = {"room_id": _to_str, "peer_id": _to_int, "ttl": _to_int}


def sk_kind(sk: str) -> str:
    """방 파티션 sk 의 종류. "ROOM", "PEER", "VIP", "PAIR". 6.3 항목 표의 형식과 전체 일치하지 않으면
    판정 불가(_Unreadable)다. 접두어만 보지 않는다.
    """
    if sk == ROOM_SK:
        return ROOM_SK
    m = _PEER_SK.fullmatch(sk)
    if m is not None and int(m.group(1)) <= _UINT32_MAX:
        return "PEER"
    if _VIP_SK.fullmatch(sk) is not None:
        return "VIP"
    m = _PAIR_SK.fullmatch(sk)
    if m is not None and int(m.group(1)) < int(m.group(2)) <= _UINT32_MAX:
        return "PAIR"
    raise _Unreadable


def _convert(item: Mapping, attrs: Mapping[str, Callable]) -> dict:
    out = {}
    for name, conv in attrs.items():
        if name not in item:
            raise _Unreadable
        out[name] = conv(item[name])
    return out


def _convert_peer(item: Mapping) -> dict:
    out = _convert(item, _PEER_ATTRS)
    # 6.3 예외 1. 후보를 아직 등록하지 않은 피어(5.3 joined)는 속성이 없다. 빈 목록이다.
    out["candidates"] = _to_candidates(item["candidates"]) if "candidates" in item else []
    return out


def _loose_peer(item: Mapping) -> dict:
    """4.6 서버 회수가 판정하도록 읽지 못한 PEER# 를 넘기는 모양. 정수인 Decimal 만 int 로 바꾸고
    나머지는 읽은 그대로 둔다. 판정(ops.reclaim_verdict)이 int 가 아닌 값을 판정 불가로 본다.
    """
    out = {}
    for name in (*_PEER_ATTRS, "candidates"):
        if name not in item:
            continue
        value = item[name]
        if name == "candidates":
            try:
                value = _to_candidates(value)
            except _Unreadable:
                pass
        else:
            try:
                value = _to_int(value)
            except _Unreadable:
                pass
        out[name] = value
    return out


# ---------------------------------------------------------------- 읽은 방

@dataclass
class RoomView:
    """Query(pk = room_id) 한 번의 결과. 값은 전부 변환을 거친 것이다."""
    room: dict | None = None
    peers: dict[int, dict] = field(default_factory=dict)
    vips: dict[str, int] = field(default_factory=dict)          # virtual_ip -> peer_id
    pairs: dict[str, dict] = field(default_factory=dict)        # PAIR#<lo>-<hi> -> 항목
    unreadable_peers: list[dict] = field(default_factory=list)  # lenient 일 때만 찬다
    sks: list[str] = field(default_factory=list)                # 이 파티션의 모든 sk. 갱신 쓰기의 목록


# ---------------------------------------------------------------- 저장소

class Store:
    def __init__(self, table: str, client, on_write: WriteHook | None = None) -> None:
        if not isinstance(table, str) or not table:
            raise ValueError("테이블 이름이 비었다")
        self.table = table
        self.client = client
        self.on_write = on_write or _no_hook
        self._ser = TypeSerializer()
        self._de = TypeDeserializer()

    @classmethod
    def from_env(cls, environ: Mapping[str, str] | None = None, client=None,
                 on_write: WriteHook | None = None, config=None, session=None) -> "Store":
        """7.6 설정. SANGTACHI_CP_TABLE 과 AWS_REGION 은 필수, SANGTACHI_CP_ENDPOINT 는 선택이다.
        자격 증명을 받는 변수는 없다. boto3 표준 경로(EC2 에서는 IAM 역할)에 맡긴다.

        boto3 의 기본 세션(boto3.DEFAULT_SESSION)을 쓰지 않고 새 Session 을 만든다. 기본 세션은 먼저
        만든 쪽의 자격 증명을 캐시한다. config 는 botocore Config 이고 시험이 프록시를 끄는 데 쓴다.
        session 은 시험이 서비스 모델 로더를 나눠 쓰는 격리된 Session 을 넘길 때 쓴다.
        """
        env = os.environ if environ is None else environ
        table = env.get("SANGTACHI_CP_TABLE", "")
        if not table:
            raise ValueError("SANGTACHI_CP_TABLE 이 없다")
        if client is None:
            region = env.get("AWS_REGION", "")
            if not region:
                raise ValueError("AWS_REGION 이 없다")
            endpoint = env.get("SANGTACHI_CP_ENDPOINT") or None
            if session is None:
                session = boto3.session.Session()
            client = session.client("dynamodb", region_name=region, endpoint_url=endpoint, config=config)
        return cls(table, client, on_write)

    # ------------------------------------------------------------ 직렬화

    def _values(self, values: Mapping) -> dict:
        return {k: self._ser.serialize(v) for k, v in values.items()}

    def _plain(self, raw: Mapping) -> dict:
        return {k: self._de.deserialize(v) for k, v in raw.items()}

    def _key(self, pk: str, sk: str) -> dict:
        return {"pk": {"S": pk}, "sk": {"S": sk}}

    # ------------------------------------------------------------ 쓰기 공통

    def _write(self, label: str, sk: str, call: Callable[[], object]) -> bool:
        """단일 항목 조건부 쓰기. 조건이 실패하면 False."""
        self.on_write("before", label, sk)
        try:
            call()
        except ClientError as exc:
            if exc.response.get("Error", {}).get("Code") == "ConditionalCheckFailedException":
                return False
            raise
        self.on_write("after", label, sk)
        return True

    def _transact(self, label: str, sk: str, items: list[dict], layout: tuple[str, ...],
                  priority: tuple[str, ...]) -> str | None:
        """트랜잭션 한 번. 성공하면 None, 취소되면 classify_cancel 의 종류."""
        self.on_write("before", label, sk)
        try:
            self.client.transact_write_items(TransactItems=items)
        except ClientError as exc:
            if exc.response.get("Error", {}).get("Code") != "TransactionCanceledException":
                raise
            return classify_cancel(layout, exc.response.get("CancellationReasons"), priority)
        self.on_write("after", label, sk)
        return None

    def _put(self, pk: str, sk: str, values: Mapping, condition: str) -> dict:
        return {"Put": {"TableName": self.table, "Item": self._values({"pk": pk, "sk": sk, **values}),
                        "ConditionExpression": condition}}

    def _check(self, pk: str, sk: str, condition: str, values: Mapping) -> dict:
        return {"ConditionCheck": {"TableName": self.table, "Key": self._key(pk, sk),
                                   "ConditionExpression": condition, "ExpressionAttributeValues": self._values(values)}}

    def _delete(self, pk: str, sk: str, condition: str | None = None, values: Mapping | None = None) -> dict:
        op = {"TableName": self.table, "Key": self._key(pk, sk)}
        if condition is not None:
            op["ConditionExpression"] = condition
        if values:
            op["ExpressionAttributeValues"] = self._values(values)
        return {"Delete": op}

    # ------------------------------------------------------------ 읽기 (6.3 읽기 표)

    def get_nonce(self, client_nonce: str) -> dict | None:
        """NONCE# GetItem. {room_id, peer_id, ttl} 이나 None."""
        resp = self.client.get_item(TableName=self.table, Key=self._key(nonce_pk(client_nonce), NONCE_SK),
                                    ConsistentRead=True)
        if "Item" not in resp:
            return None
        try:
            return _convert(self._plain(resp["Item"]), _NONCE_ATTRS)
        except _Unreadable:
            raise _internal("NONCE# 항목을 읽을 수 없다") from None

    def get_room(self, room_id: str) -> dict | None:
        """ROOM GetItem. {created_at_ms, expires_at_ms, host_peer_id, ttl} 이나 None."""
        resp = self.client.get_item(TableName=self.table, Key=self._key(room_id, ROOM_SK), ConsistentRead=True)
        if "Item" not in resp:
            return None
        try:
            return _convert(self._plain(resp["Item"]), _ROOM_ATTRS)
        except _Unreadable:
            raise _internal("ROOM 항목을 읽을 수 없다") from None

    def query_room(self, room_id: str, lenient: bool = False) -> RoomView:
        """Query(pk = room_id). lenient 면 읽지 못한 PEER# 를 unreadable_peers 로 넘긴다 (4.6 서버 회수).
        그 밖의 판정 불가는 internal 이다. 모르는 sk 도 판정 불가다.
        """
        view = RoomView()
        kwargs = {"TableName": self.table, "KeyConditionExpression": "pk = :pk",
                  "ExpressionAttributeValues": {":pk": {"S": room_id}}, "ConsistentRead": True}
        while True:
            resp = self.client.query(**kwargs)
            for raw in resp.get("Items", []):
                self._absorb(view, self._plain(raw), lenient)
            last = resp.get("LastEvaluatedKey")
            if not last:
                return view
            kwargs["ExclusiveStartKey"] = last

    def _absorb(self, view: RoomView, item: dict, lenient: bool) -> None:
        sk = item.get("sk")
        if not isinstance(sk, str):
            raise _internal("sk 를 읽을 수 없다")
        view.sks.append(sk)
        try:
            kind = sk_kind(sk)
            if kind == ROOM_SK:
                view.room = _convert(item, _ROOM_ATTRS)
            elif kind == "PEER":
                try:
                    peer = _convert_peer(item)
                except _Unreadable:
                    if not lenient:
                        raise
                    view.unreadable_peers.append(_loose_peer(item))
                else:
                    view.peers[peer["peer_id"]] = peer
            elif kind == "VIP":
                view.vips[sk[len("VIP#"):]] = _convert(item, _VIP_ATTRS)["peer_id"]
            else:
                view.pairs[sk] = _convert(item, _PAIR_ATTRS)
        except _Unreadable:
            raise _internal("방 항목을 읽을 수 없다") from None

    # ------------------------------------------------------------ 쓰기 (6.3 쓰기 표)

    def create_room(self, room_id: str, host_peer_id: int, peer_token: str, client_nonce: str,
                    now_ms: int) -> str:
        """create_room 트랜잭션. CREATED, NONCE_EXISTS, ROOM_ID_TAKEN. 그 밖은 internal.

        처음 expires_at_ms 는 now + ROOM_LEASE_S * 1000 이다 (6.3 ROOM 행). store 가 계산한다. 부르는 쪽은
        응답의 expires_in_s 를 get_room 이나 이 값으로 다시 만든다 (같은 공식: created_expires_at).
        """
        expires_at_ms = created_expires_at(now_ms)
        ttl = ttl_for(expires_at_ms)
        room = {"created_at_ms": now_ms, "expires_at_ms": expires_at_ms, "host_peer_id": host_peer_id, "ttl": ttl}
        host = {"peer_id": host_peer_id, "peer_token": peer_token, "virtual_ip": HOST_VIRTUAL_IP,
                "client_nonce": client_nonce, "joined_at_ms": now_ms, "ttl": ttl}
        vip = {"peer_id": host_peer_id, "ttl": ttl}
        nonce = {"room_id": room_id, "peer_id": host_peer_id, "ttl": ttl}
        items = [
            self._put(room_id, ROOM_SK, room, _IF_ABSENT),
            self._put(room_id, peer_sk(host_peer_id), host, _IF_ABSENT),
            self._put(room_id, vip_sk(HOST_VIRTUAL_IP), vip, _IF_ABSENT),
            self._put(nonce_pk(client_nonce), NONCE_SK, nonce, _IF_ABSENT),
        ]
        kind = self._transact("create_room", ROOM_SK, items, (_ROOM_PUT, _PEER, _VIP, _NONCE), _CREATE_PRIORITY)
        return {None: CREATED, _NONCE: NONCE_EXISTS, _ROOM_PUT: ROOM_ID_TAKEN}[kind]

    def join_room(self, room_id: str, virtual_ip: str, peer_id: int, peer_token: str, client_nonce: str,
                  now_ms: int, expires_at_ms: int) -> str:
        """join_room 새 참가의 한 주소. JOINED, NONCE_EXISTS, ROOM_GONE, VIP_TAKEN, PEER_ID_TAKEN.

        expires_at_ms 는 사전 읽기의 ROOM 값이다. 새 항목의 ttl 을 거기서 만든다 (6.5).
        후보 속성(candidates)은 쓰지 않는다. 등록 전 피어는 그 속성이 없다 (6.3 형 변환의 예외 1).
        """
        ttl = ttl_for(expires_at_ms)
        vip = {"peer_id": peer_id, "ttl": ttl}
        peer = {"peer_id": peer_id, "peer_token": peer_token, "virtual_ip": virtual_ip,
                "client_nonce": client_nonce, "joined_at_ms": now_ms, "ttl": ttl}
        nonce = {"room_id": room_id, "peer_id": peer_id, "ttl": ttl}
        items = [
            self._check(room_id, ROOM_SK, _JOIN_ROOM_IF, {":now": now_ms}),
            self._put(room_id, vip_sk(virtual_ip), vip, _IF_ABSENT),
            self._put(room_id, peer_sk(peer_id), peer, _IF_ABSENT),
            self._put(nonce_pk(client_nonce), NONCE_SK, nonce, _IF_ABSENT),
        ]
        kind = self._transact("join_room", vip_sk(virtual_ip), items, (_ROOM_CHECK, _VIP, _PEER, _NONCE),
                              _JOIN_PRIORITY)
        return {None: JOINED, _NONCE: NONCE_EXISTS, _ROOM_CHECK: ROOM_GONE, _VIP: VIP_TAKEN,
                _PEER: PEER_ID_TAKEN}[kind]

    def register_candidates(self, room_id: str, peer_id: int, peer_token: str, candidates: list[dict]) -> bool:
        """register_candidate 의 PEER# update. 조건이 실패하면 False 이고 부르는 쪽이 unauthorized 로 끝낸다."""
        sk = peer_sk(peer_id)
        return self._write("register_candidates", sk, lambda: self.client.update_item(
            TableName=self.table, Key=self._key(room_id, sk), UpdateExpression="SET candidates = :list",
            ConditionExpression=_REGISTER_IF, ExpressionAttributeValues=self._values({":list": candidates,
                                                                                  ":t": peer_token})))

    def release_peer(self, room_id: str, peer_id: int, virtual_ip: str, host_peer_id: int,
                     client_nonce: str | None = None) -> bool:
        """host_report 회수 (departed). PEER#, 그 VIP#, 그 쌍의 PAIR#, 그 피어의 NONCE# 를 지운다.
        이미 없던 대상이면 False.

        NONCE# 는 서버 회수와 같이 조건 없는 delete 다. 그래서 회수된 피어가 같은 nonce 로 다시 오면 새
        참가다. client_nonce 가 None 이면 부르는 쪽이 PEER# 의 client_nonce 를 읽지 못한 것이다. 그때는
        NONCE# 만 건너뛰고 나머지는 지운다 (디렉터 결정. 피어를 남기는 것보다 낫다).
        """
        sk = peer_sk(peer_id)
        items = [
            self._delete(room_id, sk, _IF_EXISTS),
            self._delete(room_id, vip_sk(virtual_ip)),
            self._delete(room_id, ops.pair_sk(host_peer_id, peer_id)),
        ]
        layout = (_PEER, _OTHER, _OTHER)
        if client_nonce is not None:
            items.append(self._delete(nonce_pk(client_nonce), NONCE_SK))
            layout = (*layout, _OTHER)
        return self._transact("release_peer", sk, items, layout, _DELETE_PRIORITY) is None

    def reclaim_peer(self, room_id: str, peer_id: int, virtual_ip: str, client_nonce: str, now_ms: int) -> bool:
        """host_report 서버 회수. PEER#, 그 VIP#, 그 NONCE# 를 지운다. 조건이 실패하면 False
        (그 사이 등록했거나, 유예가 안 지났거나, 이미 없다).
        """
        sk = peer_sk(peer_id)
        cutoff = now_ms - JOIN_REGISTER_GRACE_S * 1000
        items = [
            self._delete(room_id, sk, _RECLAIM_IF, {":zero": 0, ":cutoff": cutoff}),
            self._delete(room_id, vip_sk(virtual_ip)),
            self._delete(nonce_pk(client_nonce), NONCE_SK),
        ]
        return self._transact("reclaim_peer", sk, items, (_PEER, _OTHER, _OTHER), _DELETE_PRIORITY) is None

    def confirm_pair(self, room_id: str, host_peer_id: int, other_peer_id: int, ready_at_wall_ms: int,
                     ready_at_mono_ns: int, ready_boot_id: str, expires_at_ms: int) -> str:
        """host_report 확인. CONFIRMED, ALREADY_CONFIRMED, NOT_ELIGIBLE. 셋 다 오류가 아니다.

        expires_at_ms 는 첫 Query 의 ROOM 값이다. PAIR# 의 ttl 을 거기서 만든다 (6.5).
        """
        sk = ops.pair_sk(host_peer_id, other_peer_id)
        pair = {"ready_at_wall_ms": ready_at_wall_ms, "ready_at_mono_ns": ready_at_mono_ns,
                "ready_boot_id": ready_boot_id, "punch_delay_ms": PUNCH_DELAY_MS, "ttl": ttl_for(expires_at_ms)}
        items = [
            self._check(room_id, peer_sk(host_peer_id), _CONFIRM_HOST_IF, {":zero": 0}),
            self._check(room_id, peer_sk(other_peer_id), _CONFIRM_OTHER_IF, {":zero": 0}),
            self._put(room_id, sk, pair, _IF_ABSENT),
        ]
        kind = self._transact("confirm_pair", sk, items, (_PEER_CHECK, _PEER_CHECK, _PAIR), _CONFIRM_PRIORITY)
        return {None: CONFIRMED, _PEER_CHECK: NOT_ELIGIBLE, _PAIR: ALREADY_CONFIRMED}[kind]

    def renew(self, room_id: str, now_ms: int, new_expires_at_ms: int, sks: list[str]) -> bool:
        """host_report 갱신. ROOM update 를 먼저 쓰고, 성공하면 나머지 항목의 ttl 을 하나씩 민다.

        ROOM 조건이 실패하면 False(room_expired)이고 나머지를 쓰지 않는다. 나머지 항목은 각각
        attribute_exists(pk) 이고 실패하면 그 사이 지워진 항목이므로 무시한다. 없는 항목을 만들지 않는다.
        sks 는 첫 Query 결과에서 회수로 지운 항목을 뺀 것이다. new_expires_at_ms 는 부르는 쪽이
        now + ROOM_LEASE_S * 1000 으로 만든다 (4.6 처리 순서 4번).
        """
        ttl = ttl_for(new_expires_at_ms)
        names = {"#ttl": "ttl"}
        room_values = self._values({":new": new_expires_at_ms, ":ttl": ttl, ":now": now_ms})
        renewed = self._write("renew_room", ROOM_SK, lambda: self.client.update_item(
            TableName=self.table, Key=self._key(room_id, ROOM_SK), UpdateExpression="SET expires_at_ms = :new, #ttl = :ttl",
            ConditionExpression=_RENEW_ROOM_IF, ExpressionAttributeNames=names, ExpressionAttributeValues=room_values))
        if not renewed:
            return False
        item_values = self._values({":ttl": ttl})
        for sk in sks:
            if sk == ROOM_SK:
                continue
            self._renew_item(room_id, sk, names, item_values)
        return True

    def _renew_item(self, room_id: str, sk: str, names: dict, values: dict) -> bool:
        """갱신의 나머지 항목 하나. 지워진 항목이면 False 이고 무시한다. 다음 항목으로 계속 간다."""
        return self._write("renew_item", sk, lambda: self.client.update_item(
            TableName=self.table, Key=self._key(room_id, sk), UpdateExpression="SET #ttl = :ttl",
            ConditionExpression=_RENEW_ITEM_IF, ExpressionAttributeNames=names, ExpressionAttributeValues=values))
