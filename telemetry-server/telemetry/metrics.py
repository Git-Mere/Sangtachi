"""CloudWatch 지표의 모음과 전송. 출처는 architecture.md 3.4 텔레메트리 서비스의 CloudWatch 지표다.

add 는 저장에 성공한 레코드 하나를 받아 지표 값으로 바꿔 모은다. flush 는 모은 것을 값과 횟수의 배열로
묶어 PutMetricData 로 보낸다. 실패한 요청의 묶음은 버리고 metrics_put_error 를 올린다. 다시 보내지 않는다.
"""

from __future__ import annotations

import threading
from collections import Counter, defaultdict

NAMESPACE = "Sangtachi"
FLUSH_PERIOD_S = 60
MAX_VALUES_PER_DATUM = 150

UNITS = {
    "Attempts": "Count",
    "SetupTime": "Milliseconds",
    "StunFailures": "Count",
    "PunchFailures": "Count",
    "Rtt": "Milliseconds",
    "Jitter": "Milliseconds",
    "LossRate": "Percent",
    "TunnelBytes": "Bytes",
    "SessionDuration": "Seconds",
    "TunnelDropped": "Count",
}


def samples(r: dict) -> list[tuple[str, float, float]]:
    """레코드 하나의 (지표, 값, 가중치) 목록. 플레이어만 세는 지표와 room_known 조건은 3.4 의 표다."""
    out: list[tuple[str, float, float]] = []
    kind, player, known = r["kind"], r["role"] == "player", r["room_known"] is True
    if kind == "establish" and player:
        result = r["result"]
        if result != "ABORTED":
            out.append(("Attempts", 1, 1))
        if result == "OK" and known and r["setup_ms"] is not None:
            out.append(("SetupTime", r["setup_ms"], 1))
        if result == "STUN_DISCOVERY_FAILED":
            out.append(("StunFailures", 1, 1))
        if result in ("HOLE_PUNCH_TIMEOUT", "PEER_HANDSHAKE_FAILED"):
            out.append(("PunchFailures", 1, 1))
    if kind in ("interval", "end") and known:
        out.extend(("Rtt", us / 1000, 1) for us in r["rtt_us"])
        if r["jitter_us"] is not None:
            out.append(("Jitter", r["jitter_us"] / 1000, 1))
        out.append(("TunnelBytes", r["bytes_out"], 1))
    if kind == "end" and known:
        expected = r["total_expected"]
        if expected > 0:
            out.append(("LossRate", (expected - r["total_accepted"]) / expected * 100, expected))
        if player:
            out.append(("SessionDuration", r["duration_ms"] / 1000, 1))
            if r["result"] == "TUNNEL_DROPPED":
                out.append(("TunnelDropped", 1, 1))
    return out


def requests(batch: dict[str, Counter]) -> list[dict]:
    """PutMetricData 의 인자 목록. 지표 하나의 값이 MAX_VALUES_PER_DATUM 을 넘으면 다음 요청으로 나눈다."""
    out: list[dict] = []
    for name in sorted(batch):
        items = sorted(batch[name].items())
        for i in range(0, len(items), MAX_VALUES_PER_DATUM):
            chunk = items[i:i + MAX_VALUES_PER_DATUM]
            datum = {"MetricName": name, "Unit": UNITS[name], "Values": [float(v) for v, _ in chunk],
                     "Counts": [float(c) for _, c in chunk]}
            n = i // MAX_VALUES_PER_DATUM
            if n == len(out):
                out.append({"Namespace": NAMESPACE, "MetricData": []})
            out[n]["MetricData"].append(datum)
    return out


class Metrics:
    def __init__(self, cloudwatch) -> None:
        self.client = cloudwatch
        self._lock = threading.Lock()
        self._batch: dict[str, Counter] = defaultdict(Counter)
        self.put_errors = 0

    def add(self, record: dict) -> None:
        with self._lock:
            for name, value, weight in samples(record):
                self._batch[name][value] += weight

    def take(self) -> dict[str, Counter]:
        with self._lock:
            batch, self._batch = self._batch, defaultdict(Counter)
        return batch

    def flush(self) -> None:
        for req in requests(self.take()):
            try:
                self.client.put_metric_data(**req)
            except Exception:
                self.put_errors += 1  # 이 flush 만 부른다. 한 스레드다

    def counters(self) -> dict[str, int]:
        return {"metrics_put_error": self.put_errors}
