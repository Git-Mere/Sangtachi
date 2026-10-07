"""tests/harness/cp_harness.py 의 시험. 하네스는 시험 전용이고 배포하지 않는다.

주입 판정(check_rule, Injections.take, parse_command, RateClock)은 케이스 표로 본다. 하네스 프로세스 전체는
store 표시 행이 DynamoDB local 에서 띄워 본다. 표의 출처는 이 파일이다. control_plane.md 에 이 하네스의 표는 없다.
"""

from __future__ import annotations

import json
import os
import re
import secrets
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

from harness import cp_harness as H

HARNESS = Path(H.__file__).resolve()
PY = sys.executable

# ---------------------------------------------------------------- check_rule

RULE_CASES = [
    # (kind, op, count, params, ok, note)
    ("delay", "create_room", 1, {"ms": 1500, "when": "after"}, True, "delay one op after dispatch"),
    ("delay", "*", 3, {"ms": 0, "when": "before"}, True, "any op, zero ms"),
    ("delay", "create_room", 1, {"ms": 60_001, "when": "after"}, False, "ms over the cap"),
    ("delay", "create_room", 1, {"ms": -1, "when": "after"}, False, "negative ms"),
    ("delay", "create_room", 1, {"ms": True, "when": "after"}, False, "bool is not an int"),
    ("delay", "create_room", 1, {"ms": 10, "when": "during"}, False, "unknown when"),
    ("delay", "create_room", 1, {"ms": 10}, False, "missing when"),
    ("delay", "make_room", 1, {"ms": 10, "when": "after"}, False, "unknown op"),
    ("error", "join_room", 1, {"code": "room_full", "when": "before"}, True, "error code from errors.STATUS"),
    ("error", "host_report", 2, {"code": "unauthorized", "when": "before"}, True, "host_report error"),
    ("error", "create_room", 1, {"code": "internal", "when": "after"}, True, "error after a successful dispatch"),
    ("error", "create_room", 1, {"code": "internal"}, False, "error needs when"),
    ("error", "create_room", 1, {"code": "internal", "when": "later"}, False, "unknown when for error"),
    ("error", "join_room", 1, {"code": "too_large", "when": "before"}, False, "too_large has two statuses, not offered"),
    ("error", "join_room", 1, {"code": "teapot", "when": "before"}, False, "unknown code"),
    ("error", "join_room", 1, {"code": "room_full", "when": "before", "ms": 1}, False, "extra parameter"),
    ("error", "join_room", 0, {"code": "room_full", "when": "before"}, False, "count zero"),
    ("error", "join_room", 101, {"code": "room_full", "when": "before"}, False, "count over the cap"),
    ("error", "join_room", "1", {"code": "room_full", "when": "before"}, False, "count as text"),
    ("after_commit", "join_room", 2, {}, True, "store write label"),
    ("after_commit", "register_candidates", 1, {}, True, "label differs from op name"),
    ("after_commit", "register_candidate", 1, {}, False, "op name is not a store label"),
    ("after_commit", "*", 1, {}, False, "after_commit needs one label"),
    ("hold", "*", 1, {}, True, "hold any"),
    ("hold", "create_room", 1, {}, False, "hold cannot pick an op"),
    ("drop", "*", 1, {}, True, "drop any"),
    ("drop", "*", 1, {"ms": 1}, False, "drop takes no parameter"),
    ("sleep", "*", 1, {}, False, "unknown kind"),
]


@pytest.mark.parametrize("kind,op,count,params,ok,note", RULE_CASES, ids=[c[-1] for c in RULE_CASES])
def test_check_rule(kind, op, count, params, ok, note):
    if ok:
        H.check_rule(kind, op, count, params)
    else:
        with pytest.raises(ValueError):
            H.check_rule(kind, op, count, params)


# ---------------------------------------------------------------- Injections.take

def test_take_matches_kind_and_op_and_counts_down():
    inj = H.Injections()
    inj.add("error", "join_room", 2, code="room_full", when="before")
    assert inj.take("error", "create_room") is None          # 다른 연산
    assert inj.take("delay", "join_room") is None            # 다른 종류
    assert inj.take("error", "join_room")["code"] == "room_full"
    assert inj.take("error", "join_room")["code"] == "room_full"
    assert inj.take("error", "join_room") is None            # 두 번으로 끝났다
    assert inj.pending() == []
    assert inj.fired["error"] == 2


def test_take_star_matches_every_op_and_rules_are_first_in_first_out():
    inj = H.Injections()
    inj.add("error", "*", 1, code="internal", when="before")
    inj.add("error", "create_room", 1, code="unavailable", when="before")
    assert inj.take("error", "create_room")["code"] == "internal"
    assert inj.take("error", "create_room")["code"] == "unavailable"
    assert inj.take("error", "get_peers") is None


