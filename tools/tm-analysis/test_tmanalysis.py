"""tmanalysis 의 순수 계산부. DynamoDB 를 부르지 않는다.

    python -m pytest -q -p no:cacheprovider tools/tm-analysis
"""

import sys
from decimal import Decimal
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tmanalysis as tm  # noqa: E402

DAY = "2026-11-20"


def ms(hhmmss: str, day: str = DAY) -> int:
    return tm.parse_utc(f"{day}T{hhmmss}Z")


def rec(kind="establish", role="player", seq=0, inst="a" * 32, attempt=1, at="10:00:00", **kw):
    r = {"pk": f"{inst}#{attempt}", "seq": seq, "kind": kind, "role": role, "instance_id": inst,
         "attempt": attempt, "received_at_ms": ms(at), "room_known": True, "build_kind": "release"}
    if kind == "establish":
        r.update(result="OK", setup_ms=2000, stun_same_endpoint=True, local_differs_public=True, same_lan=False)
    r.update(kw)
    return r


# ---- Wilson 점수 구간. 알려진 값과 대조한다 ----
# 기대값은 점수 검정의 이차식 (p̂-p)^2 = z^2 p(1-p)/n 의 두 근을 numpy.roots 로 따로 구한 것이다. 구현의 닫힌 식과 다른 길이다.
WILSON = [
    (0, 10, (0.0, 0.27753)),
    (10, 10, (0.72247, 1.0)),
    (5, 10, (0.23659, 0.76341)),
    (8, 10, (0.49016, 0.94332)),
    (1, 1, (0.20655, 1.0)),
    (81, 263, (0.25529, 0.36621)),
]


@pytest.mark.parametrize("k,n,want", WILSON)
def test_wilson(k, n, want):
    lo, hi = tm.wilson(k, n)
    assert lo == pytest.approx(want[0], abs=1e-5)
    assert hi == pytest.approx(want[1], abs=1e-5)


def test_wilson_empty():
    assert tm.wilson(0, 0) is None


# ---- 사분위. 정렬 후 선형 보간(experiments.md 1장). numpy 기본(type 7)과 같은 값 ----
QUARTILES = [
    ([7], (7, 7, 7)),
    ([1, 2], (1.25, 1.5, 1.75)),
    ([4, 1, 3, 2], (1.75, 2.5, 3.25)),
    ([1, 2, 3, 4, 5], (2, 3, 4)),
    ([6, 7, 15, 36, 39, 40, 41, 42, 43, 47, 49], (25.5, 40, 42.5)),
]


@pytest.mark.parametrize("vals,want", QUARTILES)
def test_quartiles(vals, want):
    assert tm.quartiles(vals) == pytest.approx(want)


def test_quartiles_empty():
    assert tm.quartiles([]) is None


# ---- 플레이어 establish 의 분류. CloudWatch 지표 표(architecture.md 3.4)와 같은 거르기 ----
CLASSIFY = [
    ("OK, room_known true", dict(result="OK", room_known=True), "success"),
    ("OK, room_known false", dict(result="OK", room_known=False), "excluded_ok"),
    ("OK, room_known null", dict(result="OK", room_known=None), "excluded_ok"),
    ("수립 전 실패, 방 없음", dict(result="STUN_DISCOVERY_FAILED", room_known=None, room_id=None), "failure"),
    ("실패, room_known false 도 센다", dict(result="HOLE_PUNCH_TIMEOUT", room_known=False), "failure"),
    ("실패, room_known true", dict(result="PEER_HANDSHAKE_FAILED", room_known=True), "failure"),
    ("교환 실패", dict(result="CONTROL_PLANE_EXCHANGE_FAILED", room_known=None), "failure"),
    ("상대 실패 통지는 실패", dict(result="PEER_FAILED", room_known=True), "failure"),
    ("ABORTED 는 분모에서 뺀다", dict(result="ABORTED", room_known=True), "aborted"),
    ("ABORTED, 방 없음", dict(result="ABORTED", room_known=None), "aborted"),
]


