"""control_plane.md 6.3 항목과 6.5 TTL 의 저장소 계층. store.py 를 DynamoDB local 에서 돌린다.

표는 이 파일이 출처다. 행을 고치려면 여기를 고친다. 변이는 tests/mutants/store.py.

store 표시가 있는 시험은 --store 를 줄 때만 돈다 (README). 표시 없는 것(취소 사유의 순수 판정,
설정, 하네스의 루프백 검사)은 늘 돈다.

문서 표는 행마다 한 원소로 옮겼다(WRITES, CANCEL_ORDER, READS, TTL). 그 행을 돌리는 시나리오는
따로 두고 row 필드로 문서 행을 가리킨다. test_every_doc_row_is_exercised 가 빠진 행을 센다.
"""

import os
import subprocess
import sys
import time
from decimal import Decimal
from pathlib import Path

import pytest

from conftest import FAKE_KEY_ID, NO_PROXY_VALUE, PROXY_VARS, isolate_aws, loopback_endpoint, no_proxy_config, proxy_for
from controlplane import store as S
from controlplane.constants import JOIN_REGISTER_GRACE_S, PUNCH_DELAY_MS, ROOM_LEASE_S, STORAGE_GRACE_S
from controlplane.errors import OpError
from controlplane.ops import KEEP, RECLAIM, SKIP, reclaim_verdict

CP_ROOT = Path(__file__).resolve().parents[1]

