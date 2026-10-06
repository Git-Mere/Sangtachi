"""control_plane.md 5.1 방. 상태 표, 만료 판정 케이스 표, expires_in_s.

표는 이 파일이 출처다. 행을 고치려면 여기를 고친다. 변이는 tests/mutants/room_state.py.
"""

import pytest

from controlplane.constants import ROOM_LEASE_S
from controlplane.errors import OpError
from controlplane.ops import ROOM_EXPIRED, ROOM_OPEN, expires_in_s, room_state

NOW = 1_700_000_000_000  # 서버 벽시계 밀리초. 값 자체는 뜻이 없다

# ---------------------------------------------------------------------------
# 출처: control_plane.md 5.1 방, 상태 표 (상태 | 정의).
# 방의 상태는 저장된 필드에서 파생한다. 상태 필드를 따로 두지 않는다.
# 왜: 상태 필드를 두면 필드와 상태가 어긋나는 경우가 생긴다.
# room 은 ROOM 항목(6.3), None 은 항목이 없다는 뜻이다.
# ---------------------------------------------------------------------------
STATES = [
    {
        "id": "open",
        "state": "`open`",
        "note": "`ROOM` 항목이 있고 `now < expires_at`",
        "room": {"expires_at_ms": NOW + 60_000},
        "expect": ROOM_OPEN,
    },
    {
        "id": "expired",
        "state": "`expired`",
        "note": "`ROOM` 항목이 있고 `now >= expires_at`. TTL 삭제 전까지 이 상태로 보인다",
        "room": {"expires_at_ms": NOW - 60_000},
        "expect": ROOM_EXPIRED,
    },
    {
        "id": "absent",
        "state": "(없음)",
        "note": "`ROOM` 항목이 없다. 처음부터 없었거나 TTL 이 지웠다",
        "room": None,
        "expect": None,
    },
]

# ---------------------------------------------------------------------------
# 출처: control_plane.md 5.1 방, 만료 판정 케이스 표 (now 와 expires_at_ms | 판정).
# now 는 서버 벽시계 밀리초, expires_at_ms 는 저장값이다.
# expect 가 "internal" 이면 OpError("internal") 이다.
# ---------------------------------------------------------------------------
EXPIRY = [
    {
        "id": "now-before",
        "when": "`now < expires_at_ms`",
        "room": {"expires_at_ms": NOW + 1},
        "expect": ROOM_OPEN,
        "note": "살아 있음",
    },
    {
        "id": "now-equal",
        "when": "`now == expires_at_ms`",
        "room": {"expires_at_ms": NOW},
        "expect": ROOM_EXPIRED,
        "note": "**만료.** 경계는 만료 쪽이다. `expires_in_s` 가 0 인 방은 참가할 수 없다",
    },
    {
        "id": "now-after",
        "when": "`now > expires_at_ms`",
        "room": {"expires_at_ms": NOW - 1},
        "expect": ROOM_EXPIRED,
        "note": "만료",
    },
    {
        "id": "field-missing",
        "when": "`expires_at_ms` 필드 없음",
        "room": {"created_at_ms": NOW - 1_000, "host_peer_id": 7},
        "expect": "internal",
        "note": "**`internal`.** 판정 불가를 통과로 바꾸지 않는다. 생성 경로가 항상 넣는 필드이므로 "
                "없으면 저장소가 손상된 것이다",
    },
]

# ---------------------------------------------------------------------------
# 표 밖 행. 위 표 field-missing 행의 원칙(판정 불가를 통과로 바꾸지 않는다)을 형에 넓힌 것이다.
# 저장소 숫자는 store 가 int 로 바꿔 넘기고, 형이 다르면 internal 이다 (디렉터 결정).
# None 검사만 하는 구현은 field-missing 행을 통과하므로 이 행들이 필요하다.
# ---------------------------------------------------------------------------
EXPIRY_UNREADABLE = [
    {"id": "float", "room": {"expires_at_ms": float(NOW + 60_000)},
     "note": "float 는 정수가 아니다. 1.7e12 처럼 값이 맞아 보여도 판정하지 않는다"},
    {"id": "string", "room": {"expires_at_ms": str(NOW + 60_000)}, "note": "문자열은 정수가 아니다"},
    {"id": "bool", "room": {"expires_at_ms": True},
     "note": "bool 은 Python 에서 int 의 하위형이지만 정수 시각이 아니다"},
]


