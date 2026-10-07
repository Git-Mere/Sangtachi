"""4장 연산 다섯. 출처는 control_plane.md 4장 연산, 5장 상태 전이, 7.2 ops.dispatch 의 계약, 7.3 연산 처리
순서다.

앞쪽은 판정만 하는 순수한 함수다. 저장된 항목(6.3 항목의 dict)과 now 를 받는다. 케이스 표는
tests/test_room_state.py, tests/test_get_peers.py, tests/test_host_report.py 에 있다.

뒤쪽의 Service 가 연산 다섯을 7.3 순서대로 돈다. 저장소(store.Store)와 시계는 밖에서 받는다. 표는
tests/test_ops.py 와 tests/test_host_report.py 의 store 행이다. 변이는 tests/mutants/ops.py.
"""

from __future__ import annotations

import hmac
import threading
from collections.abc import Callable

from controlplane import clock, ids, log
from controlplane import store as storage
from controlplane.candidates import sanitize_candidates
from controlplane.constants import (
    JOIN_REGISTER_GRACE_S,
    MAX_PEER_ID_ATTEMPTS,
    MAX_PEERS,
    MAX_ROOM_ID_ATTEMPTS,
    PUNCH_DELAY_MS,
    ROOM_LEASE_S,
)
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
    -> 경계(지나지 않았으면 KEEP) -> 지울 키(virtual_ip, client_nonce) 판독. candidates 속성이 없으면
    빈 것이다 (6.3 서버 회수 조건과 같다).
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
    if joined_at_ms + JOIN_REGISTER_GRACE_S * 1000 > now_ms:
        return KEEP, None
    if not isinstance(peer.get("virtual_ip"), str) or not isinstance(peer.get("client_nonce"), str):
        return SKIP, "keys_unreadable"
    return RECLAIM, None


# ---------------------------------------------------------------- 연산 다섯 (7.2, 7.3)
#
# Service.dispatch 가 7.2 의 ops.dispatch 다. 성공이면 연산별 필드의 dict 를 돌려주고("ok" 는 서버가
# 붙인다), 오류이면 OpError 를 던진다. 연산마다 7.3 의 순서를 지킨다.
#   1 필드와 형 -> 2 room_id 정규화 -> 3 필드 조합 -> 4 저장소 읽기 -> 5 방 존재 -> 6 방 만료
#   -> 7 토큰(상수 시간 비교, 쓰기 전) -> 8 본체 -> 9 응답
# 저장소의 쓰기 메서드는 한 번만 시도한다. 풀 순회, 재추첨, NONCE# 다시 읽기는 여기서 한다.

_UINT32_MAX = 2**32 - 1

# 2.5 참가자 풀. 낮은 주소부터 10.100.0.(MAX_PEERS) 까지. 순회의 시도 상한이 곧 풀 크기다.
PARTICIPANT_POOL = tuple(f"10.100.0.{n}" for n in range(2, MAX_PEERS + 1))


def _bad(message: str) -> OpError:
    # message 는 고정 문자열이다. 요청 값을 넣지 않는다 (4.1).
    return OpError("bad_request", message)


def _uint32(body: dict, name: str) -> int:
    """3.3 인코딩. 정수는 JSON number 이고 소수점 없음, uint32 는 0 이상 4294967295 이하.
    bool 과 float 는 정수가 아니다. 그래서 type 으로 본다.
    """
    value = body.get(name)
    if type(value) is not int or value < 0 or value > _UINT32_MAX:
        raise _bad(f"{name} 는 uint32 정수다")
    return value


def _peer_id_list(body: dict, name: str) -> list[int]:
    """4.6 의 departed, confirm. 생략이면 빈 배열이다. 배열이고 길이 MAX_PEERS - 1 이하, 원소는 uint32."""
    if name not in body:
        return []
    value = body[name]
    if type(value) is not list:
        raise _bad(f"{name} 는 배열이다")
    if len(value) > MAX_PEERS - 1:
        raise _bad(f"{name} 가 너무 길다")
    for item in value:
        if type(item) is not int or item < 0 or item > _UINT32_MAX:
            raise _bad(f"{name} 의 원소는 uint32 정수다")
    return list(value)


