"""CloudWatch 지표의 케이스 표 (architecture.md 3.4 CloudWatch 지표). CloudWatch 는 가짜 클라이언트다."""

import pytest

from telemetry import metrics
from test_ingest import BASE, record


def rec(base, role="player", room_known=True, **over):
    r = record(base, role=role, **over)
    r["room_known"] = room_known
    return r


def names(r):
    return sorted(n for n, _, _ in metrics.samples(r))


# (id, 레코드, 나와야 하는 지표 이름)
SAMPLE_CASES = [
    ("player-ok-known", rec("establish"), ["Attempts", "SetupTime"]),
    ("player-ok-unknown", rec("establish", room_known=False), ["Attempts"]),
    ("player-ok-null", rec("establish", room_known=None), ["Attempts"]),
    ("player-ok-setup-null", rec("establish", setup_ms=None), ["Attempts"]),
    ("host-ok", rec("establish", role="host"), []),
    ("player-aborted", rec("establish", result="ABORTED", setup_ms=None), []),
    ("player-stun", rec("establish", result="STUN_DISCOVERY_FAILED", room_known=None), ["Attempts", "StunFailures"]),
    ("player-punch", rec("establish", result="HOLE_PUNCH_TIMEOUT", room_known=False), ["Attempts", "PunchFailures"]),
    ("player-handshake", rec("establish", result="PEER_HANDSHAKE_FAILED"), ["Attempts", "PunchFailures"]),
    ("player-cp-failed", rec("establish", result="CONTROL_PLANE_EXCHANGE_FAILED"), ["Attempts"]),
    ("player-peer-failed", rec("establish", result="PEER_FAILED"), ["Attempts"]),
    ("host-stun", rec("establish", role="host", result="STUN_DISCOVERY_FAILED"), []),
    ("interval-known", rec("interval"), ["Jitter", "Rtt", "Rtt", "TunnelBytes"]),
    ("interval-host-known", rec("interval", role="host"), ["Jitter", "Rtt", "Rtt", "TunnelBytes"]),
    ("interval-unknown", rec("interval", room_known=False), []),
    ("interval-null", rec("interval", room_known=None), []),
    ("interval-null-jitter", rec("interval", jitter_us=None, rtt_us=[]), ["TunnelBytes"]),
    ("end-player", rec("end"), ["Jitter", "LossRate", "Rtt", "Rtt", "SessionDuration", "TunnelBytes"]),
    ("end-player-dropped", rec("end", result="TUNNEL_DROPPED"),
     ["Jitter", "LossRate", "Rtt", "Rtt", "SessionDuration", "TunnelBytes", "TunnelDropped"]),
    ("end-host", rec("end", role="host", result="TUNNEL_DROPPED"), ["Jitter", "LossRate", "Rtt", "Rtt", "TunnelBytes"]),
    ("end-zero-expected", rec("end", total_expected=0, total_accepted=0),
     ["Jitter", "Rtt", "Rtt", "SessionDuration", "TunnelBytes"]),
    ("end-unknown", rec("end", room_known=False, result="TUNNEL_DROPPED"), []),
]


@pytest.mark.parametrize("r,expected", [c[1:] for c in SAMPLE_CASES], ids=[c[0] for c in SAMPLE_CASES])
def test_samples(r, expected):
    assert names(r) == expected


def test_values_and_units():
    got = {n: (v, w) for n, v, w in metrics.samples(rec("end", rtt_us=[1500], jitter_us=250, total_expected=200,
                                                         total_accepted=150, duration_ms=2500, bytes_out=77))}
    assert got["Rtt"] == (1.5, 1)
    assert got["Jitter"] == (0.25, 1)
    assert got["LossRate"] == (25.0, 200)
    assert got["SessionDuration"] == (2.5, 1)
    assert got["TunnelBytes"] == (77, 1)
    s = {n: (v, w) for n, v, w in metrics.samples(rec("establish", setup_ms=2410))}
    assert s["SetupTime"] == (2410, 1)
    assert set(metrics.UNITS) == {"Attempts", "SetupTime", "StunFailures", "PunchFailures", "Rtt", "Jitter",
                                  "LossRate", "TunnelBytes", "SessionDuration", "TunnelDropped"}


def test_loss_rate_weighted_mean_is_total_lost_over_total_expected():
    m = metrics.Metrics(None)
    m.add(rec("end", total_expected=100, total_accepted=90))   # 10%
    m.add(rec("end", total_expected=900, total_accepted=900))  # 0%
    (req,) = metrics.requests(m.take())
    d = next(x for x in req["MetricData"] if x["MetricName"] == "LossRate")
    mean = sum(v * c for v, c in zip(d["Values"], d["Counts"])) / sum(d["Counts"])
    assert mean == pytest.approx(10 / 1000 * 100)


class FakeCw:
    def __init__(self, fail_at=()):
        self.calls, self.fail_at = [], set(fail_at)

    def put_metric_data(self, **kw):
        self.calls.append(kw)
        if len(self.calls) - 1 in self.fail_at:
            raise RuntimeError("down")


def test_batch_values_and_counts():
    cw = FakeCw()
    m = metrics.Metrics(cw)
    m.add(rec("interval", rtt_us=[2000, 1000, 2000], jitter_us=None, bytes_out=5))
    m.add(rec("establish"))
    m.add(rec("establish", room_known=False))
    m.flush()
    (call,) = cw.calls
    assert call["Namespace"] == "Sangtachi"
    data = {d["MetricName"]: d for d in call["MetricData"]}
    assert "Dimensions" not in data["Rtt"]
    assert data["Rtt"]["Unit"] == "Milliseconds"
    assert data["Rtt"]["Values"] == [1.0, 2.0] and data["Rtt"]["Counts"] == [1.0, 2.0]
    assert data["Attempts"]["Values"] == [1.0] and data["Attempts"]["Counts"] == [2.0]
    assert data["SetupTime"]["Counts"] == [1.0]
    m.flush()
    assert len(cw.calls) == 1  # 보낸 것은 비웠다. 빈 묶음은 보내지 않는다


@pytest.mark.parametrize("n,requests_", [(150, 1), (151, 2), (300, 2), (301, 3)])
def test_split_over_150(n, requests_):
    m = metrics.Metrics(None)
    m.add(rec("establish"))
    for i in range(n):
        m.add(rec("interval", rtt_us=[1000 + i], jitter_us=None))
    reqs = metrics.requests(m.take())
    assert len(reqs) == requests_
    rtt = [d for r in reqs for d in r["MetricData"] if d["MetricName"] == "Rtt"]
    assert all(len(d["Values"]) <= 150 and len(d["Values"]) == len(d["Counts"]) for d in rtt)
    assert sum(len(d["Values"]) for d in rtt) == n
    assert [d["MetricName"] for d in reqs[0]["MetricData"]].count("Attempts") == 1
    assert all(len({d["MetricName"] for d in r["MetricData"]}) == len(r["MetricData"]) for r in reqs)


def test_failed_request_dropped_and_counted():
    cw = FakeCw(fail_at={0})
    m = metrics.Metrics(cw)
    for i in range(151):
        m.add(rec("interval", rtt_us=[1000 + i], jitter_us=None))
    m.flush()
    assert len(cw.calls) == 2
    assert m.counters() == {"metrics_put_error": 1}
    m.flush()
    assert len(cw.calls) == 2  # 다시 보내지 않는다