# ---------------------------------------------------------------------------
# 출처: control_plane.md 5.1 방, expires_in_s 문단.
#   expires_in_s 는 max(0, ceil((expires_at_ms - now) / 1000)) 이다. 클라이언트는 이 값을 표시에만
#   쓰고 판정에 쓰지 않는다.
#   - 살아 있는 방에서는 1 이상이다. 1ms 라도 남았으면 올림이 1 을 만든다
#   - 0 은 만료를 뜻한다. 위 경계 행과 같은 말이다
# 문서에 표는 없다. 아래는 위 공식과 두 불릿을 값으로 펼친 것이다. left_ms 는 expires_at_ms - now.
# ---------------------------------------------------------------------------
EXPIRES_IN = [
    {"id": "left-1ms", "left_ms": 1, "expect": 1,
     "note": "살아 있는 방에서는 1 이상이다. 1ms 라도 남았으면 올림이 1 을 만든다"},
    {"id": "left-1000ms", "left_ms": 1_000, "expect": 1, "note": "공식. 나누어떨어지면 올림이 값을 바꾸지 않는다"},
    {"id": "left-1001ms", "left_ms": 1_001, "expect": 2, "note": "공식. 올림"},
    {"id": "left-lease", "left_ms": ROOM_LEASE_S * 1000, "expect": ROOM_LEASE_S,
     "note": "4.6 응답의 expires_in_s. 갱신 뒤 남은 임대이고 정상이면 ROOM_LEASE_S 다"},
    {"id": "left-0", "left_ms": 0, "expect": 0, "note": "0 은 만료를 뜻한다. 위 경계 행과 같은 말이다"},
    {"id": "left-minus-1ms", "left_ms": -1, "expect": 0, "note": "공식의 max(0, ...). 지난 방은 0"},
    {"id": "left-minus-1500ms", "left_ms": -1_500, "expect": 0, "note": "공식의 max(0, ...). 음수를 내지 않는다"},
    {"id": "left-huge", "left_ms": 10**18 + 1, "expect": 10**15 + 1,
     "note": "공식의 ceil 은 수학의 올림이다. 부동소수점 나눗셈은 이 값에서 10**15 를 낸다. "
             "5.1 공식을 정수로 계산하는지 보는 행이다"},
]


@pytest.mark.parametrize("case", STATES, ids=[c["id"] for c in STATES])
def test_state(case):
    assert room_state(case["room"], NOW) == case["expect"]


@pytest.mark.parametrize("case", EXPIRY, ids=[c["id"] for c in EXPIRY])
def test_expiry(case):
    if case["expect"] == "internal":
        with pytest.raises(OpError) as err:
            room_state(case["room"], NOW)
        assert err.value.code == "internal"
        return
    state = room_state(case["room"], NOW)
    assert state == case["expect"]
    # "expires_in_s 가 0 인 방은 참가할 수 없다", "0 은 만료를 뜻한다": 두 판정이 같은 말이어야 한다.
    left = expires_in_s(case["room"]["expires_at_ms"], NOW)
    assert (left == 0) == (state == ROOM_EXPIRED)


@pytest.mark.parametrize("case", EXPIRES_IN, ids=[c["id"] for c in EXPIRES_IN])
def test_expires_in_s(case):
    got = expires_in_s(NOW + case["left_ms"], NOW)
    assert type(got) is int
    assert got == case["expect"]


@pytest.mark.parametrize("case", EXPIRY_UNREADABLE, ids=[c["id"] for c in EXPIRY_UNREADABLE])
def test_expiry_unreadable(case):
    with pytest.raises(OpError) as err:
        room_state(case["room"], NOW)
    assert err.value.code == "internal"
