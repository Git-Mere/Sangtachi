"""control_plane.md 4.6 host_report. 확인 경합 케이스 표와 서버 회수 케이스 표.

표는 이 파일이 출처다. 행을 고치려면 여기를 고친다. 변이는 tests/mutants/host_report.py.

needs="store" 인 행은 DynamoDB local 과 조건부 쓰기, 쓰기 직후 지연 주입이 있어야 돈다. 지금은
그 계층이 없으므로 행만 옮겨 두고 skip 한다. 저장소를 흉내 내지 않는다.
"""

import pytest

from controlplane.constants import JOIN_REGISTER_GRACE_S
from controlplane.ops import KEEP, RECLAIM, SKIP, confirm_allowed, reclaim_verdict

STORE_SKIP = "store.py 와 DynamoDB local 이 생기면 돈다 (plan 2단계). 지금은 표 행만 옮겨 두었다"


def params(table):
    out = []
    for c in table:
        marks = [pytest.mark.store, pytest.mark.skip(reason=STORE_SKIP)] if c.get("needs") == "store" else []
        out.append(pytest.param(c, id=c["id"], marks=marks))
    return out


# ---------------------------------------------------------------------------
# 출처: control_plane.md 4.6 host_report, 케이스 표 (확인 경합).
#   H 는 호스트의 host_report, P 는 플레이어의 register_candidate 다.
# 열: 순서 | 5번 읽기가 보는 것 | PAIR# 기록 | 호스트가 다음에 하는 것.
# pair_writes 는 "PAIR# 기록" 열의 횟수다. 문서가 횟수를 적지 않은 행은 None.
# 전부 저장소가 있어야 돈다. 마지막 행은 결함 있는 구현을 돌려 이 표가 잡는지 보는 행이다.
# ---------------------------------------------------------------------------
CONFIRM_RACE = [
    {"id": "p-then-h-confirm", "needs": "store",
     "order": "`P` → `H(confirm=[P])`", "read_sees": "P 의 후보 있음",
     "pair_record": "1회", "pair_writes": 1, "host_next": "펀치"},
    {"id": "h-confirm-then-p", "needs": "store",
     "order": "`H(confirm=[P])` → `P`", "read_sees": "P 의 후보 없음",
     "pair_record": "**0회.** 3번의 조건이 안 맞는다", "pair_writes": 0,
     "host_next": "다음 주기에 다시 `confirm`"},
    {"id": "h-without-confirm", "needs": "store",
     "order": "`H` 가 `confirm` 없이 부름", "read_sees": "P 의 후보 있음",
     "pair_record": "0회", "pair_writes": 0,
     "host_next": "응답의 `peers` 를 보고 다음 주기에 `confirm`"},
    {"id": "h-twice-same-p", "needs": "store",
     "order": "같은 `P` 로 `H` 를 두 번", "read_sees": "—",
     "pair_record": "1회. 두 번째는 `PAIR#` 가 이미 있다", "pair_writes": 1, "host_next": "없음"},
    {"id": "departed-and-confirm-same-p", "needs": "store",
     "order": "`H(departed=[P], confirm=[P])`", "read_sees": "P 없음",
     "pair_record": "0회", "pair_writes": 0, "host_next": "없음"},
    {"id": "unconditional-write-impl", "needs": "store",
     "order": "3번을 조건 없이 쓰는 구현", "read_sees": "—",
     "pair_record": "**후보 없는 쌍에도 기록. 이 표가 잡는 결함이다**", "pair_writes": None,
     "host_next": "—"},
]


@pytest.mark.parametrize("case", params(CONFIRM_RACE))
def test_confirm_race(case):
    pytest.fail("store 시험이 아직 없다")  # pragma: no cover