def test_take_skips_a_rule_for_another_op_to_reach_a_later_match():
    inj = H.Injections()
    inj.add("error", "host_report", 1, code="unauthorized", when="before")
    inj.add("error", "join_room", 1, code="room_full", when="before")
    assert inj.take("error", "join_room")["code"] == "room_full"
    assert [r["op"] for r in inj.pending()] == ["host_report"]


def test_take_match_filters_on_rule_parameters():
    inj = H.Injections()
    inj.add("error", "create_room", 1, code="internal", when="after")
    assert inj.take("error", "create_room", when="before") is None
    assert inj.take("error", "create_room", when="after")["code"] == "internal"


def test_clear_drops_rules_offset_and_tally():
    inj = H.Injections()
    inj.add("hold", "*", 3)
    inj.take("hold", "*")
    inj.offset_ms = 5
    inj.clear()
    assert inj.pending() == [] and inj.offset_ms == 0 and not inj.fired


def test_add_rejects_a_bad_rule_and_keeps_the_queue():
    inj = H.Injections()
    with pytest.raises(ValueError):
        inj.add("delay", "create_room", 1, ms=10, when="sometime")
    assert inj.pending() == []


# ---------------------------------------------------------------- parse_command

PARSE_CASES = [
    ("ping", ("ping", {})),
    ("delay op=create_room count=1 ms=1500 when=after", ("delay", {"op": "create_room", "count": 1, "ms": 1500,
                                                                 "when": "after"})),
    ("error op=* count=2 code=internal when=after", ("error", {"op": "*", "count": 2, "code": "internal",
                                                              "when": "after"})),
    ("error op=* count=2 code=internal", ValueError),          # when 은 필수
    ("  clock   offset_ms=-200000  ", ("clock", {"offset_ms": -200000})),
    ("room id=abcdef", ("room", {"id": "abcdef"})),
    ("", ValueError),
    ("DELAY op=x count=1 ms=1 when=after", ValueError),       # 대소문자를 가린다
    ("delay op=create_room count=1 ms=1500", ValueError),     # 빠진 인자
    ("ping extra=1", ValueError),                             # 남는 인자
    ("delay op=a count=1 count=2 ms=1 when=after", ValueError),  # 같은 인자 두 번
    ("delay op=create_room count=one ms=1 when=after", ValueError),
    ("delay op=create_room count=1.5 ms=1 when=after", ValueError),
    ("room id=", ValueError),
    ("room id=a\u00e9", ValueError),                          # ASCII 밖
    ("room id=a/b", ValueError),
    ("burn n=1234567890123456", ValueError),                  # 16자리 정수
    ("ping=1", ValueError),
]


@pytest.mark.parametrize("line,expect", PARSE_CASES, ids=[repr(c[0]) for c in PARSE_CASES])
def test_parse_command(line, expect):
    if expect is ValueError:
        with pytest.raises(ValueError):
            H.parse_command(line)
    else:
        assert H.parse_command(line) == expect


def test_every_command_has_a_handler_branch():
    source = HARNESS.read_text(encoding="utf-8")
    for verb in H.COMMANDS:
        assert f'"{verb}"' in source.split("async def handle_admin", 1)[1].split("def reset_rate", 1)[0], verb


# ---------------------------------------------------------------- RateClock

class FakeTime:
    def __init__(self) -> None:
        self.t = 100.0

    def __call__(self) -> float:
        return self.t


def test_rate_clock_freeze_thaw_advance():
    base = FakeTime()
    rc = H.RateClock(base)
    assert rc() == 100.0
    rc.freeze()
    base.t = 130.0
    assert rc() == 100.0 and rc.frozen          # 멈춰 있다
    rc.advance(6)
    assert rc() == 106.0                        # 멈춘 채로도 앞으로 민다
    rc.thaw()
    assert rc() == 136.0 and not rc.frozen      # 실제 시계 + 민 만큼. 멈춘 동안이 한꺼번에 보충된다
    rc.advance(60)
    assert rc() == 196.0


def test_rate_clock_never_goes_backwards_on_thaw():
    base = FakeTime()
    rc = H.RateClock(base)
    rc.freeze()
    rc.advance(50)          # 멈춘 값 150
    base.t = 120.0          # 실제 시계는 그보다 뒤
    rc.thaw()
    assert rc() >= 150.0


@pytest.mark.parametrize("seconds", [-1, 86_401])
def test_rate_clock_rejects_out_of_range_advance(seconds):
    with pytest.raises(ValueError):
        H.RateClock(FakeTime()).advance(seconds)


