"""control_plane.md 4.5 get_peers. peers 원소의 필드 집합 케이스 표.

표는 이 파일이 출처다. 행을 고치려면 여기를 고친다. 변이는 tests/mutants/get_peers.py.
"""

import pytest

from controlplane.constants import MAX_CANDIDATES, PUNCH_DELAY_MS
from controlplane.ops import pair_sk, peer_element

BASE_KEYS = {"peer_id", "virtual_ip", "ready"}
READY_KEYS = BASE_KEYS | {"punch_delay_ms", "elapsed_since_ready_ms", "candidates"}

# 원소가 될 상대 피어의 PEER# 항목(6.3). 후보는 일부러 정렬되지 않은 순서로 둔다. 저장된 순서가
# 그대로 나가는지 보려는 것이다. PAIR# 가 없는 행에서도 후보가 있다. 준비 전에 후보가 새는지 본다.
PEER = {
    "peer_id": 3_000_000_001,
    "peer_token": "0" * 32,
    "virtual_ip": "10.100.0.2",
    "candidates": [
        {"ip": "203.0.113.7", "port": 51000, "kind": "reflexive"},
        {"ip": "192.168.0.10", "port": 51000, "kind": "local"},
        {"ip": "10.0.0.5", "port": 1, "kind": "local"},
    ],
    "client_nonce": "n",
    "joined_at_ms": 1_700_000_000_000,
}

# 그 쌍의 PAIR# 항목(6.3). punch_delay_ms 는 PUNCH_DELAY_MS 와 다른 값이다. 응답이 상수가 아니라
# "그 쌍에 기록된 값" 을 내는지 보려는 것이다.
PAIR = {
    "ready_at_wall_ms": 1_700_000_001_000,
    "ready_at_mono_ns": 5_000_000_000,
    "ready_boot_id": "boot-a",
    "punch_delay_ms": PUNCH_DELAY_MS + 234,
}

ELAPSED = 4321  # 주입한 경과 함수가 돌려주는 값

# ---------------------------------------------------------------------------
# 출처: control_plane.md 4.5 get_peers, 원소의 필드 집합 케이스 표
#       (그 쌍의 상태 | 원소에 있는 필드). 원소의 필드 집합은 ready 가 가른다.
# 표 바로 아래의 근거:
#   왜. 준비 전에 후보를 주면 클라이언트가 호스트의 확인 없이 펀치를 시작할 수 있다. 펀치 시각을
#   맞추는 것이 4.6 의 확인이므로(5.2) 그 전에는 쏠 주소를 주지 않는다.
# ---------------------------------------------------------------------------
FIELD_SETS = [
    {
        "id": "pair-absent",
        "pair_state": "`PAIR#` 없음",
        "fields": "`peer_id`, `virtual_ip`, `ready: false`",
        "pair": None,
        "keys": BASE_KEYS,
        "ready": False,
        "note": "준비 전에는 쏠 주소를 주지 않는다. 후보가 저장돼 있어도 원소에 없다",
    },
    {
        "id": "pair-present",
        "pair_state": "`PAIR#` 있음",
        "fields": "위에 더해 `punch_delay_ms`, `elapsed_since_ready_ms`, `candidates`",
        "pair": PAIR,
        "keys": READY_KEYS,
        "ready": True,
        "note": "punch_delay_ms 는 그 쌍에 기록된 값, elapsed_since_ready_ms 는 7.4, "
                "candidates 는 저장된 순서대로",
    },
]


@pytest.mark.parametrize("case", FIELD_SETS, ids=[c["id"] for c in FIELD_SETS])
def test_field_set(case):
    seen = []

    def elapsed(pair):
        seen.append(pair)
        return ELAPSED

    element = peer_element(PEER, case["pair"], elapsed)
    assert set(element) == case["keys"]
    assert element["ready"] is case["ready"]
    assert element["peer_id"] == PEER["peer_id"]
    assert element["virtual_ip"] == PEER["virtual_ip"]
    if not case["ready"]:
        assert seen == []
        return
    assert element["punch_delay_ms"] == PAIR["punch_delay_ms"]
    assert element["elapsed_since_ready_ms"] == ELAPSED
    assert seen == [PAIR]
    assert element["candidates"] == PEER["candidates"]


# 표 밖 행. pair-present 행의 "candidates 는 저장된 순서대로" 를 상한(MAX_CANDIDATES)까지 본다.
# 위 PEER 는 후보가 3개라 앞 몇 개만 복사하는 구현을 잡지 못한다.
EIGHT = [{"ip": f"203.0.113.{i + 1}", "port": 51000 + (7 - i), "kind": "reflexive" if i % 2 else "local"}
         for i in range(MAX_CANDIDATES)]


def test_all_candidates_in_order():
    element = peer_element(dict(PEER, candidates=EIGHT), PAIR, lambda pair: 0)
    assert element["candidates"] == EIGHT


def test_candidates_are_copies():
    # 응답 조립이 저장 항목을 바꾸지 않는다. 표 행이 아니라 위 행의 부수 조건이다.
    element = peer_element(PEER, PAIR, lambda pair: 0)
    element["candidates"][0]["port"] = 9
    assert PEER["candidates"][0]["port"] == 51000


# ---------------------------------------------------------------------------
# 출처: control_plane.md 6.3 항목의 PAIR#<lo>-<hi> 행. 문서에 표는 없다.
#   lo 와 hi 는 두 peer_id 를 10진수로 적고 오름차순으로 붙인다.
# 4.5 의 ready 는 이 키의 항목이 있는가이므로 키가 어긋나면 준비 완료를 못 본다.
# ---------------------------------------------------------------------------
PAIR_KEYS = [
    {"id": "ordered", "a": 1, "b": 2, "expect": "PAIR#1-2", "note": "오름차순"},
    {"id": "reversed", "a": 2, "b": 1, "expect": "PAIR#1-2", "note": "인자 순서와 무관하다"},
    {"id": "numeric-not-text", "a": 10, "b": 9, "expect": "PAIR#9-10",
     "note": "수의 오름차순이다. 문자열로 정렬하면 10-9 가 된다"},
    {"id": "uint32-max", "a": 4_294_967_295, "b": 1, "expect": "PAIR#1-4294967295", "note": "10진수"},
]


@pytest.mark.parametrize("case", PAIR_KEYS, ids=[c["id"] for c in PAIR_KEYS])
def test_pair_sk(case):
    assert pair_sk(case["a"], case["b"]) == case["expect"]