def _check_room(room: dict | None, now_ms: int) -> None:
    """7.3 의 5번과 6번. 없으면 room_not_found, 만료면 room_expired."""
    state = room_state(room, now_ms)
    if state is None:
        raise OpError("room_not_found")
    if state != ROOM_OPEN:
        raise OpError("room_expired")


def _issued(room_id: str, peer: dict, room: dict, now_ms: int) -> dict:
    """4.2, 4.3 응답. 처음 발급한 값과 그때의 남은 임대."""
    return {"room_id": room_id, "peer_id": peer["peer_id"], "peer_token": peer["peer_token"],
            "virtual_ip": peer["virtual_ip"], "expires_in_s": expires_in_s(room["expires_at_ms"], now_ms)}


class Service:
    """연산 다섯. 방·피어 상태를 들고 있지 않는다 (6.1 저장 5). 갖는 것은 저장소와 시계와 카운터뿐이다.

    now_ms, mono_ns, boot_id 는 인자 없는 함수다. 기본값은 7.4 시계의 세 값이다. 시험이 주입한다.
    카운터는 elapsed_wall_fallback 하나이고 서버가 counters() 로 읽어 7.5 counter 줄을 낸다.
    """

    def __init__(self, store, *, now_ms: Callable[[], int] | None = None,
                 mono_ns: Callable[[], int] | None = None, boot_id: Callable[[], str] | None = None) -> None:
        self.store = store
        self._now_ms = clock.now_wall_ms if now_ms is None else now_ms
        self._mono_ns = clock.now_mono_ns if mono_ns is None else mono_ns
        self._boot_id = clock.current_boot_id if boot_id is None else boot_id
        self._lock = threading.Lock()
        self._counters = {clock.FALLBACK_COUNTER: 0}
        self._handlers = {
            "create_room": self._create_room,
            "join_room": self._join_room,
            "register_candidate": self._register_candidate,
            "get_peers": self._get_peers,
            "host_report": self._host_report,
        }

    def counters(self) -> dict[str, int]:
        """카운터의 사본. 7.5 의 elapsed_wall_fallback."""
        with self._lock:
            return dict(self._counters)

    def dispatch(self, req) -> dict:
        """7.2 ops.dispatch 의 계약. req 는 .op 와 .body 를 갖는다."""
        handler = self._handlers.get(req.op) if isinstance(req.op, str) else None
        if handler is None:
            raise OpError("unknown_op")
        if not isinstance(req.body, dict):
            raise _bad("본문은 JSON 객체다")
        return handler(req.body)

    # ------------------------------------------------------------ 4.2 create_room

    def _create_room(self, body: dict) -> dict:
        nonce = ids.check_client_nonce(body.get("client_nonce"))
        now = self._now_ms()
        found = self.store.get_nonce(nonce)  # 6.3 읽기 표. 멱등성 조회가 먼저다
        if found is not None:
            return self._replay(found, now)
        host_id = ids.new_peer_id()
        token = ids.new_peer_token()
        for _ in range(MAX_ROOM_ID_ATTEMPTS):
            room_id = ids.new_room_id()
            result = self.store.create_room(room_id, host_id, token, nonce, now)
            if result == storage.CREATED:
                log.emit("INFO", "room.created", peer_id=host_id, virtual_ip=storage.HOST_VIRTUAL_IP)
                return {"room_id": room_id, "peer_id": host_id, "peer_token": token,
                        "virtual_ip": storage.HOST_VIRTUAL_IP,
                        "expires_in_s": expires_in_s(storage.created_expires_at(now), now)}
            if result == storage.NONCE_EXISTS:  # 6.3 취소 사유 1번
                return self._replay(self._reread_nonce(nonce), now)
            if result != storage.ROOM_ID_TAKEN:  # 5번. 그 밖은 모르는 결과다
                raise OpError("internal", "create_room 의 결과를 판정할 수 없다")
        raise OpError("internal", "room_id 재추첨 상한에 닿았다")

    # ------------------------------------------------------------ 4.3 join_room

    def _join_room(self, body: dict) -> dict:
        nonce = ids.check_client_nonce(body.get("client_nonce"))
        room_id = ids.normalize_room_id(body.get("room_id"))
        now = self._now_ms()
        found = self.store.get_nonce(nonce)
        if found is not None:
            return self._replay_join(found, room_id, now)
        room = self.store.get_room(room_id)
        _check_room(room, now)
        peer_id = ids.new_peer_id()
        token = ids.new_peer_token()
        drawn = 1
        index = 0
        while index < len(PARTICIPANT_POOL):
            vip = PARTICIPANT_POOL[index]
            result = self.store.join_room(room_id, vip, peer_id, token, nonce, now, room["expires_at_ms"])
            if result == storage.JOINED:
                log.emit("INFO", "vip.claimed", virtual_ip=vip, peer_id=peer_id)
                return {"room_id": room_id, "peer_id": peer_id, "peer_token": token, "virtual_ip": vip,
                        "expires_in_s": expires_in_s(room["expires_at_ms"], now)}
            if result == storage.NONCE_EXISTS:  # 1번. 같은 nonce 가 먼저 성립했다
                return self._replay_join(self._reread_nonce(nonce), room_id, now)
            if result == storage.ROOM_GONE:  # 2번. 다음 주소로 넘어가지 않는다
                if self.store.get_room(room_id) is None:
                    raise OpError("room_not_found")
                raise OpError("room_expired")
            if result == storage.VIP_TAKEN:  # 3번. 다음 주소
                index += 1
            elif result == storage.PEER_ID_TAKEN:  # 4번. peer_id 재추첨
                drawn += 1
                if drawn > MAX_PEER_ID_ATTEMPTS:
                    raise OpError("internal", "peer_id 재추첨 상한에 닿았다")
                peer_id = ids.new_peer_id()
            else:
                raise OpError("internal", "join_room 의 결과를 판정할 수 없다")
        raise OpError("room_full")

    def _replay_join(self, found: dict, room_id: str, now: int) -> dict:
        if found["room_id"] != room_id:  # nonce 를 다른 방에 재사용했다 (6.3 읽기 표, 취소 사유 1번)
            raise _bad("client_nonce 가 다른 방의 것이다")
        return self._replay(found, now)

    def _reread_nonce(self, nonce: str) -> dict:
        found = self.store.get_nonce(nonce)
        if found is None:
            raise OpError("internal", "NONCE# 를 다시 읽지 못했다")
        return found

    def _replay(self, found: dict, now: int) -> dict:
        """4.2, 4.3 멱등성. NONCE# 의 room_id 로 Query 해 처음 발급한 것을 다시 조립한다."""
        view = self.store.query_room(found["room_id"])
        _check_room(view.room, now)
        peer = view.peers.get(found["peer_id"])
        if peer is None:
            raise OpError("internal", "NONCE# 가 가리키는 피어가 방에 없다")
        return _issued(found["room_id"], peer, view.room, now)

    # ------------------------------------------------------------ 4.4 register_candidate

    def _register_candidate(self, body: dict) -> dict:
        peer_id = _uint32(body, "peer_id")
        token = ids.check_peer_token(body.get("peer_token"))
        stored, rejected = sanitize_candidates(body.get("candidates"))
        room_id = ids.normalize_room_id(body.get("room_id"))
        now = self._now_ms()
        view = self.store.query_room(room_id)
        _check_room(view.room, now)
        self._authenticate(view, peer_id, token)
        if not self.store.register_candidates(room_id, peer_id, token, stored):
            raise OpError("unauthorized")  # 토큰 검사 뒤 회수됐다 (4.6 서버 회수)
        return {"accepted": len(stored), "rejected": rejected}

    # ------------------------------------------------------------ 4.5 get_peers

    def _get_peers(self, body: dict) -> dict:
        peer_id = _uint32(body, "peer_id")
        token = ids.check_peer_token(body.get("peer_token"))
        room_id = ids.normalize_room_id(body.get("room_id"))
        now = self._now_ms()
        view = self.store.query_room(room_id)
        _check_room(view.room, now)
        self._authenticate(view, peer_id, token)
        host_id = view.room["host_peer_id"]
        if peer_id == host_id:
            # 문서가 정하지 않았다 (보고서의 DOC GAP). 4.6 응답의 peers 와 같이 자신을 뺀 전부를 준다.
            others = [p for pid, p in view.peers.items() if pid != peer_id]
        else:
            host = view.peers.get(host_id)
            if host is None:
                raise OpError("internal", "방에 호스트의 PEER# 가 없다")
            others = [host]
        elements = self._elements(view, peer_id, others)
        return {"ready": any(e["ready"] for e in elements), "peers": elements}

    # ------------------------------------------------------------ 4.6 host_report

    def _host_report(self, body: dict) -> dict:
        peer_id = _uint32(body, "peer_id")
        token = ids.check_peer_token(body.get("peer_token"))
        departed = _peer_id_list(body, "departed")
        confirm = _peer_id_list(body, "confirm")
        room_id = ids.normalize_room_id(body.get("room_id"))
        now = self._now_ms()
        view = self.store.query_room(room_id, lenient=True)  # 서버 회수가 읽지 못한 PEER# 도 본다
        _check_room(view.room, now)
        # 1. 호출자 판정. 호스트의 peer_id 이고 토큰이 그 피어의 것이어야 한다.
        host_id = view.room["host_peer_id"]
        if peer_id != host_id:
            raise OpError("unauthorized")
        host = self._authenticate(view, peer_id, token)
        # 2 -> 3 -> 4 -> 5. 순서가 규칙이다 (4.6 처리 순서).
        released: list[int] = []
        removed: set[str] = set()   # 2번이 지운 sk. 4번 갱신의 목록에서 뺀다
        self._release_departed(room_id, view, host_id, departed, released, removed)
        self._reclaim(room_id, view, host_id, now, {*departed, *released}, released, removed)
        confirmed = self._confirm(room_id, view, host, confirm, skip={*departed, *released})
        self._renew(room_id, view, host_id, removed, now)
        after = self.store.query_room(room_id)  # 5. 쓰기 뒤에 다시 읽어 응답을 만든다
        if after.room is None:
            raise OpError("internal", "갱신한 방을 다시 읽지 못했다")
        others = [p for pid, p in after.peers.items() if pid != host_id]
        return {"expires_in_s": expires_in_s(after.room["expires_at_ms"], now), "released": released,
                "confirmed": confirmed, "peers": self._elements(after, host_id, others)}

    def _release_departed(self, room_id: str, view, host_id: int, departed: list[int], released: list[int],
                          removed: set[str]) -> None:
        """4.6 처리 순서 2번의 앞쪽. departed 회수. 지운 피어를 released 와 removed 에 더한다."""
        for target in departed:
            if target == host_id:
                continue  # 문서가 정하지 않았다 (보고서의 DOC GAP). 호출자 자신은 지우지 않는다
            found = self._departed_target(view, target)
            if found is None:
                continue  # 이미 없던 대상. released 에 담지 않는다
            vip, nonce = found
            if self.store.release_peer(room_id, target, vip, host_id, nonce):
                released.append(target)
                removed.update({storage.peer_sk(target), storage.vip_sk(vip), pair_sk(host_id, target)})
                log.emit("INFO", "peer.released", peer_id=target, virtual_ip=vip, by="host")

    def _reclaim(self, room_id: str, view, host_id: int, now: int, skip: set, released: list[int],
                 removed: set[str]) -> None:
        """4.6 처리 순서 2번의 뒤쪽. 서버 회수. skip 은 departed 가 이미 다룬 피어다. 호스트의 통지가 이긴다."""
        for peer in [*view.peers.values(), *view.unreadable_peers]:
            target = peer.get("peer_id")
            if _is_int(target) and target in skip:  # 판정 순서 1번이 먼저다. 리스트나 맵은 해시할 수 없다
                continue
            verdict, reason = reclaim_verdict(peer, host_id, now)
            if verdict == SKIP:
                log.emit("WARN", "peer.reclaim_skipped", peer_id=target if _is_int(target) else "", reason=reason)
            elif verdict == RECLAIM:
                vip = peer["virtual_ip"]
                if self.store.reclaim_peer(room_id, target, vip, peer["client_nonce"], now):
                    released.append(target)
                    removed.update({storage.peer_sk(target), storage.vip_sk(vip)})
                    log.emit("INFO", "peer.released", peer_id=target, virtual_ip=vip, by="server")

    def _departed_target(self, view, target: int) -> tuple[str, str | None] | None:
        """departed 회수의 (virtual_ip, client_nonce). 읽은 방에 없으면 None.

        PEER# 를 읽지 못했으면 판정 불가이고 internal 이다 (6.3). 예외는 하나다. 읽지 못한 것이
        client_nonce 이고 virtual_ip 는 문자열이면 NONCE# 만 건너뛰고 지운다 (디렉터 결정).
        """
        peer = view.peers.get(target)
        if peer is not None:
            return peer["virtual_ip"], peer["client_nonce"]
        if storage.peer_sk(target) not in view.sks:
            return None
        for loose in view.unreadable_peers:
            if _is_int(loose.get("peer_id")) and loose["peer_id"] == target \
                    and not isinstance(loose.get("client_nonce"), str) and isinstance(loose.get("virtual_ip"), str):
                return loose["virtual_ip"], None
        raise OpError("internal", "PEER# 항목을 읽을 수 없다")

    def _confirm(self, room_id: str, view, host: dict, confirm: list[int], skip: set[int]) -> list[int]:
        """4.6 처리 순서 3번. 사전 판정은 1번에서 읽은 값으로 하고, 경합은 저장소 조건이 막는다.

        skip 은 2번이 처리한 피어다(departed 의 대상과 2번이 실제로 지운 피어). 회수가 이긴다(4.6). 1번에서
        읽은 낡은 값으로 그 피어를 다시 판정하지 않는다. 그 PEER# 를 읽지 못했어도 여기서 끝내지 않는다.
        """
        host_id = host["peer_id"]
        confirmed: list[int] = []
        for target in confirm:
            if target in skip:
                continue  # 2번이 이겼다
            if target == host_id:
                continue  # 문서가 정하지 않았다 (보고서의 DOC GAP)
            other = self._known_peer(view, target)
            if not confirm_allowed(host, other, view.pairs.get(pair_sk(host_id, target))):
                continue
            result = self.store.confirm_pair(room_id, host_id, target, self._now_ms(), self._mono_ns(),
                                             self._boot_id(), view.room["expires_at_ms"])
            if result == storage.CONFIRMED:
                confirmed.append(target)
                log.emit("INFO", "pair.ready", host_peer_id=host_id, peer_id=target, punch_delay_ms=PUNCH_DELAY_MS)
            elif result not in (storage.ALREADY_CONFIRMED, storage.NOT_ELIGIBLE):
                raise OpError("internal", "확인의 결과를 판정할 수 없다")
        return confirmed

    def _renew(self, room_id: str, view, host_id: int, removed: set[str], now: int) -> None:
        """4.6 처리 순서 4번. 항목 목록은 첫 Query 결과에서 2번이 지운 항목을 뺀 것이다 (6.3)."""
        new_expires = now + ROOM_LEASE_S * 1000
        sks = [sk for sk in view.sks if sk not in removed]
        if not self.store.renew(room_id, now, new_expires, sks):
            raise OpError("room_expired")
        log.emit("INFO", "room.renewed", host_peer_id=host_id, expires_in_s=expires_in_s(new_expires, now))

    # ------------------------------------------------------------ 공통

    def _known_peer(self, view, target: int) -> dict | None:
        """읽은 방의 PEER#. 없으면 None. 있는데 읽지 못했으면 판정 불가이고 internal 이다 (6.3)."""
        peer = view.peers.get(target)
        if peer is None and storage.peer_sk(target) in view.sks:
            raise OpError("internal", "PEER# 항목을 읽을 수 없다")
        return peer

    def _authenticate(self, view, peer_id: int, token: str) -> dict:
        """7.3 의 7번. 저장소에서 읽은 토큰과 상수 시간으로 비교한다 (6.3). 없는 피어도 unauthorized 다."""
        peer = self._known_peer(view, peer_id)
        if peer is None:
            raise OpError("unauthorized")
        stored = peer["peer_token"].encode("utf-8")
        if not hmac.compare_digest(stored, token.encode("ascii")):
            raise OpError("unauthorized")
        return peer

    def _elements(self, view, caller: int, others: list[dict]) -> list[dict]:
        """4.5 peers 원소. 쌍은 호출자와 그 원소의 피어다. 경과는 7.4 시계로 응답을 만드는 시점에 잰다."""
        boot = self._boot_id()
        mono = self._mono_ns()
        wall = self._now_ms()
        local: dict[str, int] = {}

        def elapsed(pair: dict) -> int:
            return clock.elapsed_since_ready_ms(pair, boot, mono, wall, local)[0]

        out = [peer_element(p, view.pairs.get(pair_sk(caller, p["peer_id"])), elapsed) for p in others]
        with self._lock:
            for name, value in local.items():
                self._counters[name] = self._counters.get(name, 0) + value
        return out
