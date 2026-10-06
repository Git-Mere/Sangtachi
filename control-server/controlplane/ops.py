"""4장 연산의 순수한 부분. 저장소와 시계를 부르지 않는다.

저장된 항목(control_plane.md 6.3 항목의 dict)과 now 를 받아 판정만 한다. store.py 는 아직 없다.
clock.py 를 import 하지 않고, 시계 값이 필요한 자리는 부르는 쪽이 넘기는 함수를 받는다.
"""

from __future__ import annotations

from collections.abc import Callable

from controlplane.constants import JOIN_REGISTER_GRACE_S
from controlplane.errors import OpError

ROOM_OPEN = "open"
ROOM_EXPIRED = "expired"

RECLAIM = "reclaim"
KEEP = "keep"
SKIP = "skip"


def _is_int(value: object) -> bool:
    # bool 은 int 의 하위형이라 따로 뺀다.
    return isinstance(value, int) and not isinstance(value, bool)


def room_state(room: dict | None, now_ms: int) -> str | None:
    """5.1 방의 상태. ROOM 항목이 없으면 None 이다 (표의 "(없음)").

    expires_at_ms 가 없거나 정수가 아니면 판정 불가이고 internal 이다. 통과로 바꾸지 않는다.
    """
    if room is None:
        return None
    expires_at_ms = room.get("expires_at_ms")
    if not _is_int(expires_at_ms):
        raise OpError("internal", "방 항목의 expires_at_ms 를 읽을 수 없다")
    if now_ms < expires_at_ms:
        return ROOM_OPEN
    return ROOM_EXPIRED


def expires_in_s(expires_at_ms: int, now_ms: int) -> int:
    """5.1 의 max(0, ceil((expires_at_ms - now) / 1000)). 부동소수점을 쓰지 않는다."""
    left_ms = expires_at_ms - now_ms
    return max(0, -(-left_ms // 1000))


def pair_sk(a: int, b: int) -> str:
    """6.3 PAIR#<lo>-<hi>. 두 peer_id 를 수의 오름차순으로 10진수로 붙인다."""
    lo, hi = sorted((a, b))
    return f"PAIR#{lo}-{hi}"


def peer_element(peer: dict, pair: dict | None, elapsed_ms: Callable[[dict], int]) -> dict:
    """4.5 peers 원소 하나. pair 는 그 쌍의 PAIR# 항목이고 없으면 None 이다.

    ready 는 PAIR# 의 존재만으로 정한다. 후보 수를 다시 세지 않는다. 나머지 세 필드는 ready 일
    때만 넣는다. elapsed_ms 는 PAIR# 항목을 받아 7.4 시계의 경과를 돌려주는 함수다.
    """
    element = {"peer_id": peer["peer_id"], "virtual_ip": peer["virtual_ip"], "ready": pair is not None}
    if pair is None:
        return element
    element["punch_delay_ms"] = pair["punch_delay_ms"]
    element["elapsed_since_ready_ms"] = elapsed_ms(pair)
    element["candidates"] = [dict(c) for c in peer["candidates"]]
    return element


def has_candidates(peer: dict | None) -> bool:
    """후보가 1개 이상 있는가 (5.3 registered). 모양을 모르면 거짓이다."""
    if peer is None:
        return False
    candidates = peer.get("candidates")
    return isinstance(candidates, list) and len(candidates) > 0


def confirm_allowed(host: dict | None, other: dict | None, pair: dict | None) -> bool:
    """4.6 처리 순서 3번의 조건. 둘 다 후보가 있고 그 쌍의 PAIR# 가 아직 없다.

    읽은 값으로 본 사전 판정이다. 경합은 store 의 조건부 쓰기(6.3)가 막는다.
    """
    if pair is not None:
        return False
    return has_candidates(host) and has_candidates(other)


def reclaim_verdict(peer: dict, host_peer_id: int, now_ms: int) -> tuple[str, str | None]:
    """4.6 서버 회수 대상인가. (RECLAIM | KEEP | SKIP, SKIP 의 사유).

    SKIP 은 판정 불가라 남기는 것이다. 부르는 쪽이 7.5 peer.reclaim_skipped 를 남긴다.
    판정 순서: peer_id 판독 -> 호스트면 KEEP -> 후보 판독 -> 후보 있으면 KEEP -> joined_at_ms 판독
    -> 경계. candidates 속성이 없으면 빈 것이다 (6.3 서버 회수 조건과 같다).
    """
    peer_id = peer.get("peer_id")
    if not _is_int(peer_id):
        return SKIP, "peer_id_unreadable"
    if peer_id == host_peer_id:
        return KEEP, None
    candidates = peer.get("candidates", [])
    if not isinstance(candidates, list):
        return SKIP, "candidates_unreadable"
    if candidates:
        return KEEP, None
    joined_at_ms = peer.get("joined_at_ms")
    if not _is_int(joined_at_ms):
        return SKIP, "joined_at_unreadable"
    if joined_at_ms + JOIN_REGISTER_GRACE_S * 1000 <= now_ms:
        return RECLAIM, None
    return KEEP, None