@pytest.mark.parametrize("name,over,want", CLASSIFY, ids=[c[0] for c in CLASSIFY])
def test_classify(name, over, want):
    assert tm.classify(rec(**over)) == want


# ---- 누락 수: 시도 안의 빈 seq. 마지막 레코드나 시도 전체를 잃은 것은 보이지 않는다 ----
MISSING = [
    ([0], 0),
    ([0, 1, 2, 3], 0),
    ([0, 2, 3], 1),
    ([0, 3], 2),
    ([1, 2], 1),  # establish(seq 0) 를 잃음
    ([0, 1], 0),  # 그 뒤를 잃었어도 보이지 않는다
]


@pytest.mark.parametrize("seqs,want", MISSING)
def test_missing(seqs, want):
    assert tm.missing_seq([rec(seq=s) for s in seqs]) == want


# ---- 묶음 파일 ----
GOOD = """# 시작 끝 환경 종류
2026-11-20T10:00:00Z 2026-11-20T11:00:00Z home-home real

2026-11-20T11:00:00Z 2026-11-20T11:10:00Z ctl-stun controlled  # 끝 주석
2026-11-21T09:00:00Z 2026-11-21T10:00:00Z home-home real
"""


def test_parse_batches():
    b = tm.parse_batches(GOOD)
    assert [(x.env, x.kind, x.date) for x in b] == [
        ("home-home", "real", "2026-11-20"), ("ctl-stun", "controlled", "2026-11-20"),
        ("home-home", "real", "2026-11-21")]
    assert b[0].start_ms == ms("10:00:00") and b[0].end_ms == ms("11:00:00")


BAD_BATCHES = [
    ("필드 셋", "2026-11-20T10:00:00Z 2026-11-20T11:00:00Z home"),
    ("필드 다섯", "2026-11-20T10:00:00Z 2026-11-20T11:00:00Z home real x"),
    ("종류 오타", "2026-11-20T10:00:00Z 2026-11-20T11:00:00Z home Real"),
    ("Z 없음", "2026-11-20T10:00:00 2026-11-20T11:00:00Z home real"),
    ("시간대 오프셋", "2026-11-20T10:00:00+09:00 2026-11-20T11:00:00Z home real"),
    ("끝이 시작보다 앞", "2026-11-20T11:00:00Z 2026-11-20T10:00:00Z home real"),
    ("길이 0", "2026-11-20T10:00:00Z 2026-11-20T10:00:00Z home real"),
    ("겹침", "2026-11-20T10:00:00Z 2026-11-20T11:00:00Z a real\n2026-11-20T10:59:59Z 2026-11-20T12:00:00Z b real"),
    ("같은 환경의 종류가 다름", "2026-11-20T10:00:00Z 2026-11-20T11:00:00Z a real\n"
                               "2026-11-21T10:00:00Z 2026-11-21T11:00:00Z a controlled"),
    ("빈 파일", "# 주석만\n"),
]


@pytest.mark.parametrize("name,text", BAD_BATCHES, ids=[c[0] for c in BAD_BATCHES])
def test_parse_batches_rejects(name, text):
    with pytest.raises(tm.AnalysisError):
        tm.parse_batches(text)


# ---- 묶음 배정: 시도는 받은 seq 가 가장 작은 레코드의 received_at_ms 로 간다 ----
def test_assign():
    batches = tm.parse_batches(
        "2026-11-20T10:00:00Z 2026-11-20T11:00:00Z e1 real\n"
        "2026-11-20T12:00:00Z 2026-11-20T13:00:00Z e2 real\n")
    records = [
        rec(inst="1" * 32, at="10:00:00"),                    # 시작 경계는 든다
        rec(inst="2" * 32, at="10:59:59"),
        rec(inst="2" * 32, seq=1, kind="end", at="12:30:00"),  # 묶음 밖으로 넘친 세션
        rec(inst="3" * 32, at="11:00:00"),                    # 끝 경계는 들지 않는다
        rec(inst="4" * 32, seq=1, kind="end", at="12:00:00"),  # 늦게 받은 end 가 먼저 와도 seq 로 고른다
        rec(inst="4" * 32, seq=0, at="10:30:00"),
    ]
    got = tm.assign(records, batches)
    assert sorted(a[0]["instance_id"][0] for a in got.attempts["e1"]) == ["1", "2", "4"]
    assert got.attempts.get("e2", []) == []
    assert got.unassigned == 1
    assert got.straddling == {"e1": 2}


