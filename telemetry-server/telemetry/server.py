"""수집 엔드포인트와 입력 상한. 출처는 architecture.md 3.4 텔레메트리 서비스의 입력 상한, 설정과 배포다.

HTTP 파싱, 응답, 시간 제한, MAX_INFLIGHT, 출발지 표는 제어 서버의 controlplane.server 를 그대로 쓴다
(control_plane.md 3.3, 3.4, 6.4). 여기 있는 것은 텔레메트리의 값과 그 값이 다른 두 규칙이다.

- 출발지 예산은 요청마다 하나 쓴다. 성공도 센다. 제어 서버처럼 파싱 뒤, 저장소 전에 본다
- 서버 전체 초당 요청 상한. 출발지 예산을 지난 요청만 센다. 넘으면 unavailable
"""

from __future__ import annotations

import threading
import time
from collections.abc import Callable, Mapping

from controlplane import log
from controlplane.errors import OpError
from controlplane.server import Request, RateTable, Server, paths_for

TM_PORT = 8001
RATE_LIMIT_BUCKET = 30
RATE_LIMIT_REFILL_PER_MIN = 30
GLOBAL_RATE_PER_S = 5
MAX_INFLIGHT = 32

PATHS = paths_for(("report",))

# 로그 줄. 모양은 제어 서버와 같고 http.request 에 출발지 IP(src)가 없다 (3.4 설정과 배포).
EVENTS: dict[str, tuple[str, ...]] = {
    "http.request": ("op", "status", "error", "ms"),
    "server.started": ("bind", "port"),
    "counter": ("name", "value"),
}
COUNTER_NAMES = ("http_read_timeout", "internal_error", "rate_limited", "rate_table_full", "unavailable",
                 "metrics_put_error")


class PerRequestRate(RateTable):
    """출발지 예산을 요청마다 하나 쓴다. 남았으면 쓰고 False, 다 썼으면 True."""

    def exhausted(self, src: str) -> bool:
        if super().exhausted(src):
            return True
        self.spend(src)
        return False


class GlobalRate:
    """서버 전체 초당 요청. 용량 GLOBAL_RATE_PER_S, 초당 같은 수만큼 연속으로 찬다. dispatch 스레드에서 불린다."""

    def __init__(self, per_s: int = GLOBAL_RATE_PER_S, clock: Callable[[], float] = time.monotonic) -> None:
        self._per_s = per_s
        self._clock = clock
        self._tokens = float(per_s)
        self._at = clock()
        self._lock = threading.Lock()
        self.rejected = 0

    def take(self) -> bool:
        with self._lock:
            now = self._clock()
            self._tokens = min(self._per_s, self._tokens + (now - self._at) * self._per_s)
            self._at = now
            if self._tokens < 1:
                self.rejected += 1
                return False
            self._tokens -= 1
            return True


def limited(dispatch: Callable[[Request], dict], rate: GlobalRate) -> Callable[[Request], dict]:
    """전체 상한을 넘은 요청은 dispatch 하지 않고 unavailable 이다."""
    def run(req: Request) -> dict:
        if not rate.take():
            raise OpError("unavailable")
        return dispatch(req)
    return run


def build_server(dispatch: Callable[[Request], dict], service_counters: Callable[[], Mapping[str, int]], *,
                 global_rate: GlobalRate | None = None, rate_clock: Callable[[], float] = time.monotonic,
                 **server_kwargs) -> Server:
    """제어 서버의 수락 루프에 텔레메트리의 경로, 로그, 카운터, 상한을 건다. server_kwargs 는 시험이 시간 값을 줄인다."""
    global_rate = global_rate or GlobalRate()

    def counters() -> dict[str, int]:
        return {**service_counters(), "unavailable": global_rate.rejected}

    server = Server(limited(dispatch, global_rate), service_counters=counters,
                    paths=PATHS, emit=log.emitter(EVENTS), log_src=False, counter_names=COUNTER_NAMES,
                    max_inflight=server_kwargs.pop("max_inflight", MAX_INFLIGHT), **server_kwargs)
    server.rate = PerRequestRate(rate_clock, server.counters, bucket=RATE_LIMIT_BUCKET,
                                 refill_per_min=RATE_LIMIT_REFILL_PER_MIN)
    return server