# ---------------------------------------------------------------------------
# 출처: control_plane.md 4.6 host_report, 처리 순서 3번과 6.3 의 host_report 확인 조건.
# 문서에 표는 없다. 위 확인 경합 표의 순수한 핵심(읽은 값으로 본 사전 조건)만 펼친다.
#   3. confirm 의 각 peer_id 에 대해, 그 피어와 호출자가 둘 다 후보를 1개 이상 갖고 있고
#      그 쌍의 PAIR# 가 없으면 조건부 쓰기로 만든다.
# ---------------------------------------------------------------------------
WITH = {"candidates": [{"ip": "203.0.113.7", "port": 51000, "kind": "reflexive"}]}
EMPTY = {"candidates": []}
NO_ATTR = {}
PAIR = {"punch_delay_ms": 1000}

CONFIRM_CORE = [
    {"id": "both-have", "host": WITH, "other": WITH, "pair": None, "expect": True,
     "note": "둘 다 후보 1개 이상, PAIR# 없음. 확인 경합 표 1행의 사전 조건"},
    {"id": "other-empty", "host": WITH, "other": EMPTY, "pair": None, "expect": False,
     "note": "상대 후보 없음. 확인 경합 표 2행. 쓰면 get_peers 가 빈 후보와 함께 준비 완료를 준다"},
    {"id": "other-no-attr", "host": WITH, "other": NO_ATTR, "pair": None, "expect": False,
     "note": "candidates 속성이 없는 joined 피어(6.3 서버 회수 조건의 attribute_not_exists)"},
    {"id": "host-empty", "host": EMPTY, "other": WITH, "pair": None, "expect": False,
     "note": "호출자 쪽도 후보가 있어야 한다. '둘 다'"},
    {"id": "other-absent", "host": WITH, "other": None, "pair": None, "expect": False,
     "note": "상대 PEER# 가 없다. 확인 경합 표 5행처럼 회수된 피어"},
    {"id": "pair-exists", "host": WITH, "other": WITH, "pair": PAIR, "expect": False,
     "note": "이미 확인된 쌍. 확인 경합 표 4행의 두 번째 호출"},
]


@pytest.mark.parametrize("case", params(CONFIRM_CORE))
def test_confirm_core(case):
    assert confirm_allowed(case["host"], case["other"], case["pair"]) is case["expect"]


# ---------------------------------------------------------------------------
# 출처: control_plane.md 4.6 host_report, 서버 회수 케이스 표 (피어 | 시각 | 결과).
#   J 는 joined_at_ms, G 는 JOIN_REGISTER_GRACE_S * 1000 이다.
# 대상 조건: 호스트가 아니다. 후보가 비어 있다(5.3 joined). joined_at_ms + G <= now.
# expect 는 ops.reclaim_verdict 의 판정이다. SKIP 은 남기고 7.5 peer.reclaim_skipped 를 낸다.
# ---------------------------------------------------------------------------
HOST_ID = 11
PEER_ID = 22
J = 1_700_000_000_000
G = JOIN_REGISTER_GRACE_S * 1000


def peer(**over):
    base = {"peer_id": PEER_ID, "virtual_ip": "10.100.0.2", "candidates": [], "client_nonce": "n",
            "joined_at_ms": J}
    base.update(over)
    return {k: v for k, v in base.items() if v is not ...}