def test_assign_without_seq0():
    batches = tm.parse_batches("2026-11-20T10:00:00Z 2026-11-20T11:00:00Z e1 real\n")
    got = tm.assign([rec(seq=2, kind="interval", at="10:10:00"), rec(seq=3, kind="end", at="10:20:00")], batches)
    assert len(got.attempts["e1"]) == 1 and got.straddling == {}


# ---- 요약: 거르기가 숫자에 그대로 반영되는가 ----
def attempt(n, **over):
    return [rec(inst=f"{n:032x}", **over)]


def test_summarize_counts():
    attempts = [
        attempt(1),
        attempt(2, setup_ms=3000),
        attempt(3, room_known=False),
        attempt(4, result="STUN_DISCOVERY_FAILED", setup_ms=None, room_known=None, stun_same_endpoint=None,
                local_differs_public=None, same_lan=None),
        attempt(5, result="HOLE_PUNCH_TIMEOUT", setup_ms=None, room_known=False, stun_same_endpoint=False),
        attempt(6, result="ABORTED", setup_ms=None),
        attempt(7, role="host", setup_ms=900),
        attempt(8, role="host", setup_ms=700, room_known=False),
    ]
    s = tm.summarize(attempts)
    assert (s.attempts, s.successes, s.aborted, s.excluded_ok) == (4, 2, 1, 1)
    assert s.failures == {"STUN_DISCOVERY_FAILED": 1, "HOLE_PUNCH_TIMEOUT": 1}
    assert s.player_setup == [2000, 3000]
    assert s.host_setup == [900]
    assert s.by["stun_same_endpoint"] == {True: (2, 2), False: (1, 0), None: (1, 0)}
    assert s.by["same_lan"] == {False: (3, 2), None: (1, 0)}
    assert s.by["build_kind"] == {"release": (4, 2)}


def test_summarize_session_values():
    inst = "f" * 32
    a = [
        rec(inst=inst),
        rec(inst=inst, seq=1, kind="interval", rtt_us=[Decimal(1000), 3000], jitter_us=500, interval_ms=60000,
            expected=10, accepted=9, bytes_out=1, bytes_in=1, rtt_overflow=0),
        rec(inst=inst, seq=3, kind="end", result="CLOSED", rtt_us=[2000], jitter_us=None, duration_ms=125000,
            total_expected=100, total_accepted=97, interval_ms=1, expected=1, accepted=1, bytes_out=1, bytes_in=1,
            rtt_overflow=0, total_bytes_out=1, total_bytes_in=1, total_tx_err_send=0),
    ]
    host_end = rec(inst="e" * 32, role="host", seq=1, kind="end", result="TUNNEL_DROPPED", rtt_us=[9000],
                   jitter_us=700, duration_ms=1000, total_expected=0, total_accepted=0)
    fake_end = rec(inst="d" * 32, seq=1, kind="end", result="CLOSED", rtt_us=[99000], jitter_us=9,
                   duration_ms=9, total_expected=50, total_accepted=0, room_known=False)
    s = tm.summarize([a, [host_end], [fake_end]])
    assert sorted(s.rtt_ms) == [1.0, 2.0, 3.0, 9.0]
    assert sorted(s.jitter_ms) == [0.5, 0.7]
    assert (s.lost, s.expected) == (3, 100)
    assert s.duration_s == [125.0]          # 플레이어의 end 만
    assert s.end_results == {"CLOSED": 1}   # 플레이어의 end 만
    assert s.missing == 3                    # a 의 seq 2, 두 end 시도의 seq 0. room_known 과 무관하게 센다