def test_frozen_rate_clock_keeps_the_bucket_empty():
    """RateTable 에 끼웠을 때 멈춘 시계는 보충하지 않는다. 풀면 보충한다 (control_plane.md 6.4)."""
    from controlplane.constants import RATE_LIMIT_BUCKET
    from controlplane.server import RateTable

    base = FakeTime()
    rc = H.RateClock(base)
    table = RateTable(clock=rc)
    rc.freeze()
    for _ in range(RATE_LIMIT_BUCKET):
        table.spend("127.0.0.1")
    base.t += 600
    assert table.exhausted("127.0.0.1")
    rc.thaw()
    assert not table.exhausted("127.0.0.1")


# ---------------------------------------------------------------- dispatch 와 쓰기 주입점

class FakeService:
    def __init__(self) -> None:
        self.calls = []

    def dispatch(self, req):
        self.calls.append(req.op)
        return {"room_id": "SECRET", "peer_id": 7, "peer_token": "t" * 32, "virtual_ip": "10.100.0.2",
                "expires_in_s": 120}


def _harness(lines):
    inj = H.Injections()
    svc = FakeService()
    h = H.Harness(None, None, svc, None, inj, H.RateClock())
    return h, inj, svc


@pytest.fixture
def captured(monkeypatch):
    lines = []
    monkeypatch.setattr(H, "_write_line", lines.append)
    return lines


def test_error_injection_answers_without_calling_the_service(captured):
    from controlplane.errors import OpError
    from controlplane.server import Request

    h, inj, svc = _harness(captured)
    inj.add("error", "join_room", 1, code="room_full", when="before")
    with pytest.raises(OpError) as exc:
        h.dispatch(Request("join_room", {}))
    assert exc.value.code == "room_full" and exc.value.status == 409
    assert svc.calls == []
    h.dispatch(Request("join_room", {}))       # 한 번으로 끝났다
    assert svc.calls == ["join_room"]


def test_error_after_runs_the_service_then_fails_the_answer(captured):
    from controlplane.errors import OpError
    from controlplane.server import Request

    h, inj, svc = _harness(captured)
    inj.add("error", "create_room", 1, code="internal", when="after")
    with pytest.raises(OpError) as exc:
        h.dispatch(Request("create_room", {}))
    assert exc.value.code == "internal" and svc.calls == ["create_room"]
    h.dispatch(Request("create_room", {}))     # 한 번으로 끝났다


def test_error_after_is_not_spent_by_a_failing_dispatch(captured):
    """after 는 성공한 dispatch 에만 걸린다. 앞 시도가 다른 이유로 실패하면 그 규칙은 남는다."""
    from controlplane.errors import OpError
    from controlplane.server import Request

    h, inj, svc = _harness(captured)
    inj.add("error", "create_room", 1, code="internal", when="before")
    inj.add("error", "create_room", 1, code="unavailable", when="after")
    with pytest.raises(OpError) as exc:
        h.dispatch(Request("create_room", {}))
    assert exc.value.code == "internal"
    with pytest.raises(OpError) as exc:
        h.dispatch(Request("create_room", {}))
    assert exc.value.code == "unavailable" and svc.calls == ["create_room"]


@pytest.mark.parametrize("when", ["before", "after"])
def test_delay_injection_holds_the_answer(captured, when):
    from controlplane.server import Request

    h, inj, svc = _harness(captured)
    inj.add("delay", "create_room", 1, ms=300, when=when)
    t = time.monotonic()
    h.dispatch(Request("create_room", {}))
    assert time.monotonic() - t >= 0.29
    t = time.monotonic()
    h.dispatch(Request("create_room", {}))
    assert time.monotonic() - t < 0.2


def test_delay_after_also_delays_an_error(captured):
    from controlplane.errors import OpError
    from controlplane.server import Request

    h, inj, svc = _harness(captured)
    inj.add("delay", "*", 1, ms=300, when="after")
    inj.add("error", "*", 1, code="internal", when="before")
    t = time.monotonic()
    with pytest.raises(OpError):
        h.dispatch(Request("get_peers", {}))
    assert time.monotonic() - t >= 0.29


def test_issued_line_carries_no_room_id_or_token(captured):
    from controlplane.server import Request

    h, inj, svc = _harness(captured)
    h.dispatch(Request("join_room", {}))
    assert captured == ["HARNESS issued op=join_room peer_id=7 virtual_ip=10.100.0.2"]
    assert "SECRET" not in "".join(captured) and "t" * 32 not in "".join(captured)