# 시각은 실제 벽시계 근처여야 한다. DynamoDB local 은 TTL 삭제를 실제로 돌리므로 ttl 이 과거면
# 시험 도중 항목이 사라진다. 초 경계에서 999ms 떨어진 값으로 맞춰 6.5 의 내림을 본다.
NOW = (time.time_ns() // 1_000_000 // 1000) * 1000 + 999
G = JOIN_REGISTER_GRACE_S * 1000
EXP = NOW + 120_000                    # 방을 만들 때의 만료 시각 (6.3 ROOM 행, ROOM_LEASE_S). EXP % 1000 == 999
EXP_TTL = (NOW // 1000) + 120 + STORAGE_GRACE_S  # 6.5 를 손으로 편 값. 내림이라 999 가 버려진다
assert ROOM_LEASE_S == 120  # 위 두 값은 손으로 편 것이다. 상수가 바뀌면 같이 고친다

R = "QWERTY"
H, P, Q = 11, 22, 33
TOK_H, TOK_P, TOK_Q = "1" * 32, "2" * 32, "3" * 32
NON_H, NON_P, NON_Q = "a" * 32, "b" * 32, "c" * 32
VIP_H, VIP_P, VIP_Q = "10.100.0.1", "10.100.0.2", "10.100.0.3"
CAND = [{"ip": "203.0.113.7", "port": 51000, "kind": "reflexive"}]
CAND2 = [{"ip": "192.168.0.10", "port": 40000, "kind": "local"}, {"ip": "203.0.113.9", "port": 1, "kind": "reflexive"}]
OMIT = object()

# ---------------------------------------------------------------- 시드 항목 (6.3 항목 표의 모양)


def room_item(exp=EXP, **over):
    return _item(R, "ROOM", dict(created_at_ms=NOW - 1000, expires_at_ms=exp, host_peer_id=H, ttl=EXP_TTL), over)


def peer_item(pid, vip, token, nonce, candidates=OMIT, joined=NOW - 1000, **over):
    base = dict(peer_id=pid, peer_token=token, virtual_ip=vip, client_nonce=nonce, joined_at_ms=joined,
                ttl=EXP_TTL, candidates=candidates)
    return _item(R, f"PEER#{pid}", base, over)


def host_item(**over):
    return peer_item(H, VIP_H, TOK_H, NON_H, **over)


def p_item(**over):
    return peer_item(P, VIP_P, TOK_P, NON_P, **over)


def vip_item(ip, pid, **over):
    return _item(R, f"VIP#{ip}", dict(peer_id=pid, ttl=EXP_TTL), over)


def pair_item(a, b, **over):
    lo, hi = sorted((a, b))
    base = dict(ready_at_wall_ms=NOW - 500, ready_at_mono_ns=123, ready_boot_id="boot-x", punch_delay_ms=777,
                ttl=EXP_TTL)
    return _item(R, f"PAIR#{lo}-{hi}", base, over)


def nonce_item(nonce, pid, room=R, **over):
    return _item(f"NONCE#{nonce}", "NONCE", dict(room_id=room, peer_id=pid, ttl=EXP_TTL), over)


def _item(pk, sk, base, over):
    base = dict(base, **over)
    return {"pk": pk, "sk": sk, **{k: v for k, v in base.items() if v is not OMIT}}


def seed(st, items):
    """조건 없는 put. 시험의 사전 상태를 만든다. 제품 코드의 쓰기 경로를 거치지 않는다."""
    for it in items:
        st.client.put_item(TableName=st.table, Item=st._values(it))


def raw(st, pk, sk):
    resp = st.client.get_item(TableName=st.table, Key={"pk": {"S": pk}, "sk": {"S": sk}}, ConsistentRead=True)
    return st._plain(resp["Item"]) if "Item" in resp else None


def run(st, call):
    method, kwargs = call
    return getattr(st, method)(**kwargs)


def outcome(st, call):
    try:
        return run(st, call)
    except OpError as exc:
        return f"OpError:{exc.code}"


# 자주 쓰는 호출
def create(nonce=NON_H, room=R, pid=H):
    return ("create_room", dict(room_id=room, host_peer_id=pid, peer_token=TOK_H, client_nonce=nonce, now_ms=NOW))


def join(vip=VIP_P, pid=P, nonce=NON_P, now=NOW):
    return ("join_room", dict(room_id=R, virtual_ip=vip, peer_id=pid, peer_token=TOK_P, client_nonce=nonce,
                              now_ms=now, expires_at_ms=EXP))


def register(pid=P, token=TOK_P, cands=CAND2):
    return ("register_candidates", dict(room_id=R, peer_id=pid, peer_token=token, candidates=cands))


def release(pid=P, vip=VIP_P):
    return ("release_peer", dict(room_id=R, peer_id=pid, virtual_ip=vip, host_peer_id=H))


def reclaim(pid=P, vip=VIP_P, nonce=NON_P, now=NOW):
    return ("reclaim_peer", dict(room_id=R, peer_id=pid, virtual_ip=vip, client_nonce=nonce, now_ms=now))


def confirm(other=P):
    return ("confirm_pair", dict(room_id=R, host_peer_id=H, other_peer_id=other, ready_at_wall_ms=NOW,
                                 ready_at_mono_ns=5_000, ready_boot_id="boot-a", expires_at_ms=EXP))


RENEW_NOW = NOW + 60_000                # 갱신하는 요청의 now. 방이 살아 있다
RENEW_EXP = RENEW_NOW + 120_000         # 4.6 처리 순서 4번의 now + ROOM_LEASE_S * 1000
RENEW_TTL = EXP_TTL + 60


def renew(sks, now=RENEW_NOW, new=RENEW_EXP):
    return ("renew", dict(room_id=R, now_ms=now, new_expires_at_ms=new, sks=sks))


K_ROOM, K_PEER_H, K_PEER_P, K_VIP_H, K_VIP_P = (R, "ROOM"), (R, f"PEER#{H}"), (R, f"PEER#{P}"), (R, f"VIP#{VIP_H}"), (R, f"VIP#{VIP_P}")
K_PAIR = (R, f"PAIR#{H}-{P}")
K_NON_H, K_NON_P = (f"NONCE#{NON_H}", "NONCE"), (f"NONCE#{NON_P}", "NONCE")
INTERNAL = "OpError:internal"

# ---------------------------------------------------------------------------
# 출처: control_plane.md 6.3 항목, 쓰기 표 (연산 | 쓰기 | 조건). note 는 조건 열이다.
# ---------------------------------------------------------------------------
WRITES = [
    {"id": "create-room", "op": "`create_room`",
     "write": "트랜잭션: `ROOM` put, `PEER#host` put, `VIP#10.100.0.1` put, `NONCE#` put",
     "note": "네 put 전부 `attribute_not_exists(pk)` 다. `ROOM` 실패는 `room_id` 충돌 → 재추첨(2.1). `NONCE#` "
             "실패는 동시에 같은 nonce 가 들어온 것이므로 다시 읽어 그 결과를 돌려준다. `PEER#`·`VIP#` 는 `ROOM` 이 "
             "없으면 있을 수 없으므로 실패가 나면 저장소가 앞선 방의 항목을 일부만 지운 상태다. 그때는 `internal` "
             "이고 재추첨하지 않는다"},
    {"id": "join-room", "op": "`join_room` 새 참가",
     "write": "풀의 주소마다 트랜잭션: `ROOM` ConditionCheck, `VIP#<ip>` put, `PEER#<peer_id>` put, `NONCE#` put",
     "note": "`ROOM` 에 `attribute_exists(pk) AND expires_at_ms > :now`. 사전 읽기와 쓰기 사이에 방이 만료되거나 "
             "삭제되는 경합을 막는다. `VIP#` 와 `PEER#` 와 `NONCE#` 각각 `attribute_not_exists(pk)`. 실패 처리는 "
             "아래 취소 사유 표"},
    {"id": "register-candidate", "op": "`register_candidate`", "write": "`PEER#` update: `candidates = :list`",
     "note": "`attribute_exists(pk) AND peer_token = :t`. 실패는 `unauthorized` 다. 토큰 검사 뒤 그 피어가 회수된 "
             "경우다(4.6 서버 회수)"},
    {"id": "host-report-release", "op": "`host_report` 회수",
     "write": "대상마다 트랜잭션: `PEER#<target>` delete, `VIP#<그 피어의 virtual_ip>` delete, 그 피어가 낀 `PAIR#` delete",
     "note": "`PEER#` 에만 `attribute_exists(pk)` 를 건다. 그 조건이 실패하면 이미 없던 대상이고 `released` 에 담지 "
             "않는다(4.6). 나머지 둘은 조건 없는 delete 다. 확인 전에 나간 피어는 `PAIR#` 가 아예 없고, 조건을 걸면 "
             "그 회수가 통째로 취소된다"},
    {"id": "host-report-reclaim", "op": "`host_report` 서버 회수",
     "write": "대상마다 트랜잭션: `PEER#<target>` delete, `VIP#<그 피어의 virtual_ip>` delete, "
              "`NONCE#<그 피어의 client_nonce>` delete",
     "note": "`PEER#` 에 `attribute_exists(pk) AND (attribute_not_exists(candidates) OR size(candidates) = :zero) AND "
             "joined_at_ms <= :cutoff`. `:cutoff` 는 `now - JOIN_REGISTER_GRACE_S * 1000` 이다. 조건이 실패하면 그 "
             "사이 등록했거나 이미 없는 것이므로 `released` 에 담지 않는다. 후보가 없던 피어는 `PAIR#` 가 있을 수 "
             "없으므로 지우지 않는다"},
    {"id": "host-report-confirm", "op": "`host_report` 확인",
     "write": "쌍마다 트랜잭션: `PEER#<호스트>` ConditionCheck, `PEER#<상대>` ConditionCheck, `PAIR#<lo>-<hi>` put",
     "note": "두 ConditionCheck 는 각각 `attribute_exists(pk) AND size(candidates) > :zero` 다. put 은 "
             "`attribute_not_exists(pk)`. 양쪽 후보 조건을 저장소 조건으로 건다. 읽고 나서 쓰기 전에 상대가 "
             "회수되거나 후보가 비워지는 경합이 있다. put 조건만 실패하면 이미 확인된 쌍이므로 오류가 아니다(4.6)"},
    {"id": "host-report-renew", "op": "`host_report` 갱신",
     "write": "`ROOM` update(`expires_at_ms = :new`, `ttl = :new_ttl`) 하나를 먼저 쓴다. 그것이 성공하면 그 방의 "
              "나머지 항목마다 `ttl = :new_ttl` update 를 따로 쓴다. 트랜잭션으로 묶지 않는다",
     "note": "`ROOM` 에 `attribute_exists(pk) AND expires_at_ms > :now`. 실패는 `room_expired` 이고 나머지를 쓰지 "
             "않는다. 나머지 항목은 각각 `attribute_exists(pk)` 다. 조건이 실패하면 그 사이 지워진 항목이므로 "
             "무시한다. 항목 목록은 이 요청의 첫 `Query` 결과에서 2번이 지운 항목을 뺀 것이다"},
]

# 쓰기 표 행마다 조건의 두 결과(pass: 조건 성립, fail: 조건 실패)를 돈다.
# present / absent 는 호출 뒤 있어야 하는 키와 없어야 하는 키, attrs 는 호출 뒤 그 항목의 속성 값이다.
WRITE_CASES = [
    # create_room
    {"id": "create-ok", "row": "create-room", "side": "pass", "seed": [], "call": create(), "expect": S.CREATED,
     "present": [K_ROOM, K_PEER_H, K_VIP_H, K_NON_H], "absent": [],
     "attrs": {K_ROOM: {"created_at_ms": NOW, "expires_at_ms": EXP, "host_peer_id": H},
               K_PEER_H: {"peer_id": H, "peer_token": TOK_H, "virtual_ip": VIP_H, "client_nonce": NON_H,
                          "joined_at_ms": NOW},
               K_VIP_H: {"peer_id": H}, K_NON_H: {"room_id": R, "peer_id": H}},
     "note": "네 put 이 함께 들어간다"},
    {"id": "create-room-taken", "row": "create-room", "side": "fail", "seed": [room_item(host_peer_id=99)],
     "call": create(), "expect": S.ROOM_ID_TAKEN, "present": [], "absent": [K_PEER_H, K_VIP_H, K_NON_H],
     "attrs": {K_ROOM: {"host_peer_id": 99}}, "note": "ROOM 충돌. 덮어쓰지 않고 재추첨으로 간다"},
    {"id": "create-nonce-taken", "row": "create-room", "side": "fail", "seed": [nonce_item(NON_H, 99, room="ZZZZZZ")],
     "call": create(), "expect": S.NONCE_EXISTS, "present": [], "absent": [K_ROOM, K_PEER_H, K_VIP_H],
     "attrs": {K_NON_H: {"room_id": "ZZZZZZ", "peer_id": 99}}, "note": "같은 nonce 가 먼저 성립했다"},
    {"id": "create-peer-alone", "row": "create-room", "side": "fail", "seed": [host_item()],
     "call": create(), "expect": INTERNAL, "present": [], "absent": [K_ROOM, K_VIP_H, K_NON_H], "attrs": {},
     "note": "ROOM 없이 PEER# 만 있다. 저장소가 앞선 방을 일부만 지운 상태다. internal, 재추첨하지 않는다"},
    {"id": "create-vip-alone", "row": "create-room", "side": "fail", "seed": [vip_item(VIP_H, 99)],
     "call": create(), "expect": INTERNAL, "present": [], "absent": [K_ROOM, K_PEER_H, K_NON_H],
     "attrs": {K_VIP_H: {"peer_id": 99}}, "note": "ROOM 없이 VIP# 만 있다. internal"},
    # join_room
    {"id": "join-ok", "row": "join-room", "side": "pass", "seed": [room_item()], "call": join(),
     "expect": S.JOINED, "present": [K_VIP_P, K_PEER_P, K_NON_P], "absent": [],
     "attrs": {K_VIP_P: {"peer_id": P},
               K_PEER_P: {"peer_id": P, "peer_token": TOK_P, "virtual_ip": VIP_P, "client_nonce": NON_P,
                          "joined_at_ms": NOW},
               K_NON_P: {"room_id": R, "peer_id": P}},
     "note": "열린 방, 빈 주소"},
    {"id": "join-room-1ms-left", "row": "join-room", "side": "pass", "seed": [room_item(exp=NOW + 1)],
     "call": join(), "expect": S.JOINED, "present": [K_VIP_P], "absent": [], "attrs": {},
     "note": "expires_at_ms > :now. 1ms 남은 방은 살아 있다"},
    {"id": "join-room-at-expiry", "row": "join-room", "side": "fail", "seed": [room_item(exp=NOW)],
     "call": join(), "expect": S.ROOM_GONE, "present": [], "absent": [K_VIP_P, K_PEER_P, K_NON_P], "attrs": {},
     "note": "now == expires_at_ms 는 만료 쪽이다 (5.1)"},
    {"id": "join-room-absent", "row": "join-room", "side": "fail", "seed": [], "call": join(),
     "expect": S.ROOM_GONE, "present": [], "absent": [K_ROOM, K_VIP_P, K_PEER_P, K_NON_P], "attrs": {},
     "note": "방이 없다. 새 항목을 만들지 않는다"},
    {"id": "join-vip-taken", "row": "join-room", "side": "fail", "seed": [room_item(), vip_item(VIP_P, Q)],
     "call": join(), "expect": S.VIP_TAKEN, "present": [], "absent": [K_PEER_P, K_NON_P],
     "attrs": {K_VIP_P: {"peer_id": Q}}, "note": "주소가 선점됐다. 덮어쓰지 않는다"},
    {"id": "join-peer-taken", "row": "join-room", "side": "fail",
     "seed": [room_item(), peer_item(P, VIP_Q, TOK_Q, NON_Q)], "call": join(), "expect": S.PEER_ID_TAKEN,
     "present": [], "absent": [K_VIP_P, K_NON_P], "attrs": {K_PEER_P: {"peer_token": TOK_Q}},
     "note": "peer_id 충돌. 덮어쓰지 않는다"},
    {"id": "join-nonce-taken", "row": "join-room", "side": "fail", "seed": [room_item(), nonce_item(NON_P, Q)],
     "call": join(), "expect": S.NONCE_EXISTS, "present": [], "absent": [K_VIP_P, K_PEER_P],
     "attrs": {K_NON_P: {"peer_id": Q}}, "note": "같은 nonce 가 먼저 성립했다"},
    # register_candidate
    {"id": "register-ok", "row": "register-candidate", "side": "pass",
     "seed": [room_item(), p_item(candidates=CAND)], "call": register(), "expect": True,
     "present": [K_PEER_P], "absent": [], "attrs": {K_PEER_P: {"candidates": CAND2}},
     "note": "통째로 교체한다. 추가가 아니다 (4.4)"},
    {"id": "register-wrong-token", "row": "register-candidate", "side": "fail",
     "seed": [room_item(), p_item(candidates=CAND)], "call": register(token=TOK_Q), "expect": False,
     "present": [], "absent": [], "attrs": {K_PEER_P: {"candidates": CAND}},
     "note": "읽은 뒤 토큰이 바뀐 것과 같다. 쓰지 않는다"},
    {"id": "register-peer-gone", "row": "register-candidate", "side": "fail", "seed": [room_item()],
     "call": register(), "expect": False, "present": [], "absent": [K_PEER_P], "attrs": {},
     "note": "토큰 검사 뒤 회수된 피어. 항목을 되살리지 않는다"},
    # host_report 회수
    {"id": "release-ok", "row": "host-report-release", "side": "pass",
     "seed": [room_item(), host_item(candidates=CAND), p_item(candidates=CAND), vip_item(VIP_P, P),
              pair_item(H, P)],
     "call": release(), "expect": True, "present": [K_PEER_H, K_ROOM], "absent": [K_PEER_P, K_VIP_P, K_PAIR],
     "attrs": {}, "note": "PEER#, VIP#, PAIR# 를 함께 지운다"},
    {"id": "release-ok-no-pair", "row": "host-report-release", "side": "pass",
     "seed": [room_item(), p_item(), vip_item(VIP_P, P)], "call": release(), "expect": True,
     "present": [], "absent": [K_PEER_P, K_VIP_P], "attrs": {},
     "note": "확인 전에 나간 피어. PAIR# 가 없어도 회수가 취소되지 않는다"},
    {"id": "release-gone", "row": "host-report-release", "side": "fail",
     "seed": [room_item(), vip_item(VIP_P, Q), pair_item(H, P)], "call": release(), "expect": False,
     "present": [K_VIP_P, K_PAIR], "absent": [], "attrs": {K_VIP_P: {"peer_id": Q}},
     "note": "이미 없던 대상. 그 주소를 이미 받은 다른 피어의 VIP# 를 지우지 않는다"},
    # host_report 서버 회수
    {"id": "reclaim-ok-at-cutoff", "row": "host-report-reclaim", "side": "pass",
     "seed": [room_item(), p_item(joined=NOW - G), vip_item(VIP_P, P), nonce_item(NON_P, P)],
     "call": reclaim(), "expect": True, "present": [], "absent": [K_PEER_P, K_VIP_P, K_NON_P], "attrs": {},
     "note": "joined_at_ms <= :cutoff 의 경계. 후보 속성 없음"},
    {"id": "reclaim-ok-empty-list", "row": "host-report-reclaim", "side": "pass",
     "seed": [room_item(), p_item(candidates=[], joined=NOW - G - 1), vip_item(VIP_P, P), nonce_item(NON_P, P)],
     "call": reclaim(), "expect": True, "present": [], "absent": [K_PEER_P, K_VIP_P, K_NON_P], "attrs": {},
     "note": "size(candidates) = :zero. 빈 리스트도 후보 없음이다"},
    {"id": "reclaim-ok-without-vip", "row": "host-report-reclaim", "side": "pass",
     "seed": [room_item(), p_item(joined=NOW - G), nonce_item(NON_P, P)],
     "call": reclaim(), "expect": True, "present": [], "absent": [K_PEER_P, K_NON_P], "attrs": {},
     "note": "VIP# delete 는 조건이 없다. VIP# 가 이미 없어도 회수가 취소되지 않는다"},
    {"id": "reclaim-ok-without-nonce", "row": "host-report-reclaim", "side": "pass",
     "seed": [room_item(), p_item(joined=NOW - G), vip_item(VIP_P, P)],
     "call": reclaim(), "expect": True, "present": [], "absent": [K_PEER_P, K_VIP_P], "attrs": {},
     "note": "NONCE# delete 도 조건이 없다. TTL 이 먼저 지운 NONCE# 때문에 회수가 취소되지 않는다 (6.5)"},
    {"id": "reclaim-has-candidates", "row": "host-report-reclaim", "side": "fail",
     "seed": [room_item(), p_item(candidates=CAND, joined=NOW - G - 1), vip_item(VIP_P, P), nonce_item(NON_P, P)],
     "call": reclaim(), "expect": False, "present": [K_PEER_P, K_VIP_P, K_NON_P], "absent": [], "attrs": {},
     "note": "등록한 피어는 지우지 않는다"},
    {"id": "reclaim-before-cutoff", "row": "host-report-reclaim", "side": "fail",
     "seed": [room_item(), p_item(joined=NOW - G + 1), vip_item(VIP_P, P), nonce_item(NON_P, P)],
     "call": reclaim(), "expect": False, "present": [K_PEER_P, K_VIP_P, K_NON_P], "absent": [], "attrs": {},
     "note": "유예가 1ms 남았다"},
    {"id": "reclaim-gone", "row": "host-report-reclaim", "side": "fail",
     "seed": [room_item(), vip_item(VIP_P, Q), nonce_item(NON_P, Q)], "call": reclaim(), "expect": False,
     "present": [K_VIP_P, K_NON_P], "absent": [], "attrs": {},
     "note": "이미 없는 피어. 다른 피어의 VIP# 와 NONCE# 를 지우지 않는다"},
    # host_report 확인
    {"id": "confirm-ok", "row": "host-report-confirm", "side": "pass",
     "seed": [room_item(), host_item(candidates=CAND), p_item(candidates=CAND)], "call": confirm(),
     "expect": S.CONFIRMED, "present": [K_PAIR], "absent": [],
     "attrs": {K_PAIR: {"punch_delay_ms": PUNCH_DELAY_MS, "ready_at_wall_ms": NOW, "ready_at_mono_ns": 5_000,
                        "ready_boot_id": "boot-a"}},
     "note": "양쪽 후보가 있다. punch_delay_ms = PUNCH_DELAY_MS"},
    {"id": "confirm-already", "row": "host-report-confirm", "side": "fail",
     "seed": [room_item(), host_item(candidates=CAND), p_item(candidates=CAND), pair_item(H, P)],
     "call": confirm(), "expect": S.ALREADY_CONFIRMED, "present": [K_PAIR], "absent": [],
     "attrs": {K_PAIR: {"punch_delay_ms": 777, "ready_at_wall_ms": NOW - 500}},
     "note": "put 조건만 실패. 오류가 아니고 기준점을 덮어쓰지 않는다 (5.2)"},
    {"id": "confirm-already-but-other-empty", "row": "host-report-confirm", "side": "fail",
     "seed": [room_item(), host_item(candidates=CAND), p_item(candidates=[]), pair_item(H, P)],
     "call": confirm(), "expect": S.NOT_ELIGIBLE, "present": [K_PAIR], "absent": [], "attrs": {},
     "note": "put 조건'만' 실패해야 이미 확인된 쌍이다. 상대 조건도 실패하면 확인 대상이 아니다"},
    {"id": "confirm-other-empty", "row": "host-report-confirm", "side": "fail",
     "seed": [room_item(), host_item(candidates=CAND), p_item(candidates=[])], "call": confirm(),
     "expect": S.NOT_ELIGIBLE, "present": [], "absent": [K_PAIR], "attrs": {},
     "note": "상대 후보가 빈 리스트. 쓰면 get_peers 가 빈 후보와 함께 준비 완료를 준다"},
    {"id": "confirm-other-no-attr", "row": "host-report-confirm", "side": "fail",
     "seed": [room_item(), host_item(candidates=CAND), p_item()], "call": confirm(),
     "expect": S.NOT_ELIGIBLE, "present": [], "absent": [K_PAIR], "attrs": {},
     "note": "상대가 후보 속성 없는 joined 피어"},
    {"id": "confirm-other-absent", "row": "host-report-confirm", "side": "fail",
     "seed": [room_item(), host_item(candidates=CAND)], "call": confirm(),
     "expect": S.NOT_ELIGIBLE, "present": [], "absent": [K_PAIR], "attrs": {},
     "note": "상대가 회수됐다"},
    {"id": "confirm-host-empty", "row": "host-report-confirm", "side": "fail",
     "seed": [room_item(), host_item(), p_item(candidates=CAND)], "call": confirm(),
     "expect": S.NOT_ELIGIBLE, "present": [], "absent": [K_PAIR], "attrs": {},
     "note": "호출자 쪽도 후보가 있어야 한다. 호스트의 후보 속성이 없다"},
    {"id": "confirm-host-empty-list", "row": "host-report-confirm", "side": "fail",
     "seed": [room_item(), host_item(candidates=[]), p_item(candidates=CAND)], "call": confirm(),
     "expect": S.NOT_ELIGIBLE, "present": [], "absent": [K_PAIR], "attrs": {},
     "note": "호스트의 후보가 빈 리스트. size(candidates) > :zero"},
    {"id": "confirm-already-but-host-empty", "row": "host-report-confirm", "side": "fail",
     "seed": [room_item(), host_item(candidates=[]), p_item(candidates=CAND), pair_item(H, P)],
     "call": confirm(), "expect": S.NOT_ELIGIBLE, "present": [K_PAIR], "absent": [], "attrs": {},
     "note": "put 조건'만' 실패해야 이미 확인된 쌍이다. 호스트 조건도 실패하면 확인 대상이 아니다"},
    # host_report 갱신
    {"id": "renew-ok", "row": "host-report-renew", "side": "pass",
     "seed": [room_item(), host_item(), p_item(), vip_item(VIP_P, P)],
     "call": renew(["ROOM", f"PEER#{H}", f"PEER#{P}", f"VIP#{VIP_P}"]), "expect": True,
     "present": [K_ROOM, K_PEER_H, K_PEER_P, K_VIP_P], "absent": [],
     "attrs": {K_ROOM: {"expires_at_ms": RENEW_EXP, "ttl": RENEW_TTL},
               K_PEER_H: {"ttl": RENEW_TTL}, K_PEER_P: {"ttl": RENEW_TTL}, K_VIP_P: {"ttl": RENEW_TTL}},
     "note": "ROOM 을 밀고 나머지 항목의 ttl 을 같이 민다 (6.5)"},
    {"id": "renew-at-expiry", "row": "host-report-renew", "side": "fail",
     "seed": [room_item(exp=RENEW_NOW), p_item()], "call": renew(["ROOM", f"PEER#{P}"]), "expect": False,
     "present": [], "absent": [],
     "attrs": {K_ROOM: {"expires_at_ms": RENEW_NOW, "ttl": EXP_TTL}, K_PEER_P: {"ttl": EXP_TTL}},
     "note": "만료된 방은 되살리지 않는다. 나머지도 쓰지 않는다"},
    {"id": "renew-room-absent", "row": "host-report-renew", "side": "fail", "seed": [p_item()],
     "call": renew(["ROOM", f"PEER#{P}"]), "expect": False, "present": [], "absent": [K_ROOM],
     "attrs": {K_PEER_P: {"ttl": EXP_TTL}}, "note": "방이 없다. ROOM 을 만들지 않는다"},
    {"id": "renew-skips-deleted", "row": "host-report-renew", "side": "fail", "seed": [room_item()],
     "call": renew(["ROOM", f"PEER#{P}", f"VIP#{VIP_P}"]), "expect": True, "present": [K_ROOM],
     "absent": [K_PEER_P, K_VIP_P], "attrs": {},
     "note": "목록에 있지만 지워진 항목. ttl 만 가진 항목으로 되살리지 않는다 (6.3 왜 이렇게 정했나)"},
    {"id": "renew-pair-live-and-deleted", "row": "host-report-renew", "side": "fail",
     "seed": [room_item(), host_item(candidates=CAND), p_item(candidates=CAND), pair_item(H, P)],
     "call": renew(["ROOM", f"PAIR#{H}-{P}", f"PAIR#{H}-{Q}"]), "expect": True, "present": [K_PAIR],
     "absent": [(R, f"PAIR#{H}-{Q}")], "attrs": {K_PAIR: {"ttl": RENEW_TTL, "punch_delay_ms": 777}},
     "note": "PAIR# 도 방 파티션 항목이다. 살아 있는 쌍은 밀고 지워진 쌍은 되살리지 않는다"},
    {"id": "renew-continues-after-deleted", "row": "host-report-renew", "side": "fail",
     "seed": [room_item(), host_item()], "call": renew(["ROOM", f"PEER#{P}", f"PEER#{H}"]), "expect": True,
     "present": [K_PEER_H], "absent": [K_PEER_P], "attrs": {K_PEER_H: {"ttl": RENEW_TTL}},
     "note": "지워진 항목은 무시하고 다음 항목으로 간다. 첫 실패에서 멈추면 뒤의 항목이 한 번 덜 밀린다"},
]


@pytest.mark.store
@pytest.mark.parametrize("case", WRITE_CASES, ids=[c["id"] for c in WRITE_CASES])
def test_write(store, case):
    seed(store, case["seed"])
    assert outcome(store, case["call"]) == case["expect"]
    for key in case["present"]:
        assert raw(store, *key) is not None, key
    for key in case["absent"]:
        assert raw(store, *key) is None, key
    for key, attrs in case["attrs"].items():
        got = raw(store, *key)
        assert got is not None, key
        for name, value in attrs.items():
            assert got.get(name) == value, (key, name)


# ---------------------------------------------------------------------------
# 출처: control_plane.md 6.3 항목, 취소 사유 표 (순서 | 실패한 조건 | 동작). note 는 동작 열이다.
# store 는 사유를 분류해 돌려주고 동작(다시 읽기, 다음 주소, 재추첨)은 부르는 쪽이 한다.
# ---------------------------------------------------------------------------
CANCEL_ORDER = [
    {"id": "order-1-nonce", "order": "1", "failed": "`NONCE#`",
     "note": "다른 사유가 무엇이든 같은 nonce 의 요청이 먼저 성립한 것이다. `NONCE#` 를 다시 읽어 그 "
             "`room_id`·`peer_id` 로 방을 `Query` 하고 같은 응답을 돌려준다. 그 방이 만료됐으면 `room_expired` "
             "다(4.2). `room_full` 이나 재추첨으로 가지 않는다. 다시 읽은 `room_id` 가 요청의 방과 다르면 "
             "`bad_request` 다. nonce 를 다른 방에 재사용한 것이고, 사전 읽기 경로가 같은 입력에 내는 답과 같아야 한다"},
    {"id": "order-2-room-check", "order": "2", "failed": "`ROOM` ConditionCheck (`join_room`)",
     "note": "방을 다시 읽는다. 없으면 `room_not_found`, 있으면 `room_expired`. 다음 주소로 넘어가지 않는다"},
    {"id": "order-3-vip", "order": "3", "failed": "`VIP#`", "note": "다음 주소. 풀이 끝나면 `room_full`"},
    {"id": "order-4-peer", "order": "4", "failed": "`PEER#`",
     "note": "`peer_id` 재추첨. 상한은 `MAX_PEER_ID_ATTEMPTS`"},
    {"id": "order-5-room-put", "order": "5", "failed": "`ROOM` put (`create_room`)",
     "note": "`room_id` 재추첨. 상한은 `MAX_ROOM_ID_ATTEMPTS`"},
    {"id": "order-other", "order": "-", "failed": "조건 실패가 아닌 사유 (`TransactionConflict`, 스로틀)",
     "note": "`internal`. 클라이언트가 8.3 오류 분류와 재시도의 규칙대로 재시도한다"},
]

# 여러 조건이 같이 실패하는 행. 앞의 WRITE_CASES 는 한 조건만 실패한다.
CANCEL_CASES = [
    {"id": "nonce-with-vip", "row": "order-1-nonce",
     "seed": [room_item(), p_item(), vip_item(VIP_P, P), nonce_item(NON_P, P)], "call": join(pid=44),
     "expect": S.NONCE_EXISTS,
     "note": "응답을 잃은 참가자가 같은 nonce 로 다시 왔다. VIP# 를 먼저 보면 다음 주소로 넘어가 room_full 이 된다"},
    {"id": "nonce-with-room-expired", "row": "order-1-nonce",
     "seed": [room_item(exp=NOW), nonce_item(NON_P, P)], "call": join(), "expect": S.NONCE_EXISTS,
     "note": "방이 만료됐어도 1번이다. room_expired 는 다시 읽은 방으로 부르는 쪽이 낸다"},
    {"id": "nonce-with-room-put", "row": "order-1-nonce",
     "seed": [room_item(), nonce_item(NON_H, H)], "call": create(), "expect": S.NONCE_EXISTS,
     "note": "create_room 에서도 1번이다. room_id 재추첨으로 가지 않는다"},
    {"id": "room-check-with-vip", "row": "order-2-room-check",
     "seed": [room_item(exp=NOW), vip_item(VIP_P, Q)], "call": join(), "expect": S.ROOM_GONE,
     "note": "만료된 방의 선점된 주소. 다음 주소로 넘어가지 않는다"},
    {"id": "room-check-alone", "row": "order-2-room-check", "seed": [], "call": join(), "expect": S.ROOM_GONE,
     "note": "방이 없다"},
    {"id": "vip-with-peer", "row": "order-3-vip",
     "seed": [room_item(), vip_item(VIP_P, Q), peer_item(P, VIP_Q, TOK_Q, NON_Q)], "call": join(),
     "expect": S.VIP_TAKEN, "note": "3번이 4번보다 먼저다. 다음 주소로 간다"},
    {"id": "vip-alone", "row": "order-3-vip", "seed": [room_item(), vip_item(VIP_P, Q)], "call": join(),
     "expect": S.VIP_TAKEN, "note": "다음 주소"},
    {"id": "peer-alone", "row": "order-4-peer", "seed": [room_item(), peer_item(P, VIP_Q, TOK_Q, NON_Q)],
     "call": join(), "expect": S.PEER_ID_TAKEN, "note": "peer_id 재추첨"},
    {"id": "room-put-alone", "row": "order-5-room-put", "seed": [room_item()], "call": create(),
     "expect": S.ROOM_ID_TAKEN, "note": "room_id 재추첨"},
    {"id": "room-put-with-vip-and-peer", "row": "order-5-room-put",
     "seed": [room_item(), host_item(), vip_item(VIP_H, H)], "call": create(), "expect": S.ROOM_ID_TAKEN,
     "note": "앞선 방이 살아 있으면 VIP#10.100.0.1 과 그 호스트 PEER# 도 같이 실패한다. 표의 순서로는 3번(다음 "
             "주소)이 먼저지만 create_room 에는 다음 주소가 없다. 쓰기 표의 create_room 행대로 재추첨이다. 보고서의 "
             "DOC GAP 1"},
]


@pytest.mark.store
@pytest.mark.parametrize("case", CANCEL_CASES, ids=[c["id"] for c in CANCEL_CASES])
def test_cancel(store, case):
    seed(store, case["seed"])
    assert outcome(store, case["call"]) == case["expect"]


# 순서 "-" 행. DynamoDB local 은 TransactionConflict 를 내지 않으므로(7.6 로컬 시험의 한계) 사유 목록을
# 직접 만들어 판정 함수에 넣는다. join_room 트랜잭션의 위치: ROOM 확인, VIP#, PEER#, NONCE#.
JOIN_LAYOUT = ("room_check", "vip", "peer", "nonce")
JOIN_PRIORITY = S._JOIN_PRIORITY
NONE, CCF, CONFLICT = {"Code": "None"}, {"Code": "ConditionalCheckFailed"}, {"Code": "TransactionConflict"}

CLASSIFY_CASES = [
    {"id": "conflict-only", "row": "order-other", "reasons": [NONE, CONFLICT, NONE, NONE], "expect": INTERNAL,
     "note": "조건 실패가 없다"},
    {"id": "throttled", "row": "order-other",
     "reasons": [NONE, NONE, {"Code": "ProvisionedThroughputExceeded"}, NONE], "expect": INTERNAL,
     "note": "스로틀"},
    {"id": "vip-with-conflict", "row": "order-other", "reasons": [NONE, CCF, CONFLICT, NONE], "expect": INTERNAL,
     "note": "다른 항목의 사유가 일시 오류면 VIP# 실패를 믿고 다음 주소로 가지 않는다. 보고서의 DOC GAP 2"},
    {"id": "nonce-with-conflict", "row": "order-1-nonce", "reasons": [CONFLICT, NONE, NONE, CCF],
     "expect": "nonce", "note": "1번은 다른 사유가 무엇이든 먼저다"},
    {"id": "unknown-code", "row": "order-other", "reasons": [NONE, {"Code": "SomethingNew"}, NONE, NONE],
     "expect": INTERNAL, "note": "모르는 코드는 판정 불가다"},
    {"id": "vip-with-unknown", "row": "order-other", "reasons": [NONE, CCF, {"Code": "SomethingNew"}, NONE],
     "expect": INTERNAL, "note": "모르는 코드가 섞이면 VIP# 실패를 믿지 않는다. 허용 목록 밖은 판정 불가다"},
    {"id": "reason-not-dict", "row": "order-other", "reasons": [NONE, "x", NONE, NONE], "expect": INTERNAL,
     "note": "모양을 모르면 판정 불가다"},
    {"id": "length-mismatch", "row": "order-other", "reasons": [CCF, NONE, NONE], "expect": INTERNAL,
     "note": "위치로 항목을 식별할 수 없다"},
    {"id": "reasons-missing", "row": "order-other", "reasons": None, "expect": INTERNAL,
     "note": "취소 사유가 없다"},
    {"id": "all-none", "row": "order-other", "reasons": [NONE, NONE, NONE, NONE], "expect": INTERNAL,
     "note": "취소됐는데 실패한 항목이 없다"},
]


@pytest.mark.parametrize("case", CLASSIFY_CASES, ids=[c["id"] for c in CLASSIFY_CASES])
def test_classify(case):
    try:
        got = S.classify_cancel(JOIN_LAYOUT, case["reasons"], JOIN_PRIORITY)
    except OpError as exc:
        got = f"OpError:{exc.code}"
    assert got == case["expect"]


# ---------------------------------------------------------------------------
# 출처: control_plane.md 6.3 항목, 읽기 표 (연산 | 읽기). note 는 읽기 열이다.
# calls 는 그 행이 store 에서 부르는 읽기다. 전부 ConsistentRead=True 여야 한다 (저장 1).
# 일관성 자체는 DynamoDB local 에서 드러나지 않으므로(7.6) 요청 인자를 본다. 9장의 "코드를 읽어서만
# 판정할 수 있는 것" 을 대신하지 않는다. 그 인자가 빠지지 않았다는 것까지만 기계로 본다.
# ---------------------------------------------------------------------------
READS = [
    {"id": "read-create-room", "op": "`create_room`",
     "note": "`NONCE#` GetItem (멱등성). 있으면 그 `room_id` 로 `Query` 해 같은 응답을 조립",
     "calls": [("get_nonce", (NON_H,)), ("query_room", (R,))]},
    {"id": "read-join-room", "op": "`join_room` 새 참가",
     "note": "`NONCE#` GetItem → 있으면 저장된 `room_id` 가 요청과 같아야 한다(다르면 `bad_request`. nonce 를 "
             "다른 방에 재사용한 것이다) → 없으면 `ROOM` GetItem (만료·존재) → 트랜잭션",
     "calls": [("get_nonce", (NON_P,)), ("get_room", (R,))]},
    {"id": "read-register-candidate", "op": "`register_candidate`",
     "note": "`Query(pk)` 로 `ROOM` 과 `PEER#` 전부 (만료·토큰 검사) → `PEER#` update",
     "calls": [("query_room", (R,))]},
    {"id": "read-get-peers", "op": "`get_peers`",
     "note": "`Query(pk)` 한 번. `ROOM` 과 `PEER#` 와 `PAIR#` 전부가 나온다. 토큰 검사도 그 결과로 한다",
     "calls": [("query_room", (R,))]},
    {"id": "read-host-report", "op": "`host_report`",
     "note": "`Query(pk)` 로 만료·호출자 판정 → 회수·확인·갱신 쓰기 → `Query(pk)` 를 다시 읽어 응답을 조립 (4.6)",
     "calls": [("query_room", (R, True)), ("query_room", (R,))]},
]


class Spy:
    """boto3 클라이언트를 감싸 호출 이름과 인자를 적는다. 호출은 그대로 넘긴다."""

    def __init__(self, inner):
        self.inner = inner
        self.calls = []

    def __getattr__(self, name):
        fn = getattr(self.inner, name)

        def wrapped(**kwargs):
            self.calls.append((name, kwargs))
            return fn(**kwargs)
        return wrapped


FULL_ROOM = [room_item(), host_item(candidates=CAND), p_item(candidates=CAND), vip_item(VIP_H, H),
             vip_item(VIP_P, P), pair_item(H, P), nonce_item(NON_H, H), nonce_item(NON_P, P)]


@pytest.mark.store
@pytest.mark.parametrize("case", READS, ids=[c["id"] for c in READS])
def test_read_consistent(store, case):
    seed(store, FULL_ROOM)
    spy = Spy(store.client)
    store.client = spy
    for method, args in case["calls"]:
        assert getattr(store, method)(*args) is not None
    reads = [(name, kw) for name, kw in spy.calls if name in ("get_item", "query", "scan", "batch_get_item")]
    assert len(reads) == len(case["calls"])
    for name, kw in reads:
        assert name in ("get_item", "query")
        assert kw.get("ConsistentRead") is True, name
        assert "IndexName" not in kw   # 저장 2. GSI 를 쓰지 않는다


@pytest.mark.store
def test_read_shapes(store):
    """읽기 결과의 모양. 6.3 항목 표의 속성이 int 와 str 로 넘어온다."""
    seed(store, FULL_ROOM)
    assert store.get_nonce(NON_P) == {"room_id": R, "peer_id": P, "ttl": EXP_TTL}
    assert store.get_room(R) == {"created_at_ms": NOW - 1000, "expires_at_ms": EXP, "host_peer_id": H,
                                 "ttl": EXP_TTL}
    view = store.query_room(R)
    assert view.room["expires_at_ms"] == EXP
    assert set(view.peers) == {H, P}
    assert view.peers[P] == {"peer_id": P, "peer_token": TOK_P, "virtual_ip": VIP_P, "client_nonce": NON_P,
                             "joined_at_ms": NOW - 1000, "ttl": EXP_TTL, "candidates": CAND}
    assert view.vips == {VIP_H: H, VIP_P: P}
    assert view.pairs == {f"PAIR#{H}-{P}": {"ready_at_wall_ms": NOW - 500, "ready_at_mono_ns": 123,
                                             "ready_boot_id": "boot-x", "punch_delay_ms": 777, "ttl": EXP_TTL}}
    assert sorted(view.sks) == sorted(["ROOM", f"PEER#{H}", f"PEER#{P}", f"VIP#{VIP_H}", f"VIP#{VIP_P}",
                                       f"PAIR#{H}-{P}"])
    assert view.unreadable_peers == []


@pytest.mark.store
def test_read_absent(store):
    assert store.get_nonce(NON_P) is None
    assert store.get_room(R) is None
    view = store.query_room(R)
    assert view.room is None and view.peers == {} and view.sks == []


# ---------------------------------------------------------------------------
# 출처: control_plane.md 6.3 항목, 마지막 문단 (저장소가 돌려준 값의 형).
#   boto3 는 숫자를 Decimal 로 돌려주므로 정수 속성은 int 로 바꾼다. 항목 표의 속성이 없거나 형이
#   다르면 판정 불가이고 internal 이다. 예외는 둘이다. PEER# 의 candidates 는 없으면 빈 목록이다.
#   4.6 서버 회수의 판정은 요청을 끝내지 않고 그 피어만 건너뛴다.
# 문서에 표는 없다. 문단의 규칙을 행으로 편다. read 는 store 를 부르는 방법, expect 는 그 결과다.
# verdict 가 있는 행은 lenient 로 읽은 PEER# 를 ops.reclaim_verdict 에 넣은 결과까지 본다.
# ---------------------------------------------------------------------------
CONVERT_CASES = [
    {"id": "decimal-to-int", "seed": [room_item()], "read": "room", "expect": "ok",
     "note": "정수 속성은 int 로 넘어온다. Decimal 이 아니다"},
    {"id": "fraction-unreadable", "seed": [room_item(exp=Decimal("1.5"))], "read": "room", "expect": INTERNAL,
     "note": "정수가 아닌 Decimal 은 판정 불가다. int() 로 잘라 읽지 않는다"},
    {"id": "string-for-int", "seed": [room_item(host_peer_id=str(H))], "read": "room", "expect": INTERNAL,
     "note": "형이 다르다"},
    {"id": "bool-for-int", "seed": [room_item(host_peer_id=True)], "read": "room", "expect": INTERNAL,
     "note": "BOOL 은 정수가 아니다"},
    {"id": "room-attr-missing", "seed": [room_item(ttl=OMIT)], "read": "room", "expect": INTERNAL,
     "note": "항목 표의 속성이 없다"},
    {"id": "room-unreadable-in-query", "seed": [room_item(expires_at_ms=OMIT)], "read": "query",
     "expect": INTERNAL, "note": "Query 에서도 같다"},
    {"id": "room-unreadable-lenient", "seed": [room_item(expires_at_ms="x")], "read": "lenient",
     "expect": INTERNAL, "note": "예외는 PEER# 뿐이다. ROOM 은 서버 회수 경로에서도 internal"},
    {"id": "nonce-peer-id-string", "seed": [nonce_item(NON_P, str(P))], "read": "nonce", "expect": INTERNAL,
     "note": "NONCE# 도 같다"},
    {"id": "nonce-room-id-number", "seed": [nonce_item(NON_P, P, room=5)], "read": "nonce", "expect": INTERNAL,
     "note": "문자열 속성에 숫자"},
    {"id": "candidates-absent-empty", "seed": [room_item(), p_item()], "read": "query", "expect": "ok",
     "note": "예외 1. 후보를 아직 등록하지 않은 피어(5.3 joined). 빈 목록이다"},
    {"id": "candidate-port-int", "seed": [room_item(), p_item(candidates=CAND)], "read": "query", "expect": "ok",
     "note": "후보 원소의 port 도 int 로 넘어온다"},
    {"id": "candidate-port-fraction", "seed": [room_item(), p_item(candidates=[dict(CAND[0], port=Decimal("1.5"))])],
     "read": "query", "expect": INTERNAL, "note": "후보 원소의 형도 본다"},
    {"id": "candidate-ip-missing", "seed": [room_item(), p_item(candidates=[{"port": 51000, "kind": "local"}])],
     "read": "query", "expect": INTERNAL, "note": "후보 원소의 ip 가 없다. 'None' 같은 문자열로 만들지 않는다"},
    {"id": "candidate-ip-number", "seed": [room_item(), p_item(candidates=[dict(CAND[0], ip=5)])],
     "read": "query", "expect": INTERNAL, "note": "ip 는 문자열이다"},
    {"id": "candidate-kind-missing", "seed": [room_item(), p_item(candidates=[{"ip": "203.0.113.7", "port": 1}])],
     "read": "query", "expect": INTERNAL, "note": "kind 가 없다"},
    {"id": "candidate-kind-number", "seed": [room_item(), p_item(candidates=[dict(CAND[0], kind=1)])],
     "read": "query", "expect": INTERNAL, "note": "kind 는 문자열이다"},
    {"id": "candidates-not-list", "seed": [room_item(), p_item(candidates="x")], "read": "query",
     "expect": INTERNAL, "note": "서버 회수 밖에서는 internal"},
    {"id": "peer-attr-missing", "seed": [room_item(), p_item(virtual_ip=OMIT)], "read": "query",
     "expect": INTERNAL, "note": "PEER# 의 속성이 없다"},
    {"id": "vip-unreadable", "seed": [room_item(), vip_item(VIP_P, "x")], "read": "lenient", "expect": INTERNAL,
     "note": "VIP# 는 서버 회수 경로에서도 internal"},
    {"id": "pair-unreadable", "seed": [room_item(), pair_item(H, P, punch_delay_ms=OMIT)], "read": "query",
     "expect": INTERNAL, "note": "PAIR# 의 속성이 없다"},
    {"id": "unknown-sk", "seed": [room_item(), _item(R, "WHAT#1", {"ttl": EXP_TTL}, {})], "read": "query",
     "expect": INTERNAL, "note": "모르는 항목 종류는 판정 불가다. 보고서의 '확실하지 않은 것'"},
    # 예외 2. lenient 로 읽으면 그 PEER# 만 unreadable_peers 로 넘어오고 reclaim_verdict 가 SKIP 한다.
    {"id": "lenient-candidates-not-list", "seed": [room_item(), p_item(candidates="x")], "read": "lenient",
     "expect": "unreadable", "verdict": (SKIP, "candidates_unreadable"),
     "note": "요청을 끝내지 않고 그 피어만 건너뛴다"},
    {"id": "lenient-joined-fraction", "seed": [room_item(), p_item(joined=Decimal("1.5"))], "read": "lenient",
     "expect": "unreadable", "verdict": (SKIP, "joined_at_unreadable"),
     "note": "정수가 아닌 Decimal 을 int 로 잘라 넘기지 않는다. 잘리면 1ms 로 읽혀 회수된다"},
    {"id": "lenient-joined-string", "seed": [room_item(), p_item(joined=str(NOW))], "read": "lenient",
     "expect": "unreadable", "verdict": (SKIP, "joined_at_unreadable"), "note": "형이 다르다"},
    {"id": "lenient-peer-id-missing", "seed": [room_item(), p_item(peer_id=OMIT)], "read": "lenient",
     "expect": "unreadable", "verdict": (SKIP, "peer_id_unreadable"), "note": "호스트인지 알 수 없다"},
    {"id": "lenient-token-missing-reclaimable", "seed": [room_item(), p_item(peer_token=OMIT, joined=NOW - G)],
     "read": "lenient", "expect": "unreadable", "verdict": (RECLAIM, None),
     "note": "판정에 쓰지 않는 속성만 빠졌다. 정수 속성은 int 로 넘어와 회수 판정이 된다"},
]


def _read(st, how):
    if how == "room":
        return st.get_room(R)
    if how == "nonce":
        return st.get_nonce(NON_P)
    return st.query_room(R, lenient=(how == "lenient"))


@pytest.mark.store
@pytest.mark.parametrize("case", CONVERT_CASES, ids=[c["id"] for c in CONVERT_CASES])
def test_convert(store, case):
    seed(store, [host_item(candidates=CAND)] + case["seed"])
    if case["expect"] == INTERNAL:
        with pytest.raises(OpError) as exc:
            _read(store, case["read"])
        assert exc.value.code == "internal"
        return
    got = _read(store, case["read"])
    if case["read"] == "room":
        assert all(type(v) is int for v in got.values())
        return
    assert type(got.room["expires_at_ms"]) is int
    assert type(got.peers[H]["candidates"][0]["port"]) is int
    if case["expect"] == "ok":
        assert got.unreadable_peers == []
        peer = got.peers[P]
        assert all(type(peer[k]) is int for k in ("peer_id", "joined_at_ms", "ttl"))
        assert all(type(c["port"]) is int for c in peer["candidates"])
        assert peer["candidates"] == (CAND if case["id"] == "candidate-port-int" else [])
        return
    # unreadable
    assert set(got.peers) == {H}
    assert len(got.unreadable_peers) == 1
    loose = got.unreadable_peers[0]
    assert reclaim_verdict(loose, H, NOW) == case["verdict"]


# ---------------------------------------------------------------------------
# 출처: control_plane.md 6.3 항목 표의 sk 열과 그 아래 문단 "방 파티션에서 위 항목 표에 없는 sk 가
# 나오면 판정 불가이고 internal 이다". 접두어가 아니라 전체 형식을 본다.
#   ROOM / PEER#<peer_id 10진> / VIP#<점 십진> / PAIR#<lo>-<hi> (수의 오름차순, 10진)
# peer_id 는 2.2 대로 0 이 아닌 uint32 다. 10진 표기는 선행 0 이 없는 하나로 본다.
# ---------------------------------------------------------------------------
PEER_ATTRS = dict(peer_id=P, peer_token=TOK_P, virtual_ip=VIP_P, client_nonce=NON_P, joined_at_ms=NOW, ttl=EXP_TTL)
PAIR_ATTRS = dict(ready_at_wall_ms=NOW, ready_at_mono_ns=1, ready_boot_id="b", punch_delay_ms=1000, ttl=EXP_TTL)
VIP_ATTRS = dict(peer_id=P, ttl=EXP_TTL)

SK_CASES = [
    {"id": "peer-uint32-max", "sk": "PEER#4294967295", "attrs": PEER_ATTRS, "read": "query", "expect": "ok",
     "note": "uint32 의 끝"},
    {"id": "peer-over-uint32", "sk": "PEER#4294967296", "attrs": PEER_ATTRS, "read": "query", "expect": INTERNAL,
     "note": "와이어 헤더의 peer_id 는 4바이트다 (2.2)"},
    {"id": "peer-zero", "sk": "PEER#0", "attrs": PEER_ATTRS, "read": "query", "expect": INTERNAL,
     "note": "peer_id 는 0 이 아니다 (2.2)"},
    {"id": "peer-leading-zero", "sk": "PEER#022", "attrs": PEER_ATTRS, "read": "query", "expect": INTERNAL,
     "note": "10진 표기는 하나다. 022 와 22 를 같은 피어로 읽지 않는다"},
    {"id": "peer-suffix", "sk": "PEER#22x", "attrs": PEER_ATTRS, "read": "query", "expect": INTERNAL,
     "note": "전체 일치. 앞부분만 맞는 것을 받지 않는다"},
    {"id": "peer-sign", "sk": "PEER#+22", "attrs": PEER_ATTRS, "read": "query", "expect": INTERNAL,
     "note": "int() 가 받는 표기를 받지 않는다"},
    {"id": "peer-unicode-digit", "sk": "PEER#2\u0662", "attrs": PEER_ATTRS, "read": "query", "expect": INTERNAL,
     "note": "ASCII 밖의 숫자. \\d 와 int() 는 받는다"},
    {"id": "peer-no-id", "sk": "PEER#", "attrs": PEER_ATTRS, "read": "query", "expect": INTERNAL, "note": "빈 id"},
    {"id": "peer-bad-lenient", "sk": "PEER#022", "attrs": PEER_ATTRS, "read": "lenient", "expect": INTERNAL,
     "note": "sk 형식 위반은 서버 회수 경로에서도 internal 이다. 예외 2 는 속성 판독에만 걸린다"},
    {"id": "vip-ok-edge", "sk": "VIP#255.0.0.0", "attrs": VIP_ATTRS, "read": "query", "expect": "ok",
     "note": "옥텟 255 와 0"},
    {"id": "vip-three-octets", "sk": "VIP#10.100.0", "attrs": VIP_ATTRS, "read": "query", "expect": INTERNAL,
     "note": "점 십진 네 옥텟"},
    {"id": "vip-octet-256", "sk": "VIP#10.100.0.256", "attrs": VIP_ATTRS, "read": "query", "expect": INTERNAL,
     "note": "옥텟은 0~255"},
    {"id": "vip-leading-zero", "sk": "VIP#10.100.0.02", "attrs": VIP_ATTRS, "read": "query", "expect": INTERNAL,
     "note": "선행 0 은 받지 않는다 (7.1 의 후보 파싱과 같은 규칙)"},
    {"id": "vip-suffix", "sk": "VIP#10.100.0.2x", "attrs": VIP_ATTRS, "read": "query", "expect": INTERNAL,
     "note": "전체 일치"},
    {"id": "pair-reversed", "sk": f"PAIR#{P}-{H}", "attrs": PAIR_ATTRS, "read": "query", "expect": INTERNAL,
     "note": "lo 가 hi 보다 작아야 한다. 같은 쌍의 두 번째 표기를 받지 않는다"},
    {"id": "pair-equal", "sk": f"PAIR#{H}-{H}", "attrs": PAIR_ATTRS, "read": "query", "expect": INTERNAL,
     "note": "자기 자신과의 쌍은 없다"},
    {"id": "pair-zero", "sk": f"PAIR#0-{H}", "attrs": PAIR_ATTRS, "read": "query", "expect": INTERNAL,
     "note": "peer_id 는 0 이 아니다"},
    {"id": "pair-over-uint32", "sk": f"PAIR#{H}-4294967296", "attrs": PAIR_ATTRS, "read": "query",
     "expect": INTERNAL, "note": "uint32 밖"},
    {"id": "pair-suffix", "sk": f"PAIR#{H}-{P}x", "attrs": PAIR_ATTRS, "read": "query", "expect": INTERNAL,
     "note": "전체 일치"},
    {"id": "pair-numeric-order", "sk": "PAIR#9-10", "attrs": PAIR_ATTRS, "read": "query", "expect": "ok",
     "note": "수의 오름차순이다. 문자열 순서로 보면 9 가 10 보다 뒤라 거부된다"},
    {"id": "room-lower", "sk": "room", "attrs": PAIR_ATTRS, "read": "query", "expect": INTERNAL,
     "note": "ROOM 은 정확히 그 값. 속성이 다른 항목 종류와 맞아도 모르는 sk 는 internal 이다"},
    {"id": "room-suffix", "sk": "ROOM#1", "attrs": PAIR_ATTRS, "read": "query", "expect": INTERNAL,
     "note": "모르는 sk. 속성 판독에 기대지 않는다"},
]


@pytest.mark.store
@pytest.mark.parametrize("case", SK_CASES, ids=[c["id"] for c in SK_CASES])
def test_sk(store, case):
    seed(store, [room_item(), _item(R, case["sk"], case["attrs"], {})])
    if case["expect"] == INTERNAL:
        with pytest.raises(OpError) as exc:
            store.query_room(R, lenient=(case["read"] == "lenient"))
        assert exc.value.code == "internal"
        return
    assert case["sk"] in store.query_room(R).sks


# 숫자 필드의 형. 6.3 형 변환 문단. 항목 종류마다 정수 속성이 int 로 넘어오는지 본다. Decimal 은 int 와
# == 로 같으므로 값 비교만으로는 변환이 빠진 것이 드러나지 않는다.
INT_TYPES = [
    {"id": "room", "note": "ROOM 의 created_at_ms, expires_at_ms, host_peer_id, ttl"},
    {"id": "peer", "note": "PEER# 의 peer_id, joined_at_ms, ttl, 후보의 port"},
    {"id": "vip", "note": "VIP# 의 peer_id. ttl 은 RoomView 에 싣지 않는다"},
    {"id": "pair", "note": "PAIR# 의 ready_at_wall_ms, ready_at_mono_ns, punch_delay_ms, ttl"},
    {"id": "nonce", "note": "NONCE# 의 peer_id, ttl"},
]


@pytest.mark.store
@pytest.mark.parametrize("case", INT_TYPES, ids=[c["id"] for c in INT_TYPES])
def test_int_types(store, case):
    seed(store, FULL_ROOM)
    view = store.query_room(R)
    if case["id"] == "room":
        values = list(view.room.values()) + list(store.get_room(R).values())
    elif case["id"] == "peer":
        peer = view.peers[P]
        values = [peer["peer_id"], peer["joined_at_ms"], peer["ttl"]] + [c["port"] for c in peer["candidates"]]
    elif case["id"] == "vip":
        values = list(view.vips.values())  # RoomView 는 VIP# 의 peer_id 만 넘긴다
    elif case["id"] == "pair":
        pair = view.pairs[f"PAIR#{H}-{P}"]
        values = [pair[k] for k in ("ready_at_wall_ms", "ready_at_mono_ns", "punch_delay_ms", "ttl")]
    else:
        got = store.get_nonce(NON_P)
        values = [got["peer_id"], got["ttl"]]
    assert values and all(type(x) is int for x in values), values


# ---------------------------------------------------------------------------
# 출처: control_plane.md 6.5 TTL 표 (항목 | ttl 값). 한 행이고 항목 다섯을 나열한다. 항목마다 한 원소.
#   방의 모든 항목: floor(expires_at_ms / 1000) + STORAGE_GRACE_S
# via 는 그 항목을 만드는 쓰기다. EXP 는 초 경계에서 999ms 떨어져 있어 올림이나 반올림을 가른다.
# ---------------------------------------------------------------------------
TTL = [
    {"id": "ttl-room", "item": "`ROOM`", "via": "create", "keys": [K_ROOM],
     "note": "`floor(expires_at_ms / 1000) + STORAGE_GRACE_S`"},
    {"id": "ttl-peer", "item": "`PEER#`", "via": "create-join", "keys": [K_PEER_H, K_PEER_P],
     "note": "같은 방의 항목은 같은 `ttl` 을 갖는다"},
    {"id": "ttl-vip", "item": "`VIP#`", "via": "create-join", "keys": [K_VIP_H, K_VIP_P],
     "note": "같은 방의 항목은 같은 `ttl` 을 갖는다"},
    {"id": "ttl-pair", "item": "`PAIR#`", "via": "confirm", "keys": [K_PAIR],
     "note": "같은 방의 항목은 같은 `ttl` 을 갖는다"},
    {"id": "ttl-nonce", "item": "`NONCE#`", "via": "create-join", "keys": [K_NON_H, K_NON_P],
     "note": "pk 가 방이 아니어도 방의 값이다"},
]


@pytest.mark.store
@pytest.mark.parametrize("case", TTL, ids=[c["id"] for c in TTL])
def test_ttl(store, case):
    if case["via"] == "confirm":
        seed(store, [room_item(), host_item(candidates=CAND), p_item(candidates=CAND)])
        assert run(store, confirm()) == S.CONFIRMED
    else:
        assert run(store, create()) == S.CREATED
        assert run(store, join()) == S.JOINED
    for key in case["keys"]:
        assert raw(store, *key)["ttl"] == EXP_TTL, key


# ---------------------------------------------------------------------------
# 경합. 출처는 control_plane.md 4.6 host_report 의 "경합은 저장소 조건으로 막는다", 6.3 의 확인 행과
# "왜 이렇게 정했나" 첫 항, 9장 검증 표의 마지막 행(쓰기 직후 지연을 넣는 주입점), roadmap.md Phase 3
# 검증의 서버 회수 반대 순서와 커밋 뒤 실패 주입점. 순서는 store.on_write 의 "before" 에서 다른 쓰기를
# 끼워 넣어 만든다.
# ---------------------------------------------------------------------------
RACES = [
    {"id": "reclaim-after-register", "note": "4.6 서버 회수 표 registers-between-read-and-delete. 읽을 때 후보가 "
     "없었지만 지우기 전에 등록했다. 삭제 조건이 실패하고 피어가 남는다"},
    {"id": "register-after-reclaim", "note": "반대 순서. 토큰 검사를 통과한 뒤 쓰기 전에 회수가 커밋됐다. "
     "update 가 실패하고(부르는 쪽 unauthorized) 지워진 PEER# 가 되살아나지 않는다"},
    {"id": "confirm-after-release", "note": "6.3 확인 행. 읽고 나서 쓰기 전에 상대가 회수됐다. ConditionCheck "
     "가 실패하고 PAIR# 를 쓰지 않는다"},
    {"id": "renew-after-release", "note": "6.3 왜 이렇게 정했나. 첫 Query 뒤 같은 요청의 회수나 겹친 요청이 지운 "
     "PEER#·VIP# 를 ttl 만 가진 항목으로 되살리지 않는다. ROOM 갱신은 그대로 성공한다"},
    {"id": "join-after-room-deleted", "note": "6.3 join_room 행. 사전 읽기와 쓰기 사이에 방이 삭제됐다. 새 항목을 "
     "만들지 않는다"},
    {"id": "after-hook-fails-after-commit", "note": "roadmap Phase 3 의 커밋 뒤 응답 직전 실패 주입점. after 에서 "
     "던진 예외가 그대로 올라가고 쓰기는 커밋돼 있다"},
]


def once_before(st, label, action):
    """label 의 첫 "before" 에서 action 을 한 번 돌린다. action 안의 쓰기는 다시 끼어들지 않는다."""
    state = {"done": False}

    def hook(phase, lab, sk):
        if phase == "before" and lab == label and not state["done"]:
            state["done"] = True
            action()
    st.on_write = hook
    return state


@pytest.mark.store
@pytest.mark.parametrize("case", RACES, ids=[c["id"] for c in RACES])
def test_race(store, case):
    st = store
    rid = case["id"]
    if rid == "reclaim-after-register":
        seed(st, [room_item(), host_item(candidates=CAND), p_item(joined=NOW - G - 1), vip_item(VIP_P, P),
                  nonce_item(NON_P, P)])
        loose = st.query_room(R, lenient=True)
        assert reclaim_verdict(loose.peers[P], H, NOW) == (RECLAIM, None)
        state = once_before(st, "reclaim_peer", lambda: run(st, register()))
        assert run(st, reclaim()) is False
        assert state["done"]
        assert raw(st, *K_PEER_P)["candidates"] == CAND2
        assert raw(st, *K_VIP_P) is not None and raw(st, *K_NON_P) is not None
    elif rid == "register-after-reclaim":
        seed(st, [room_item(), p_item(joined=NOW - G - 1), vip_item(VIP_P, P), nonce_item(NON_P, P)])
        assert st.query_room(R).peers[P]["peer_token"] == TOK_P   # 토큰 검사는 통과했다
        state = once_before(st, "register_candidates", lambda: run(st, reclaim()))
        assert run(st, register()) is False
        assert state["done"]
        assert raw(st, *K_PEER_P) is None
    elif rid == "confirm-after-release":
        seed(st, [room_item(), host_item(candidates=CAND), p_item(candidates=CAND), vip_item(VIP_P, P)])
        state = once_before(st, "confirm_pair", lambda: run(st, release()))
        assert run(st, confirm()) == S.NOT_ELIGIBLE
        assert state["done"]
        assert raw(st, *K_PAIR) is None and raw(st, *K_PEER_P) is None
    elif rid == "renew-after-release":
        seed(st, [room_item(), host_item(), p_item(), vip_item(VIP_P, P)])
        sks = st.query_room(R).sks
        state = once_before(st, "renew_item", lambda: run(st, release()))
        assert run(st, renew(sks)) is True
        assert state["done"]
        assert raw(st, *K_PEER_P) is None and raw(st, *K_VIP_P) is None
        assert raw(st, *K_ROOM)["expires_at_ms"] == RENEW_EXP
        assert raw(st, *K_PEER_H)["ttl"] == RENEW_TTL
    elif rid == "join-after-room-deleted":
        seed(st, [room_item()])
        assert st.get_room(R) is not None
        state = once_before(st, "join_room", lambda: st.client.delete_item(
            TableName=st.table, Key={"pk": {"S": R}, "sk": {"S": "ROOM"}}))
        assert run(st, join()) == S.ROOM_GONE
        assert state["done"]
        assert raw(st, *K_VIP_P) is None and raw(st, *K_PEER_P) is None and raw(st, *K_NON_P) is None
    elif rid == "after-hook-fails-after-commit":
        seed(st, [room_item()])

        class Injected(Exception):
            pass

        def hook(phase, label, sk):
            if phase == "after":
                raise Injected
        st.on_write = hook
        with pytest.raises(Injected):
            run(st, join())
        assert raw(st, *K_VIP_P) is not None and raw(st, *K_NON_P) is not None
    else:
        pytest.fail(f"모르는 경합 행 {rid}")


@pytest.mark.store
def test_hook_sequence(store):
    """주입점은 쓰기마다 before 를 부르고, 성공한 쓰기에만 after 를 부른다."""
    seed(store, [room_item(), host_item()])
    seen = []
    store.on_write = lambda phase, label, sk: seen.append((phase, label, sk))
    assert run(store, renew(["ROOM", f"PEER#{H}", f"PEER#{P}"])) is True
    assert seen == [
        ("before", "renew_room", "ROOM"), ("after", "renew_room", "ROOM"),
        ("before", "renew_item", f"PEER#{H}"), ("after", "renew_item", f"PEER#{H}"),
        ("before", "renew_item", f"PEER#{P}"),
    ]
    seen.clear()
    assert run(store, join()) == S.JOINED
    assert run(store, join()) == S.NONCE_EXISTS
    assert seen == [("before", "join_room", f"VIP#{VIP_P}"), ("after", "join_room", f"VIP#{VIP_P}"),
                    ("before", "join_room", f"VIP#{VIP_P}")]


# ---------------------------------------------------------------------------
# 출처: control_plane.md 7.6 설정과 배포, 변수 표 가운데 store 가 읽는 셋 (변수 | 뜻 | 기본값).
# SANGTACHI_CP_PORT 는 서버의 것이라 여기서 보지 않는다. 클라이언트를 만들기만 하고 부르지 않는다.
# ---------------------------------------------------------------------------
CONFIG_CASES = [
    {"id": "table-required", "env": {"AWS_REGION": "local-only"}, "expect": "SANGTACHI_CP_TABLE",
     "note": "SANGTACHI_CP_TABLE. 없음. 필수"},
    {"id": "table-empty", "env": {"SANGTACHI_CP_TABLE": "", "AWS_REGION": "local-only"},
     "expect": "SANGTACHI_CP_TABLE",
     "note": "빈 값은 없는 것과 같다"},
    {"id": "region-required", "env": {"SANGTACHI_CP_TABLE": "t"}, "expect": "AWS_REGION",
     "note": "AWS_REGION. 없음. 필수. boto3 표준 변수"},
    {"id": "endpoint-optional", "env": {"SANGTACHI_CP_TABLE": "t", "AWS_REGION": "us-east-2"},
     "expect": "https://dynamodb.us-east-2.amazonaws.com", "note": "SANGTACHI_CP_ENDPOINT. 없으면 리전 기본"},
    {"id": "endpoint-override", "env": {"SANGTACHI_CP_TABLE": "t", "AWS_REGION": "local-only",
                                        "SANGTACHI_CP_ENDPOINT": "http://127.0.0.1:8001"},
     "expect": "http://127.0.0.1:8001", "note": "로컬 시험용 덮어쓰기"},
]


@pytest.mark.parametrize("case", CONFIG_CASES, ids=[c["id"] for c in CONFIG_CASES])
def test_config(case):
    # expect 가 변수 이름이면 그 변수를 짚는 ValueError 다. boto3 가 다른 이유로 내는 ValueError 와 가른다.
    if not case["expect"].startswith("http"):
        with pytest.raises(ValueError, match=case["expect"]):
            S.Store.from_env(case["env"])
        return
    st = S.Store.from_env(case["env"])
    assert st.table == "t"
    assert st.client.meta.endpoint_url == case["expect"]
    assert st.client.meta.region_name == case["env"]["AWS_REGION"]


# ---------------------------------------------------------------------------
# 하네스. 출처는 control_plane.md 7.6 의 로컬 시험 문단: 하네스는 엔드포인트가 루프백일 때만 저장소
# 시험을 돈다. 허용 목록 {127.0.0.1, localhost, ::1}. 모르는 것은 거부한다.
# ---------------------------------------------------------------------------
ENDPOINT_CASES = [
    {"id": "v4", "url": "http://127.0.0.1:8001", "expect": "http://127.0.0.1:8001", "note": "기본값"},
    {"id": "localhost", "url": "http://localhost:8001/", "expect": "http://localhost:8001", "note": "허용 목록"},
    {"id": "localhost-upper", "url": "http://LOCALHOST:8001", "expect": "http://localhost:8001",
     "note": "호스트 이름은 대소문자를 가리지 않는다"},
    {"id": "v6", "url": "http://[::1]:8001", "expect": "http://[::1]:8001", "note": "허용 목록"},
    {"id": "other-loopback", "url": "http://127.0.0.2:8001", "expect": None,
     "note": "127/8 의 다른 주소도 목록에 없으면 거부한다. 판정 함수를 쓰지 않는다"},
    {"id": "remote", "url": "http://dynamodb.us-east-1.amazonaws.com:80", "expect": None, "note": "실제 AWS"},
    {"id": "https", "url": "https://127.0.0.1:8001", "expect": None, "note": "http 만"},
    {"id": "userinfo", "url": "http://127.0.0.1:8001@example.com:8001", "expect": None,
     "note": "사용자 정보 뒤의 실제 호스트"},
    {"id": "userinfo-loopback", "url": "http://user:pw@127.0.0.1:8001", "expect": None,
     "note": "호스트가 루프백이어도 사용자 정보가 있으면 거부한다. 무엇을 뜻하는지 모르는 URL 이다"},
    {"id": "no-port", "url": "http://127.0.0.1", "expect": None, "note": "포트를 짐작하지 않는다"},
    {"id": "bad-port", "url": "http://127.0.0.1:99999", "expect": None, "note": "범위 밖"},
    {"id": "path", "url": "http://127.0.0.1:8001/x", "expect": None, "note": "경로"},
    {"id": "newline", "url": "http://127.0.0.1:8001\n", "expect": None,
     "note": "urlsplit 은 줄바꿈을 지우고 읽는다. 받은 문자열을 그대로 넘기지 않는다"},
    {"id": "localhost-dot", "url": "http://localhost.:8001", "expect": None, "note": "목록과 정확히 같아야 한다"},
    {"id": "percent", "url": "http://127.0.0.%31:8001", "expect": None, "note": "퍼센트 인코딩"},
    {"id": "empty", "url": "", "expect": None, "note": "빈 값"},
]


@pytest.mark.parametrize("case", ENDPOINT_CASES, ids=[c["id"] for c in ENDPOINT_CASES])
def test_endpoint(case):
    if case["expect"] is None:
        with pytest.raises(ValueError):
            loopback_endpoint(case["url"])
    else:
        assert loopback_endpoint(case["url"]) == case["expect"]


def _pytest(*args):
    return subprocess.run([sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider", *args],
                          cwd=CP_ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=120)


def test_remote_endpoint_is_usage_error():
    proc = _pytest("--store", "--store-endpoint", "http://example.com:8001", "--collect-only",
                   "tests/test_store.py")
    assert proc.returncode == 4, proc.stdout + proc.stderr


def test_unreachable_endpoint_fails_not_skips():
    # 1 번 포트는 열려 있지 않다. 닿지 않으면 건너뛰지 않고 떨어져야 한다.
    proc = _pytest("--store", "--store-endpoint", "http://127.0.0.1:1",
                   "tests/test_store.py::test_read_absent")
    out = proc.stdout + proc.stderr
    assert proc.returncode == 1, out
    assert "skipped" not in out and "닿지 않는다" in out


# ---------------------------------------------------------------------------
# 격리. 출처는 control_plane.md 7.6 의 로컬 시험 문단 ("사용자의 AWS 프로파일과 설정 파일도 읽지 않게
# 막는다"). conftest.aws_isolation 이 모든 시험에 건다. 아래 행은 그 상태와, store 가 기본 세션과
# 환경 프록시에 기대지 않는지를 본다. 저장소에 닿지 않는다.
# ---------------------------------------------------------------------------
ALLOWED_AWS_VARS = {"AWS_CONFIG_FILE", "AWS_SHARED_CREDENTIALS_FILE", "AWS_ACCESS_KEY_ID", "AWS_SECRET_ACCESS_KEY",
                    "AWS_EC2_METADATA_DISABLED"}
FAR_PROXY = "http://10.255.255.1:3128"
LOCAL = "http://127.0.0.1:8001"
ENV = {"SANGTACHI_CP_TABLE": "t", "AWS_REGION": "local-only", "SANGTACHI_CP_ENDPOINT": LOCAL}


def test_isolation_env():
    import boto3

    left = {k for k in os.environ if k.upper().startswith("AWS_")}
    assert left <= ALLOWED_AWS_VARS, left - ALLOWED_AWS_VARS
    assert os.environ["AWS_ACCESS_KEY_ID"] == FAKE_KEY_ID
    assert os.environ["AWS_EC2_METADATA_DISABLED"] == "true"
    for name in ("AWS_CONFIG_FILE", "AWS_SHARED_CREDENTIALS_FILE"):
        assert Path(os.environ[name]).read_text(encoding="ascii") == ""
    assert not any(name in os.environ for name in PROXY_VARS if name.upper() == name)
    assert os.environ.get("NO_PROXY") == NO_PROXY_VALUE
    assert boto3.DEFAULT_SESSION is None


def test_isolation_under_polluted_env(tmp_path):
    """오염된 환경에서 위 시험을 다시 돌린다. 바깥 환경이 깨끗하면 위 시험은 아무것도 지키지 않는다."""
    profile = tmp_path / "config"
    profile.write_text("[default]\ncredential_process = cmd /c exit 1\nregion = us-east-1\n", encoding="ascii")
    polluted = dict(os.environ, AWS_PROFILE="leak", AWS_DEFAULT_PROFILE="leak", AWS_SESSION_TOKEN="leak",
                    AWS_CONFIG_FILE=str(profile), AWS_ENDPOINT_URL="http://example.com", AWS_EC2_METADATA_DISABLED="false",
                    HTTP_PROXY=FAR_PROXY, HTTPS_PROXY=FAR_PROXY, ALL_PROXY=FAR_PROXY, NO_PROXY="example.com")
    proc = subprocess.run([sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider",
                           "tests/test_store.py::test_isolation_env", "tests/test_store.py::test_default_session_ignored"],
                          cwd=CP_ROOT, env=polluted, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=120)
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_default_session_ignored():
    """store 는 boto3.DEFAULT_SESSION 이 캐시한 자격 증명을 쓰지 않는다."""
    import boto3

    boto3.DEFAULT_SESSION = boto3.session.Session(aws_access_key_id="sentinelCachedKey",
                                                  aws_secret_access_key="sentinel", region_name="local-only")
    st = S.Store.from_env(ENV)
    assert st.client._request_signer._credentials.access_key == FAKE_KEY_ID


def test_default_session_restored(monkeypatch, tmp_path):
    """격리는 시험 동안만 기본 세션을 비우고 끝나면 되돌린다. 남의 상태를 바꾼 채 두지 않는다."""
    import boto3

    sentinel = object()
    boto3.DEFAULT_SESSION = sentinel
    inner = tmp_path / "inner"
    inner.mkdir()
    with isolate_aws(monkeypatch, inner):
        assert boto3.DEFAULT_SESSION is None
    assert boto3.DEFAULT_SESSION is sentinel


PROXY_CASES = [
    {"id": "config-off", "proxy": FAR_PROXY, "no_proxy": None, "config": True, "expect": None,
     "note": "시험 대상 클라이언트는 Config 로 프록시를 끈다. NO_PROXY 가 없어도 프록시로 가지 않는다"},
    {"id": "no-proxy-bypass", "proxy": FAR_PROXY, "no_proxy": NO_PROXY_VALUE, "config": False, "expect": None,
     "note": "격리 픽스처의 NO_PROXY 가 루프백을 우회시킨다"},
    {"id": "control-env-proxy", "proxy": FAR_PROXY, "no_proxy": None, "config": False, "expect": FAR_PROXY,
     "note": "대조 행. 둘 다 없으면 환경 프록시를 쓴다. 이 행이 있어야 위 두 행이 무언가를 본 것이다"},
]


@pytest.mark.parametrize("case", PROXY_CASES, ids=[c["id"] for c in PROXY_CASES])
def test_proxy(case, monkeypatch):
    monkeypatch.setenv("HTTP_PROXY", case["proxy"])
    for name in ("NO_PROXY", "no_proxy"):
        if case["no_proxy"] is None:
            monkeypatch.delenv(name, raising=False)
        else:
            monkeypatch.setenv(name, case["no_proxy"])
    st = S.Store.from_env(ENV, config=no_proxy_config() if case["config"] else None)
    assert proxy_for(st.client, LOCAL) == case["expect"]


@pytest.mark.store
def test_store_client_has_no_proxy(store):
    assert store.client.meta.config.proxies == {}
    assert proxy_for(store.client, store.client.meta.endpoint_url) is None


# ---------------------------------------------------------------------------
# 문서 행이 빠지지 않았는지 센다. 쓰기 표는 행마다 두 결과(pass, fail)가 다 있어야 한다.
# ---------------------------------------------------------------------------
def test_every_doc_row_is_exercised():
    for row in WRITES:
        sides = {c["side"] for c in WRITE_CASES if c["row"] == row["id"]}
        assert sides == {"pass", "fail"}, row["id"]
    cancel_rows = {c["row"] for c in CANCEL_CASES} | {c["row"] for c in CLASSIFY_CASES}
    assert cancel_rows == {r["id"] for r in CANCEL_ORDER}
    assert len(WRITES) == 7 and len(CANCEL_ORDER) == 6 and len(READS) == 5 and len(TTL) == 5