# ---- Scan 의 페이지 조절. 가짜 테이블이다 ----
class FakeTable:
    def __init__(self, pages):
        self.pages, self.calls = pages, []

    def scan(self, **kw):
        self.calls.append(kw)
        return self.pages[len(self.calls) - 1]


def test_scan_paces(monkeypatch):
    slept = []
    monkeypatch.setattr(tm.time, "sleep", slept.append)
    monkeypatch.setattr(tm.time, "monotonic", lambda: 100.0)
    t = FakeTable([
        {"Items": [{"seq": Decimal(0), "rtt_us": [Decimal("1500")]}], "LastEvaluatedKey": {"k": 1},
         "ConsumedCapacity": {"CapacityUnits": 5.0}},
        {"Items": [], "LastEvaluatedKey": {"k": 2}, "ConsumedCapacity": {"CapacityUnits": 2.5}},
        {"Items": [{"seq": Decimal(1)}], "ConsumedCapacity": {"CapacityUnits": 0.5}},
    ])
    items = tm.scan(t, 0, 10, max_rcu=5)
    assert items == [{"seq": 0, "rtt_us": [1500]}, {"seq": 1}]
    assert slept == [1.0, 0.5]                     # 마지막 페이지 뒤에는 쉬지 않는다
    assert [c.get("ExclusiveStartKey") for c in t.calls] == [None, {"k": 1}, {"k": 2}]
    assert all(c["Limit"] == 40 and c["ReturnConsumedCapacity"] == "TOTAL" for c in t.calls)


# ---- 출력: 날짜만 내고 시각과 식별자를 내지 않는다 ----
def test_report_hides_time_and_ids():
    batches = tm.parse_batches(
        "2026-11-20T10:00:00Z 2026-11-20T11:00:00Z e1 real\n"
        "2026-11-21T10:00:00Z 2026-11-21T11:00:00Z e2 real\n"
        "2026-11-22T10:00:00Z 2026-11-22T11:00:00Z ctl controlled\n")
    records = [rec(inst="1" * 32, at="10:10:00", room_id="ABCDEF"),
               rec(inst="2" * 32, at="10:10:00", room_id="GHIJKL", result="HOLE_PUNCH_TIMEOUT", setup_ms=None),
               rec(inst="3" * 32, result="STUN_DISCOVERY_FAILED")]
    records[2]["received_at_ms"] = ms("10:10:00", "2026-11-22")
    out = tm.report(records, batches)
    assert "10:" not in out and "1111" not in out and "ABCDEF" not in out
    assert "2026-11-20" in out and "2026-11-22" in out
    assert out.index("## 실제 환경") < out.index("### 실제 환경 합산") < out.index("## 통제된 실패")
    assert "`STUN_DISCOVERY_FAILED` | 1" in out.split("## 통제된 실패")[1]
    assert "STUN_DISCOVERY_FAILED" not in out.split("## 통제된 실패")[0]


@pytest.mark.parametrize("v", ["0", "-1", "nan", "inf"])
def test_main_rejects_max_rcu(tmp_path, v):
    b = tmp_path / "b.txt"
    b.write_text("2026-11-20T10:00:00Z 2026-11-20T11:00:00Z e1 real\n", encoding="utf-8")
    assert tm.main(["--batches", str(b), "--table", "t", "--region", "us-west-2", "--max-rcu", v]) == 2


def test_ok_with_null_setup_counts_as_success_without_setup_value():
    # 서버는 OK 의 setup_ms 가 null 인 것을 거르지 않는다. 표가 깨지지 않고 성공으로만 센다.
    player = rec(setup_ms=None)
    host = rec(role="host", inst="b" * 32, setup_ms=None)
    s = tm.summarize([[player], [host]])
    assert (s.attempts, s.successes) == (1, 1)
    assert s.player_setup == [] and s.host_setup == []