def test_after_commit_fires_only_after_a_matching_write(captured):
    from controlplane.errors import OpError

    h, inj, svc = _harness(captured)
    inj.add("after_commit", "join_room", 1)
    h.on_write("before", "join_room", "VIP#10.100.0.2")      # 쓰기 전에는 던지지 않는다
    h.on_write("after", "create_room", "ROOM")              # 다른 쓰기
    with pytest.raises(OpError) as exc:
        h.on_write("after", "join_room", "VIP#10.100.0.2")
    assert exc.value.code == "internal"
    h.on_write("after", "join_room", "VIP#10.100.0.3")       # 한 번으로 끝났다


# ---------------------------------------------------------------- STUN 응답과 로그 줄

def test_stun_response_maps_the_source():
    req = b"\x00\x01\x00\x00" + H.MAGIC + bytes(range(12))
    out = H.stun_response(req, ("127.0.0.1", 50000))
    assert out[0:2] == b"\x01\x01" and out[2:4] == b"\x00\x0c" and out[8:20] == bytes(range(12))
    port = int.from_bytes(out[26:28], "big") ^ 0x2112
    addr = ".".join(str(a ^ b) for a, b in zip(out[28:32], H.MAGIC))
    assert (addr, port) == ("127.0.0.1", 50000)


def test_stun_response_reports_the_mapped_ip_with_the_source_port():
    req = b"\x00\x01\x00\x00" + H.MAGIC + bytes(12)
    out = H.stun_response(req, ("127.0.0.1", 50000), "192.0.2.1")
    port = int.from_bytes(out[26:28], "big") ^ 0x2112
    addr = ".".join(str(a ^ b) for a, b in zip(out[28:32], H.MAGIC))
    assert (addr, port) == ("192.0.2.1", 50000)


@pytest.mark.parametrize("ip,ok", [
    ("192.0.2.1", True), ("198.51.100.254", True), ("203.0.113.7", True), ("127.0.0.1", True),
    ("192.0.2.0", False), ("192.0.2.255", False), ("192.0.2.01", False), ("192.0.3.1", False),
    ("8.8.8.8", False), ("127.0.0.2", False), ("0.0.0.0", False), ("192.0.2.1 ", False), ("", False),
])
def test_check_mapped_ip_is_an_allow_list(ip, ok):
    if ok:
        assert H.check_mapped_ip(ip) == ip
    else:
        with pytest.raises(ValueError):
            H.check_mapped_ip(ip)


def test_the_default_mapped_ip_passes_server_candidate_sanitation():
    """루프백이 아니어야 register_candidate 가 받는다 (control_plane.md 4.4). 위생 함수로 직접 본다."""
    from controlplane.candidates import sanitize_candidates
    from controlplane.errors import OpError

    stored, rejected = sanitize_candidates([{"ip": H.DEFAULT_MAPPED_IP, "port": 50000, "kind": "reflexive"}])
    assert len(stored) == 1 and rejected == 0
    with pytest.raises(OpError):
        sanitize_candidates([{"ip": "127.0.0.1", "port": 50000, "kind": "reflexive"}])


@pytest.mark.parametrize("req", [
    b"\x00\x01\x00\x00" + H.MAGIC + bytes(11),                 # 19 바이트
    b"\x01\x01\x00\x00" + H.MAGIC + bytes(12),                 # 응답이다
    b"\x00\x01\x00\x00" + b"\x00\x00\x00\x00" + bytes(12),     # 매직 쿠키가 아니다
])
def test_stun_response_ignores_non_requests(req):
    assert H.stun_response(req, ("127.0.0.1", 1)) is None


def test_hline_keeps_values_single_token_ascii():
    assert H.hline("x", a="b c", d="", e="\u00e9") == "HARNESS x a=b_c d=- e=_"


@pytest.mark.parametrize("url,ok", [
    ("http://127.0.0.1:8001", True), ("http://localhost:8001/", True), ("http://[::1]:8001", True),
    ("http://10.0.0.1:8001", False), ("https://127.0.0.1:8001", False), ("http://127.0.0.1", False),
    ("http://127.0.0.1:0", False), ("http://127.0.0.1:8001/x", False), ("http://u@127.0.0.1:8001", False),
    ("http://127.0.0.1:8001?q", False), ("http://127.0.0.1:8001\t", False), ("http://dynamodb.us-west-2.amazonaws.com:80", False),
])
def test_loopback_endpoint(url, ok):
    if ok:
        H.loopback_endpoint(url)
    else:
        with pytest.raises(ValueError):
            H.loopback_endpoint(url)


# ---------------------------------------------------------------- 프로세스 전체

def _free_closed_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def test_unreachable_ddb_fails_loudly_with_the_docker_command():
    """닿지 않으면 건너뛰지 않는다. 종료 코드 3, 띄우는 명령을 낸다."""
    port = _free_closed_port()
    proc = subprocess.run([PY, str(HARNESS), "--ddb", f"http://127.0.0.1:{port}"], capture_output=True,
                          timeout=120, text=True, encoding="ascii", errors="replace")
    assert proc.returncode == H.EXIT_NO_DDB, proc.stderr
    assert "HARNESS error reason=ddb_unreachable" in proc.stderr
    assert H.DOCKER_CMD in proc.stderr