RECLAIM_CASES = [
    {"id": "empty-before-grace", "who": "후보 없음", "when": "`now < J + G`",
     "peer": peer(), "now": J + G - 1, "expect": KEEP, "reason": None, "note": "남는다"},
    {"id": "empty-at-grace", "who": "후보 없음", "when": "`now == J + G`",
     "peer": peer(), "now": J + G, "expect": RECLAIM, "reason": None,
     "note": "**지운다.** 경계는 회수 쪽이다. `released` 에 담긴다"},
    {"id": "empty-after-grace", "who": "후보 없음", "when": "`now > J + G`",
     "peer": peer(), "now": J + G + 1, "expect": RECLAIM, "reason": None,
     "note": "지운다. 그 주소가 다시 배정 대상이 된다. 어느 참가가 받는지는 2.5 배정 순서다"},
    {"id": "has-candidates-after-grace", "who": "후보 있음", "when": "`now > J + G`",
     "peer": peer(candidates=[{"ip": "203.0.113.7", "port": 51000, "kind": "reflexive"}]),
     "now": J + G + 1, "expect": KEEP, "reason": None,
     "note": "남는다. 터널이 있을 수 있는 피어는 호스트만 회수한다"},
    {"id": "registers-between-read-and-delete", "needs": "store",
     "who": "후보 없음, 읽은 뒤 지우기 전에 등록", "when": "`now > J + G`",
     "peer": None, "now": None, "expect": KEEP, "reason": None,
     "note": "**남는다.** 삭제 조건이 실패한다. 조건 없이 지우는 구현이 이 행에서 걸린다"},
    {"id": "host-self-empty", "who": "호스트 자신, 후보 없음", "when": "`now > J + G`",
     "peer": peer(peer_id=HOST_ID), "now": J + G + 1, "expect": KEEP, "reason": None,
     "note": "남는다. 지우면 호출자 자신이 사라진다"},
    {"id": "join-retry-with-reclaimed-nonce", "needs": "store",
     "who": "회수된 피어의 nonce 로 `join_room` 재시도", "when": "-",
     "peer": None, "now": None, "expect": None, "reason": None,
     "note": "새 참가다. `NONCE#` 가 없으므로 4.3 의 멱등 응답이 아니다. 위 \"왜 90초인가\" 의 첫 항 "
             "때문에 정상 클라이언트의 재시도는 회수보다 먼저 끝난다"},
    {"id": "joined-at-missing", "who": "`joined_at_ms` 필드 없음", "when": "-",
     "peer": peer(joined_at_ms=...), "now": J + G + 1, "expect": SKIP,
     "reason": "joined_at_unreadable",
     "note": "**남는다.** 판정 불가를 회수로 바꾸지 않는다. 7.5 로그의 `peer.reclaim_skipped` 를 "
             "남긴다. 요청 전체를 `internal` 로 끝내면 그 방의 임대 갱신까지 막힌다"},
]


@pytest.mark.parametrize("case", params(RECLAIM_CASES))
def test_reclaim(case):
    assert reclaim_verdict(case["peer"], HOST_ID, case["now"]) == (case["expect"], case["reason"])


# ---------------------------------------------------------------------------
# 표 밖 행. 출처는 control_plane.md 4.6 서버 회수의 판정 순서 표다.
#   판정 순서: peer_id 판독 -> 호스트면 KEEP -> 후보 판독 -> 후보 있으면 KEEP -> joined_at_ms 판독
#   -> 경계 -> 지울 키(virtual_ip, client_nonce) 판독. 읽을 수 없으면 SKIP 이고 회수하지 않는다
#   (위 표 마지막 행과 같은 원칙).
#   candidates 속성이 없으면 빈 것이다. 6.3 서버 회수 조건의 attribute_not_exists(candidates) 와 같다.
# 모든 행이 now > J + G 다. 판독 규칙이 없으면 회수로 떨어지는 시각이다.
# ---------------------------------------------------------------------------
CAND = [{"ip": "203.0.113.7", "port": 51000, "kind": "reflexive"}]

