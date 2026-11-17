"""server.py 의 변이. 입력 상한과 로그 (architecture.md 3.4)."""

P = "telemetry/server.py"
T = "tests/test_server.py::"


def G(row):
    return T + f"test_global_rate_table[{row}]"


MUTANTS = [
    dict(id="bucket-10", path=P, old="RATE_LIMIT_BUCKET = 30", new="RATE_LIMIT_BUCKET = 10",
         kills=[T + "test_source_budget_counts_every_request_and_refills"], why="출발지 토큰 30"),
    dict(id="refill-10", path=P, old="RATE_LIMIT_REFILL_PER_MIN = 30", new="RATE_LIMIT_REFILL_PER_MIN = 10",
         kills=[T + "test_source_budget_counts_every_request_and_refills"], why="분당 30, 2초에 1개"),
    dict(id="spend-only-errors", path=P,
         old="        self.spend(src)\n        return False", new="        return False",
         kills=[T + "test_source_budget_counts_every_request_and_refills"], why="요청마다 하나 쓴다. 성공도 센다"),
    dict(id="rate-not-installed", path=P,
         old="    server.rate = PerRequestRate(", new="    server.rate_unused = PerRequestRate(",
         kills=[T + "test_source_budget_counts_every_request_and_refills"], why="출발지 표가 텔레메트리 값이어야 한다"),
    dict(id="global-off", path=P,
         old="        if not rate.take():\n", new="        if False:\n",
         kills=[T + "test_global_rate_unavailable"], why="전체 초당 상한. 넘으면 unavailable"),
    dict(id="global-10", path=P, old="GLOBAL_RATE_PER_S = 5", new="GLOBAL_RATE_PER_S = 10",
         kills=[T + "test_global_rate_unavailable", G("burst-5")], why="전체 상한 5"),
    dict(id="global-no-cap", path=P,
         old="            self._tokens = min(self._per_s, self._tokens + (now - self._at) * self._per_s)",
         new="            self._tokens = self._tokens + (now - self._at) * self._per_s",
         kills=[G("cap-5")], why="버킷 용량은 5"),
    dict(id="global-batch-refill", path=P,
         old="self._tokens + (now - self._at) * self._per_s)", new="self._tokens + int(now - self._at) * self._per_s)",
         kills=[G("refill-continuous")], why="연속 보충"),
    dict(id="global-reject-moves-anchor", path=P,
         old="            if self._tokens < 1:\n                self.rejected += 1\n                return False",
         new="            if self._tokens < 1:\n                self.rejected += 1\n                self._tokens = 0\n                return False",
         kills=[G("rejected-does-not-delay")], why="거절이 보충을 늦추지 않는다"),
    dict(id="global-not-counted", path=P,
         old='"unavailable": global_rate.rejected', new='"unavailable": 0',
         kills=[T + "test_global_rate_unavailable"], why="전체 상한으로 거절한 것도 unavailable 카운터다"),
    dict(id="log-src", path=P, old="log_src=False", new="log_src=True",
         kills=[T + "test_log_has_no_source_ip_and_counters"], why="로그에 출발지 IP 를 싣지 않는다"),
    dict(id="inflight-default", path=P, old="MAX_INFLIGHT = 32", new="MAX_INFLIGHT = 0",
         kills=[T + "test_size_and_path[body-4096]"], why="동시 연결 32"),
    dict(id="paths-cp", path=P, old='PATHS = paths_for(("report",))', new='PATHS = paths_for(("report", "create_room"))',
         kills=[T + "test_size_and_path[wrong-op]"], why="경로는 /v1/report 하나"),
]