def test_a_non_loopback_ddb_is_refused_before_any_call():
    proc = subprocess.run([PY, str(HARNESS), "--ddb", "http://192.0.2.1:8001"], capture_output=True, timeout=120,
                          text=True, encoding="ascii", errors="replace")
    assert proc.returncode == H.EXIT_USAGE and "not a loopback host" in proc.stderr


class Running:
    """띄운 하네스 하나. 표준 오류를 파일로 받는다."""

    def __init__(self, tmp_path, endpoint, *extra, ready_s=60):
        """프로세스 생성부터 준비 줄 파싱까지 전부 한 가드 안이다. 어디서 실패해도 close 로 정리한 뒤 다시 올린다.
        픽스처는 생성자가 돌아온 뒤에야 정리를 건다."""
        self.err_path = tmp_path / "harness.err"
        self.err = open(self.err_path, "wb")
        self.admin_port = None
        self.proc = None
        try:
            # --exit-on-stdin-eof: 표준 입력을 닫으면 하네스가 스스로 테이블을 지우고 끝난다. close 의 둘째 수단이다.
            self.proc = subprocess.Popen([PY, str(HARNESS), "--ddb", endpoint, "--exit-on-stdin-eof", *extra],
                                         stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=self.err)
            m = self.wait(r"HARNESS ready cp_port=(\d+) admin_port=(\d+) stun_ports=(\S+) table=(\S+)", ready_s)
            self._parse_ready(m)
        except BaseException:
            self.close()
            raise

    def _parse_ready(self, m):
        self.admin_port = int(m.group(2))
        self.cp_port = int(m.group(1))
        # --stun 0 이면 "-" 다. 빈 목록으로 읽는다.
        self.stun_ports = [] if m.group(3) == "-" else [int(p) for p in m.group(3).split(",")]
        self.table = m.group(4)

    def text(self) -> str:
        return self.err_path.read_bytes().decode("ascii", "replace")

    def wait(self, pattern, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            m = re.search(pattern, self.text())
            if m:
                return m
            if self.proc.poll() is not None:
                break
            time.sleep(0.05)
        raise AssertionError(f"no {pattern!r} in harness log:\n{self.text()}")

    def admin(self, line):
        with socket.create_connection(("127.0.0.1", self.admin_port), timeout=30) as s:
            s.sendall(line.encode("ascii") + b"\n")
            return json.loads(s.makefile("rb").readline())

    def post(self, op, body, timeout=10):
        data = json.dumps(body).encode("ascii")
        with socket.create_connection(("127.0.0.1", self.cp_port), timeout=timeout) as s:
            s.sendall(f"POST /v1/{op} HTTP/1.1\r\nHost: t\r\nContent-Type: application/json\r\n"
                      f"Content-Length: {len(data)}\r\nConnection: close\r\n\r\n".encode("ascii") + data)
            raw = b""
            while chunk := s.recv(4096):
                raw += chunk
        head, _, body = raw.partition(b"\r\n\r\n")
        return int(head.split(b" ")[1]), json.loads(body)

    def close(self):
        """quit, 그다음 표준 입력 닫기로 끝내 테이블을 지우게 한다. 죽이는 것은 둘 다 안 될 때뿐이다(죽이면
        테이블이 남는다). 프로세스를 만들지 못했으면 로그 파일만 닫는다."""
        try:
            if self.proc is None:
                return
            if self.proc.poll() is None and self.admin_port is not None:
                try:
                    self.admin("quit")
                    self.proc.wait(30)
                except (OSError, subprocess.TimeoutExpired, ValueError):
                    pass
            if self.proc.poll() is None:
                try:
                    self.proc.stdin.close()
                    self.proc.wait(30)
                except (OSError, subprocess.TimeoutExpired):
                    self.proc.kill()
            self.proc.wait(30)
            if self.proc.stdin is not None and not self.proc.stdin.closed:
                self.proc.stdin.close()
        finally:
            self.err.close()


def _tables(endpoint):
    import boto3.session
    from botocore.config import Config

    client = boto3.session.Session().client("dynamodb", region_name=H.FAKE_REGION, endpoint_url=endpoint,
                                            config=Config(proxies={}))
    return client.list_tables()["TableNames"]


@pytest.fixture
def running(request, tmp_path):
    endpoint = request.config.store_endpoint
    h = Running(tmp_path, endpoint)
    yield h
    h.close()
    assert h.table not in _tables(endpoint), "the harness left its table behind"


@pytest.mark.store
def test_injections_reach_the_real_server(running):
    h = running
    # 커밋 뒤 실패: 클라이언트는 internal 을 받지만 방은 생겼다. 같은 nonce 의 재시도가 같은 방을 돌려준다.
    nonce = secrets.token_hex(16)
    assert h.admin("after_commit label=create_room count=1")["ok"]
    status, body = h.post("create_room", {"client_nonce": nonce})
    assert (status, body["error"]) == (500, "internal")
    assert h.admin("count")["items"]["ROOM"] == 1
    status, body = h.post("create_room", {"client_nonce": nonce})
    assert status == 200 and h.admin("count")["items"]["ROOM"] == 1
    # 커밋 뒤 실패 다음에 재생 뒤 실패. store 주입점은 재생에 걸리지 않으므로 둘째는 dispatch 뒤 오류로 만든다.
    nonce2 = secrets.token_hex(16)
    assert h.admin("after_commit label=create_room count=1")["ok"]
    assert h.admin("error op=create_room count=1 code=internal when=after")["ok"]
    statuses = [h.post("create_room", {"client_nonce": nonce2})[0] for _ in range(3)]
    assert statuses == [500, 500, 200] and h.admin("count")["items"]["ROOM"] == 2
    room = h.admin(f"room id={body['room_id']}")
    assert room["room"]["host_peer_id"] == body["peer_id"] and room["vips"] == [
        {"virtual_ip": "10.100.0.1", "peer_id": body["peer_id"]}]

    # 오류 주입은 그 연산의 다음 요청 하나에만 걸린다.
    assert h.admin("error op=join_room count=1 code=unauthorized when=before")["ok"]
    status, other = h.post("create_room", {"client_nonce": secrets.token_hex(16)})
    assert status == 200
    status, err = h.post("join_room", {"room_id": body["room_id"], "client_nonce": secrets.token_hex(16)})
    assert (status, err["error"]) == (403, "unauthorized")
    status, joined = h.post("join_room", {"room_id": body["room_id"], "client_nonce": secrets.token_hex(16)})
    assert status == 200 and joined["virtual_ip"] == "10.100.0.2"

    # 지연 주입.
    assert h.admin("delay op=get_peers count=1 ms=700 when=after")["ok"]
    t = time.monotonic()
    status, _ = h.post("get_peers", {"room_id": body["room_id"], "peer_id": joined["peer_id"],
                                     "peer_token": joined["peer_token"]})
    assert status == 200 and time.monotonic() - t >= 0.69

    # 서버 시계 주입으로 room_expired. 경계는 서버가 판정한다 (control_plane.md 5.1).
    assert h.admin("clock offset_ms=121000")["ok"]
    status, err = h.post("host_report", {"room_id": body["room_id"], "peer_id": body["peer_id"],
                                         "peer_token": body["peer_token"]})
    assert (status, err["error"]) == (410, "room_expired")
    assert h.admin("clock offset_ms=0")["ok"]

    # 속도 제한 보충 시계를 멈추고 10번 태우면 다음 요청이 rate_limited 다. 저장소 앞에서 끝난다.
    assert h.admin("rate_reset")["ok"] and h.admin("rate_freeze")["ok"]
    assert h.admin("burn n=10")["statuses"] == [404] * 10
    status, err = h.post("get_peers", {"room_id": body["room_id"], "peer_id": joined["peer_id"],
                                       "peer_token": joined["peer_token"]})
    assert (status, err["error"]) == (429, "rate_limited")
    assert h.admin("rate_advance s=60")["ok"]
    status, _ = h.post("get_peers", {"room_id": body["room_id"], "peer_id": joined["peer_id"],
                                     "peer_token": joined["peer_token"]})
    assert status == 200

    # 붙들기: 응답이 오지 않는다. drop: 답 없이 닫는다.
    assert h.admin("hold count=1")["ok"]
    with socket.create_connection(("127.0.0.1", h.cp_port), timeout=1) as s:
        s.sendall(b"POST /v1/get_peers HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}")
        with pytest.raises(TimeoutError):
            s.recv(1)
    h.wait(r"HARNESS held op=get_peers")
    assert h.admin("drop count=1")["ok"]
    with socket.create_connection(("127.0.0.1", h.cp_port), timeout=5) as s:
        s.sendall(b"POST /v1/get_peers HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}")
        try:
            assert s.recv(1) == b""
        except ConnectionResetError:
            pass
    h.wait(r"HARNESS dropped op=get_peers")

    assert h.admin("pending") == {"ok": True, "rules": [], "fired": {
        "after_commit": 2, "error": 2, "delay": 1, "hold": 1, "drop": 1}, "offset_ms": 0, "rate_frozen": True}

    # 하네스 로그에도 방 코드와 토큰이 없다.
    text = h.text()
    for secret in (body["room_id"], body["peer_token"], joined["peer_token"], nonce):
        assert secret not in text


@pytest.mark.store
@pytest.mark.parametrize("how", ["quit", "stdin_eof"])
def test_the_table_is_deleted_on_exit(request, tmp_path, how):
    endpoint = request.config.store_endpoint
    h = Running(tmp_path, endpoint)
    try:
        assert h.table in _tables(endpoint)
        if how == "quit":
            assert h.admin("quit")["ok"]
        else:
            h.proc.stdin.close()
        assert h.proc.wait(30) == 0
        h.wait(rf"HARNESS table\.deleted table={h.table}")
        assert h.table not in _tables(endpoint)
    finally:
        h.close()


@pytest.mark.store
def test_ctrl_break_cleans_up(request, tmp_path):
    """Windows 의 Ctrl+Break(SIGBREAK). 새 프로세스 그룹으로 띄워 그 그룹에만 보낸다."""
    if os.name != "nt":
        pytest.skip("Windows only")
    endpoint = request.config.store_endpoint
    err_path = tmp_path / "harness.err"
    with open(err_path, "wb") as err:
        proc = subprocess.Popen([PY, str(HARNESS), "--ddb", endpoint], stdout=subprocess.DEVNULL, stderr=err,
                                creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        try:
            deadline = time.monotonic() + 60
            while "HARNESS ready" not in err_path.read_text("ascii", "replace"):
                assert time.monotonic() < deadline and proc.poll() is None
                time.sleep(0.05)
            table = re.search(r"table=(\S+)", err_path.read_text("ascii", "replace")).group(1)
            proc.send_signal(__import__("signal").CTRL_BREAK_EVENT)
            proc.wait(30)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait(30)
    assert f"HARNESS table.deleted table={table}" in err_path.read_text("ascii", "replace")
    assert table not in _tables(endpoint)


# ---------------------------------------------------------------- 리뷰 c1: 정리 경로

class FakeAdmin:
    def __init__(self, ttl_fails=False, delete_fails=False):
        self.calls = []
        self.ttl_fails = ttl_fails
        self.delete_fails = delete_fails

    def create_table(self, **kw):
        self.calls.append(("create", kw["TableName"]))

    def update_time_to_live(self, **kw):
        self.calls.append(("ttl", kw["TableName"]))
        if self.ttl_fails:
            raise RuntimeError("ttl")

    def delete_table(self, **kw):
        self.calls.append(("delete", kw["TableName"]))
        if self.delete_fails:
            raise RuntimeError("delete")


def test_a_ttl_failure_after_create_deletes_the_table(captured):
    admin = FakeAdmin(ttl_fails=True)
    with pytest.raises(RuntimeError):
        H.create_table(admin, "t1")
    assert admin.calls == [("create", "t1"), ("ttl", "t1"), ("delete", "t1")]


def test_a_ttl_failure_reports_a_failed_rollback_and_keeps_the_first_error(captured):
    admin = FakeAdmin(ttl_fails=True, delete_fails=True)
    with pytest.raises(RuntimeError, match="ttl"):
        H.create_table(admin, "t1")
    assert any("reason=table_delete_failed" in line for line in captured)


def test_table_cleanup_deletes_once_and_reports_failure(captured):
    admin = FakeAdmin()
    cleanup = H.TableCleanup(admin, "t1")
    assert cleanup() is True and cleanup() is True
    assert admin.calls == [("delete", "t1")]
    bad = H.TableCleanup(FakeAdmin(delete_fails=True), "t2")
    assert bad() is False and bad() is False


def test_the_temporary_directory_is_removed_when_ddb_is_unreachable(tmp_path):
    tmp = tmp_path / "tmp"
    tmp.mkdir()
    env = dict(os.environ, TMP=str(tmp), TEMP=str(tmp), TMPDIR=str(tmp))
    proc = subprocess.run([PY, str(HARNESS), "--ddb", f"http://127.0.0.1:{_free_closed_port()}"], env=env,
                          capture_output=True, timeout=120, text=True, encoding="ascii", errors="replace")
    assert proc.returncode == H.EXIT_NO_DDB, proc.stderr
    assert list(tmp.iterdir()) == []


def test_running_cleans_up_when_the_harness_never_gets_ready(tmp_path, monkeypatch):
    started = []
    real = subprocess.Popen

    def recording(*a, **kw):
        p = real(*a, **kw)
        started.append(p)
        return p

    monkeypatch.setattr(subprocess, "Popen", recording)
    with pytest.raises(AssertionError):
        Running(tmp_path, f"http://127.0.0.1:{_free_closed_port()}", ready_s=0.5)
    assert len(started) == 1
    assert started[0].poll() is not None          # 프로세스가 끝났다
    assert started[0].stdin.closed                # 파이프를 닫았다


@pytest.mark.store
@pytest.mark.parametrize("how", ["quit", "ctrl_break"])
def test_stopping_during_a_maximum_delay_still_deletes_the_table(request, tmp_path, how):
    """리뷰 c1. 60초 지연으로 자는 dispatch 스레드가 있어도 멈춤이 곧 끝나고 테이블이 사라진다."""
    import threading

    if how == "ctrl_break" and os.name != "nt":
        pytest.skip("Windows only")
    endpoint = request.config.store_endpoint
    real = subprocess.Popen

    def new_group(*a, **kw):
        kw["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
        return real(*a, **kw)

    if how == "ctrl_break":
        request.getfixturevalue("monkeypatch").setattr(subprocess, "Popen", new_group)
    h = Running(tmp_path, endpoint)
    try:
        assert h.admin(f"delay op=create_room count=1 ms={H.MAX_DELAY_MS} when=before")["ok"]

        def call():
            try:
                h.post("create_room", {"client_nonce": secrets.token_hex(16)}, timeout=90)
            except (OSError, IndexError, ValueError):
                pass  # 하네스가 멈추며 답 없이 닫는다

        threading.Thread(target=call, daemon=True).start()
        h.wait(r"HARNESS inject kind=delay op=create_room ms=60000 when=before")
        t = time.monotonic()
        if how == "quit":
            assert h.admin("quit")["ok"]
        else:
            h.proc.send_signal(__import__("signal").CTRL_BREAK_EVENT)
        h.proc.wait(20)
        assert time.monotonic() - t < 15
        h.wait(rf"HARNESS table\.deleted table={h.table}")
        assert h.table not in _tables(endpoint)
    finally:
        h.close()


# ---------------------------------------------------------------- 리뷰 c2: 생성자 실패 정리

def test_running_closes_the_log_when_the_process_cannot_start(tmp_path, monkeypatch):
    """파일이 닫혔는지는 그 파일 객체로 직접 본다. 객체가 수거되면 닫히므로 지우기 시도로는 가를 수 없다."""
    import builtins

    opened = []

    def recording_open(*a, **kw):
        f = builtins.open(*a, **kw)
        opened.append(f)
        return f

    def refuse(*a, **kw):
        raise OSError("no process")

    monkeypatch.setattr(sys.modules[__name__], "open", recording_open, raising=False)
    monkeypatch.setattr(subprocess, "Popen", refuse)
    with pytest.raises(OSError):
        Running(tmp_path, "http://127.0.0.1:1")
    assert len(opened) == 1 and opened[0].closed


class BrokenParse(Running):
    def _parse_ready(self, m):
        super()._parse_ready(m)
        raise ValueError("parse failed after ready")


@pytest.mark.store
def test_running_cleans_up_when_parsing_the_ready_line_fails(request, tmp_path):
    """준비 줄 뒤의 실패. 프로세스가 끝나고 테이블이 사라지고 로그가 닫힌다."""
    endpoint = request.config.store_endpoint
    started = []
    real = subprocess.Popen

    def recording(*a, **kw):
        p = real(*a, **kw)
        started.append(p)
        return p

    mp = request.getfixturevalue("monkeypatch")
    mp.setattr(subprocess, "Popen", recording)
    with pytest.raises(ValueError, match="after ready"):
        BrokenParse(tmp_path, endpoint)
    text = (tmp_path / "harness.err").read_bytes().decode("ascii", "replace")
    table = re.search(r"table=(cp-harness-\w+)", text).group(1)
    assert started[0].poll() is not None
    assert f"HARNESS table.deleted table={table}" in text
    assert table not in _tables(endpoint)
    os.remove(tmp_path / "harness.err")


@pytest.mark.store
def test_running_reads_an_empty_stun_list(request, tmp_path):
    h = Running(tmp_path, request.config.store_endpoint, "--stun", "0")
    try:
        assert h.stun_ports == [] and h.admin("ping")["ok"]
    finally:
        h.close()
    assert h.table not in _tables(request.config.store_endpoint)


def test_table_cleanup_treats_a_missing_table_as_clean(captured):
    from botocore.exceptions import ClientError

    class Missing(FakeAdmin):
        def delete_table(self, **kw):
            raise ClientError({"Error": {"Code": "ResourceNotFoundException", "Message": "x"}}, "DeleteTable")

    assert H.TableCleanup(Missing(), "t3")() is True
    assert any("HARNESS table.absent table=t3" == line for line in captured)