RECLAIM_READ = [
    {"id": "candidates-attr-absent", "peer": peer(candidates=...), "expect": RECLAIM, "reason": None,
     "note": "속성 없음은 빈 것이다 (5.3 joined, 6.3 조건). 경계 행과 같이 회수한다"},
    {"id": "peer-id-absent", "peer": peer(peer_id=...), "expect": SKIP, "reason": "peer_id_unreadable",
     "note": "호스트인지 알 수 없다. 호스트를 지우면 호출자 자신이 사라진다"},
    {"id": "peer-id-not-int", "peer": peer(peer_id=str(PEER_ID)), "expect": SKIP, "reason": "peer_id_unreadable",
     "note": "정수가 아니면 없는 것과 같다. 저장소 숫자는 store 가 int 로 바꿔 넘긴다"},
    {"id": "peer-id-bool", "peer": peer(peer_id=True), "expect": SKIP, "reason": "peer_id_unreadable",
     "note": "bool 은 Python 에서 int 의 하위형이지만 peer_id 가 아니다"},
    {"id": "peer-id-absent-with-candidates", "peer": peer(peer_id=..., candidates=CAND), "expect": SKIP,
     "reason": "peer_id_unreadable", "note": "peer_id 판독이 맨 앞이다"},
    {"id": "candidates-not-list", "peer": peer(candidates="x"), "expect": SKIP,
     "reason": "candidates_unreadable", "note": "리스트가 아니면 후보가 있는지 알 수 없다"},
    {"id": "candidates-null", "peer": peer(candidates=None), "expect": SKIP,
     "reason": "candidates_unreadable", "note": "속성은 있는데 리스트가 아니다. 없음과 다르다"},
    {"id": "candidates-not-list-host", "peer": peer(peer_id=HOST_ID, candidates="x"), "expect": KEEP,
     "reason": None, "note": "호스트 판정이 후보 판독보다 먼저다. 호스트는 판독할 필요 없이 남는다"},
    {"id": "joined-at-not-int", "peer": peer(joined_at_ms=str(J)), "expect": SKIP,
     "reason": "joined_at_unreadable", "note": "정수가 아니면 없는 것과 같다"},
    {"id": "joined-at-bool", "peer": peer(joined_at_ms=True), "expect": SKIP, "reason": "joined_at_unreadable",
     "note": "bool 은 Python 에서 int 의 하위형이지만 시각이 아니다. True 를 1 로 읽으면 회수한다"},
    {"id": "joined-at-absent-with-candidates", "peer": peer(joined_at_ms=..., candidates=CAND), "expect": KEEP,
     "reason": None, "note": "후보가 있으면 joined_at_ms 를 볼 필요가 없다. 로그를 내지 않는다"},
    {"id": "joined-at-absent-host", "peer": peer(peer_id=HOST_ID, joined_at_ms=...), "expect": KEEP,
     "reason": None, "note": "호스트는 joined_at_ms 를 볼 필요가 없다"},
    # 판정 순서 6번. 유예가 지났을 때 지울 키(VIP#, NONCE#)를 만들 수 있는가.
    {"id": "keys-vip-absent", "peer": peer(virtual_ip=...), "expect": SKIP, "reason": "keys_unreadable",
     "note": "virtual_ip 가 없으면 VIP# 키를 만들 수 없다. 지우지 않는다"},
    {"id": "keys-vip-not-str", "peer": peer(virtual_ip=167772162), "expect": SKIP, "reason": "keys_unreadable",
     "note": "문자열이 아니면 없는 것과 같다"},
    {"id": "keys-nonce-absent", "peer": peer(client_nonce=...), "expect": SKIP, "reason": "keys_unreadable",
     "note": "client_nonce 가 없으면 NONCE# 키를 만들 수 없다"},
    {"id": "keys-nonce-not-str", "peer": peer(client_nonce=7), "expect": SKIP, "reason": "keys_unreadable",
     "note": "문자열이 아니면 없는 것과 같다"},
    {"id": "keys-absent-before-grace", "peer": peer(virtual_ip=..., joined_at_ms=J + 2), "expect": KEEP,
     "reason": None, "note": "5번이 6번보다 먼저다. 유예가 남았으면 키를 볼 필요 없이 남고 로그를 내지 않는다"},
]


@pytest.mark.parametrize("case", params(RECLAIM_READ))
def test_reclaim_read(case):
    assert reclaim_verdict(case["peer"], HOST_ID, J + G + 1) == (case["expect"], case["reason"])
