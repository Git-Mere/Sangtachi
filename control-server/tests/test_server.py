"""7.2 수락 루프와 7.5 로그·카운터, 7.6 설정의 시험. 대상은 controlplane/server.py 의 Server 와
controlplane/__main__.py 다. 변이는 tests/mutants/server.py.

control_plane.md 에 이 부분의 케이스 표는 없다. 아래 목록은 이 파일이 만든 것이고, 원소마다 note 에
출처 절을 적었다. 연결은 전부 127.0.0.1 의 임시 포트에 실제 소켓으로 맺는다. dispatch 는 대역이다.
"""

from __future__ import annotations

import asyncio
import errno
import gc
import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time
import warnings
from pathlib import Path

import pytest

from controlplane import __main__ as cp_main
from controlplane import log
from controlplane.constants import (CONTROL_PORT, LINGER_MAX_BYTES, LINGER_S, MAX_INFLIGHT, MAX_REJECTING, MAX_RATE_ENTRIES, RATE_LIMIT_BUCKET,
                                   SERVER_READ_TIMEOUT_S)
from controlplane.errors import OpError
from controlplane.server import BIND_HOST, COUNTER_NAMES, COUNTER_PERIOD_S, Request, Server

HOST = "127.0.0.1"
LONG_S = 30.0       # 붙들어 둘 연결의 읽기 시간 제한. 시험 안에서 만료되지 않을 만큼 길다
DEADLINE_S = 5.0    # 일어나야 할 일을 기다리는 상한. 변이 실행기의 -j 8 에서도 넉넉하다
NO_ANSWER_WAIT_S = 2.0  # 응답이 없어야 하는 연결을 기다리는 상한. 기본 읽기 시간 제한(5초)보다 짧아야 한다
SHORT_S = 0.3       # 시험이 줄여 주입하는 시간 제한
MARK = "ZQXMARK"    # 요청에 넣는 표지. 로그 줄에 나오면 안 된다 (7.5)


@pytest.fixture
def lines():
    got: list[str] = []
    log.set_sink(got.append)
    yield got
    log.set_sink(None)


class Spy:
    """dispatch 대역. 불린 요청을 적고 behave 가 정한 대로 답한다."""

    def __init__(self, behave=None):
        self.calls: list[Request] = []
        self.behave = behave or (lambda req: {"echo": req.op})

    def __call__(self, req: Request) -> dict:
        self.calls.append(req)
        return self.behave(req)


def http(op: str, body: dict | bytes) -> bytes:
    raw = body if isinstance(body, bytes) else json.dumps(body).encode()
    return (f"POST /v1/{op} HTTP/1.1\r\nHost: t\r\nContent-Type: application/json\r\n"
            f"Content-Length: {len(raw)}\r\nConnection: close\r\n\r\n").encode() + raw


def parse(out: bytes) -> tuple[int, dict]:
    head, _, body = out.partition(b"\r\n\r\n")
    return int(head.split(b" ")[1]), json.loads(body)


def records(got: list[str], event: str) -> list[dict]:
    """event 줄을 이름=값 dict 로. 수준은 'level' 키다."""
    out = []
    for line in got:
        parts = line.split(" ")
        if parts[1] == event:
            out.append(dict(p.split("=", 1) for p in parts[2:]) | {"level": parts[0]})
    return out


async def until(pred, what: str, timeout: float = DEADLINE_S) -> None:
    loop = asyncio.get_running_loop()
    end = loop.time() + timeout
    while not pred():
        if loop.time() > end:
            raise AssertionError(f"{timeout}초 안에 일어나지 않았다: {what}")
        await asyncio.sleep(0.01)


class Running:
    """Server 하나를 임시 포트에 띄운다. async with 로 쓴다."""

    def __init__(self, server: Server, period_s: float = 60.0):
        self.server = server
        self.period_s = period_s
        self.stop = asyncio.Event()

    async def __aenter__(self):
        ready: asyncio.Future = asyncio.get_running_loop().create_future()
        self.task = asyncio.create_task(self.server.serve(HOST, 0, self.stop, counter_period_s=self.period_s,
                                                          on_ready=ready.set_result))
        self.port = await asyncio.wait_for(ready, DEADLINE_S)
        return self

    async def __aexit__(self, *exc):
        self.stop.set()
        await asyncio.wait_for(self.task, DEADLINE_S)

    async def exchange(self, raw: bytes) -> bytes:
        """raw 를 보내고 EOF 까지 읽는다."""
        reader, writer = await asyncio.open_connection(HOST, self.port)
        try:
            writer.write(raw)
            await writer.drain()
            return await asyncio.wait_for(reader.read(), DEADLINE_S)
        finally:
            writer.close()


def run(coro):
    return asyncio.run(coro)


# ---------------------------------------------------------------- MAX_INFLIGHT (7.2)

async def _hold(running: Running, n: int) -> list:
    """요청을 보내지 않는 연결 n 개를 열고 서버가 그만큼 처리 중으로 셀 때까지 기다린다."""
    held = [await asyncio.open_connection(HOST, running.port) for _ in range(n)]
    await until(lambda: running.server.inflight == n, f"inflight == {n}")
    return held


async def _release(running: Running, held: list) -> None:
    for _, w in held:
        w.close()
    await until(lambda: running.server.inflight == 0, "inflight == 0")


INFLIGHT_CASES = [
    dict(id="inflight-33rd-unavailable", hold=MAX_INFLIGHT, send=b"", expect=(503, "unavailable"),
         note="7.2: inflight >= MAX_INFLIGHT 이면 수락 직후, 파싱 전에 503 unavailable 이고 unavailable 카운터를 "
              "올린다. 33번째 연결은 아무 바이트도 보내지 않는다. 파싱하는 구현은 읽기 시간 제한까지 답하지 않는다"),
    dict(id="inflight-32nd-served", hold=MAX_INFLIGHT - 1, send=http("get_peers", {"room_id": "abc"}),
         expect=(200, None),
         note="7.2: 처리 중이 31 이면 32번째 요청은 상한 안쪽이다. 정상 처리된다"),
]


@pytest.mark.parametrize("case", INFLIGHT_CASES, ids=[c["id"] for c in INFLIGHT_CASES])
def test_inflight(case, lines):
    spy = Spy()

    async def go():
        server = Server(spy, read_timeout_s=LONG_S)
        async with Running(server) as running:
            held = await _hold(running, case["hold"])
            try:
                out = await running.exchange(case["send"])
            finally:
                await _release(running, held)
            return server, out

    server, out = run(go())
    status, payload = parse(out)
    want_status, want_error = case["expect"]
    assert status == want_status
    reqs = records(lines, "http.request")
    if want_error is None:
        assert payload == {"ok": True, "echo": "get_peers"}
        assert len(spy.calls) == 1 and server.counters["unavailable"] == 0
        return
    assert payload["ok"] is False and payload["error"] == want_error
    assert spy.calls == []
    assert server.counters["unavailable"] == 1
    assert [(r["op"], r["status"], r["error"]) for r in reqs] == [("-", "503", "unavailable")]


def test_inflight_released_after_each_request(lines):
    # 7.2 finally: inflight -= 1. 상한의 두 배를 넘게 차례로 보내도 전부 처리된다.
    spy = Spy()

    async def go():
        server = Server(spy)
        async with Running(server) as running:
            outs = [await running.exchange(http("get_peers", {"room_id": "abc"})) for _ in range(2 * MAX_INFLIGHT + 1)]
            await until(lambda: server.inflight == 0, "inflight == 0")
        return server, outs

    server, outs = run(go())
    assert [parse(o)[0] for o in outs] == [200] * (2 * MAX_INFLIGHT + 1)
    assert server.counters["unavailable"] == 0





def test_inflight_held_during_dispatch(lines):
    """7.2: inflight 는 수락부터 응답 송신까지 하나로 센다. dispatch 동안에도 자리를 잡고 있다.

    dispatch 를 threading.Event 로 막고 파싱이 끝난 요청 MAX_INFLIGHT 개로 채운 뒤, 일꾼을 풀기 전에 다음 연결이
    503 인지 본다. dispatch 동안 inflight 를 내렸다가 송신 전에 되올리는 구현이 여기서 떨어진다.
    """
    gate = threading.Event()

    def blocked(req):
        gate.wait(DEADLINE_S * 2)
        return {}

    spy = Spy(blocked)

    async def go():
        server = Server(spy)
        async with Running(server) as running:
            tasks = [asyncio.create_task(running.exchange(http("get_peers", {"room_id": "abc"})))
                     for _ in range(MAX_INFLIGHT)]
            try:
                await until(lambda: len(spy.calls) == MAX_INFLIGHT, "dispatch 가 MAX_INFLIGHT 개 막힘")
                out = await running.exchange(b"")
            finally:
                gate.set()
            outs = await asyncio.gather(*tasks)
        return out, outs

    out, outs = run(go())
    assert parse(out)[0] == 503
    assert [parse(o)[0] for o in outs] == [200] * MAX_INFLIGHT

# 거절(503) 중인 연결의 상한. 거절 중인 연결은 링거하는 동안 남는다. linger_s 를 길게 주입해 붙들어 둔다.
REJECT_CASES = [
    dict(id="reject-over-cap-aborts", hold=2, max_rejecting=2, expect=None,
         note="거절 중인 연결이 MAX_REJECTING 이면 새 연결은 응답 없이 끊고 unavailable 을 올린다. 줄도 없다"),
    dict(id="reject-under-cap-answers", hold=1, max_rejecting=2, expect=503,
         note="거절 중인 연결이 상한보다 적으면 503 으로 답한다"),
]


@pytest.mark.parametrize("case", REJECT_CASES, ids=[c["id"] for c in REJECT_CASES])
def test_rejecting_cap(case, lines):
    async def go():
        server = Server(Spy(), max_inflight=0, max_rejecting=case["max_rejecting"], linger_s=LONG_S)
        async with Running(server) as running:
            held = [await asyncio.open_connection(HOST, running.port) for _ in range(case["hold"])]
            await until(lambda: server.rejecting == case["hold"], f"rejecting == {case['hold']}")
            reader, writer = await asyncio.open_connection(HOST, running.port)
            try:
                out = await asyncio.wait_for(reader.read(), NO_ANSWER_WAIT_S)
            except ConnectionError:
                out = b""
            writer.close()
            for _, w in held:
                w.close()
            await until(lambda: server.rejecting == 0, "rejecting == 0")
            return server, out

    server, out = run(go())
    assert server.counters["unavailable"] == case["hold"] + 1
    statuses = [r["status"] for r in records(lines, "http.request")]
    if case["expect"] is None:
        assert out == b"" and statuses == ["503"] * case["hold"]
    else:
        assert parse(out)[0] == 503 and statuses == ["503"] * (case["hold"] + 1)


def test_dispatch_runs_on_own_pool(lines):
    # 7.2 스레드 풀 하나. dispatch 는 이 Server 의 풀(cp-dispatch-*)에서 돈다.
    names = []
    spy = Spy(lambda req: names.append(threading.current_thread().name) or {})

    async def go():
        async with Running(Server(spy)) as running:
            await running.exchange(http("get_peers", {"room_id": "abc"}))

    run(go())
    assert len(names) == 1 and names[0].startswith("cp-dispatch")

# 7.2 finally: close. 서버가 닫지 않아도 CPython 은 StreamWriter 가 버려질 때 닫고 ResourceWarning
# "unclosed <StreamWriter ...>" 를 낸다. 그래서 EOF 가 아니라 그 경고로 본다.
CLOSE_CASES = [
    dict(id="close-answered", max_inflight=MAX_INFLIGHT, raw=http("get_peers", {"room_id": "abc"}), expect=b"HTTP/1.1 200 ",
         note="7.2: 응답한 연결을 닫는다"),
    dict(id="close-unavailable", max_inflight=0, raw=b"", expect=b"HTTP/1.1 503 ",
         note="7.2: unavailable 로 답한 연결을 닫는다. 상한 0 을 주입해 첫 연결이 503 이다"),
    dict(id="close-no-answer", max_inflight=MAX_INFLIGHT, raw=b"POST /v1/get_peers HTTP/1.1\r\n", expect=b"",
         note="3.4: 응답 없이 닫는 경로도 닫는다"),
]


@pytest.mark.parametrize("case", CLOSE_CASES, ids=[c["id"] for c in CLOSE_CASES])
def test_server_closes_connection(case, lines):
    async def go():
        server = Server(Spy(), read_timeout_s=SHORT_S, max_inflight=case["max_inflight"])
        async with Running(server) as running:
            out = await running.exchange(case["raw"])
            await until(lambda: server.inflight == 0, "inflight == 0")
            return out

    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always", ResourceWarning)
        out = run(go())
        gc.collect()
    assert out.startswith(case["expect"])
    assert [str(w.message) for w in caught if "StreamWriter" in str(w.message)] == []

# ---------------------------------------------------------------- 응답 송신 시간 제한 (3.4)

class FakeTransport:
    def __init__(self, events: list):
        self.events = events

    def abort(self):
        self.events.append("abort")

    def is_closing(self):
        return "close" in self.events or "abort" in self.events


class FakeWriter:
    """통제된 writer. 불린 순서를 events 에 적는다. drain_blocks 면 drain 이 끝나지 않는다(읽지 않는 상대).
    on_eof 는 write_eof 가 불릴 때 한 번 불린다. 시험은 거기서 늦은 바이트를 reader 에 넣는다. 그래서 늦은 바이트는
    언제나 응답 송신이 끝나고 쓰기 쪽이 닫힌 뒤에 온다."""

    def __init__(self, *, drain_blocks: bool = False, on_eof=None):
        self.events: list[str] = []
        self.data = b""
        self.transport = FakeTransport(self.events)
        self._drain_blocks = drain_blocks
        self._on_eof = on_eof

    def get_extra_info(self, name):
        return ("198.51.100.9", 40000) if name == "peername" else None

    def write(self, data: bytes) -> None:
        self.events.append("write")
        self.data += data

    async def drain(self) -> None:
        self.events.append("drain")
        if self._drain_blocks:
            await asyncio.Event().wait()

    def write_eof(self) -> None:
        self.events.append("write_eof")
        if self._on_eof is not None:
            self._on_eof()

    def close(self) -> None:
        self.events.append("close")


UNIT_BOUND_S = 2.0  # 통제된 writer 시험 한 건의 상한. 이것을 넘으면 기다리면 안 되는 것을 기다린 것이다


def drive(server: Server, first: bytes, writer: FakeWriter, *, eof: bool = False) -> asyncio.StreamReader:
    """reader 에 first 를 넣고 on_connection 한 번을 UNIT_BOUND_S 안에 돌린다. reader 를 돌려준다."""
    async def go():
        reader = asyncio.StreamReader()
        if first:
            reader.feed_data(first)
        if eof:
            reader.feed_eof()
        writer.reader = reader
        await asyncio.wait_for(server.on_connection(reader, writer), UNIT_BOUND_S)
        return reader

    return run(go())


# 3.4 응답 송신 시간 제한. 통제된 writer 로 drain 이 끝나지 않게 만든다.
WRITE_CASES = [
    dict(id="write-drain-timeout-aborts", drain_blocks=True, events=["write", "drain", "abort", "close"],
         note="3.4: drain 이 write_timeout_s 안에 끝나지 않으면 남은 바이트를 버리고 abort 한다. 그 뒤 close"),
    dict(id="write-drain-ok-no-abort", drain_blocks=False, events=["write", "drain", "close"],
         note="송신이 끝나면 abort 하지 않는다. 정상 요청이라 링거도 없다"),
]


@pytest.mark.parametrize("case", WRITE_CASES, ids=[c["id"] for c in WRITE_CASES])
def test_write_timeout_controlled(case, lines):
    writer = FakeWriter(drain_blocks=case["drain_blocks"])
    drive(Server(Spy(), write_timeout_s=0.05), http("get_peers", {"room_id": "abc"}), writer)
    assert writer.events == case["events"]
    assert writer.data.startswith(b"HTTP/1.1 200 ")
    assert [r["status"] for r in records(lines, "http.request")] == ["200"]


BLOB = "x" * (4 * 1024 * 1024)


def test_write_timeout_aborts_connection(lines):
    """3.4 송신 시간 제한의 소켓 통합 시험. 변이 판정은 test_write_timeout_controlled 가 한다.

    Windows 는 송신 버퍼를 따로 정하지 않은 소켓의 송신을 크기와 무관하게 받아 버퍼에 넣는다(32MB, 400MB
    로 확인했다). 그래서 수락한 소켓의 SO_SNDBUF 를 0 으로, 상대의 SO_RCVBUF 를 4096 으로 줄여 상대가 읽지
    않으면 송신이 끝나지 않게 만든다. 이 조작이 OS 에 따라 듣지 않을 수 있어 변이의 kills 에 걸지 않는다.
    클라이언트는 블로킹 소켓이고 일꾼 스레드에서 돈다. 이벤트 루프를 막지 않는다.
    """
    spy = Spy(lambda req: {"blob": BLOB})

    def connect_and_send(port: int) -> socket.socket:
        client = socket.socket()
        client.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        client.connect((HOST, port))
        client.sendall(http("get_peers", {"room_id": "abc"}))
        return client

    def drain_client(client: socket.socket) -> int:
        client.settimeout(DEADLINE_S)
        got = 0
        try:
            while chunk := client.recv(2**20):
                got += len(chunk)
        except ConnectionError:
            pass
        finally:
            client.close()
        return got

    async def go():
        server = Server(spy, write_timeout_s=SHORT_S)

        async def accept(reader, writer):
            writer.get_extra_info("socket").setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 0)
            await server.on_connection(reader, writer)

        srv = await asyncio.start_server(accept, HOST, 0)
        try:
            client = await asyncio.to_thread(connect_and_send, srv.sockets[0].getsockname()[1])
            # 읽지 않는다. 서버가 송신을 포기하고 연결을 놓을 때까지 기다린다.
            await until(lambda: spy.calls and server.inflight == 0, "송신 포기")
            return await asyncio.to_thread(drain_client, client)
        finally:
            srv.close()

    got = run(go())
    assert got < len(json.dumps({"blob": BLOB, "ok": True}))  # 남은 응답을 끝까지 보내지 않았다
    assert [r["status"] for r in records(lines, "http.request")] == ["200"]


# ---------------------------------------------------------------- http.request 줄 (7.5)

SECRET_BODY = {"room_id": MARK + "R", "peer_id": 7, "peer_token": MARK + "T", "client_nonce": MARK + "N"}


def _raise(exc):
    def behave(req):
        raise exc
    return behave


def _exhaust(server: Server) -> None:
    for _ in range(RATE_LIMIT_BUCKET):
        server.rate.spend(HOST)


# 줄 하나의 기대값. op 는 파싱이 끝난 요청의 연산 이름이고 파싱 전에 끝나면 '-' 다. 요청 바이트 곳곳에 MARK
# 를 넣고 dispatch 가 던지는 오류의 message 와 예외 내용에도 넣는다. 어느 줄에도 MARK 가 나오면 안 된다.
LOG_CASES = [
    dict(id="log-ok", raw=http("get_peers", SECRET_BODY), behave=None, prepare=None,
         line=("get_peers", "200", "-"), dispatched=True,
         note="7.5 http.request: op, status, error(성공이면 -), src, ms. room_id, peer_token, 본문을 싣지 않는다"),
    dict(id="log-op-error", raw=http("register_candidate", SECRET_BODY),
         behave=_raise(OpError("unauthorized", MARK + "-message")), prepare=None,
         line=("register_candidate", "403", "unauthorized"), dispatched=True,
         note="7.5: error 는 4.1 오류 코드다. message 는 싣지 않는다"),
    dict(id="log-internal", raw=http("join_room", SECRET_BODY), behave=_raise(RuntimeError(MARK + "-boom")),
         prepare=None, line=("join_room", "500", "internal"), dispatched=True,
         note="7.2 포괄 처리: 500 internal. 예외 내용은 본문에도 줄에도 싣지 않는다"),
    dict(id="log-rate-limited", raw=http("get_peers", SECRET_BODY), behave=None, prepare=_exhaust,
         line=("get_peers", "429", "rate_limited"), dispatched=False,
         note="6.4, 7.2: 파싱 뒤 예산 소진이면 429 이고 dispatch 를 부르지 않는다. op 는 파싱한 이름이다"),
    dict(id="log-unknown-op", raw=http(MARK, SECRET_BODY), behave=None, prepare=None,
         line=("-", "404", "unknown_op"), dispatched=False,
         note="7.5: op 는 4장의 연산 다섯 가운데 하나일 때만 싣는다. 경로의 바이트를 싣지 않는다"),
    dict(id="log-bad-body", raw=http("get_peers", ("{" + MARK).encode()), behave=None, prepare=None,
         line=("-", "400", "bad_request"), dispatched=False,
         note="3.3 파싱 실패. 요청을 다 읽지 못했으므로 op 는 '-' 다"),
]


@pytest.mark.parametrize("case", LOG_CASES, ids=[c["id"] for c in LOG_CASES])
def test_request_line(case, lines):
    spy = Spy(case["behave"])

    async def go():
        server = Server(spy)
        if case["prepare"]:
            case["prepare"](server)
        async with Running(server) as running:
            return await running.exchange(case["raw"])

    out = run(go())
    status, payload = parse(out)
    reqs = records(lines, "http.request")
    assert len(reqs) == 1
    rec = reqs[0]
    assert (rec["op"], rec["status"], rec["error"]) == case["line"]
    assert str(status) == rec["status"]
    assert rec["level"] == "INFO" and rec["src"] == HOST and rec["ms"].isdigit()
    assert set(rec) == {"level", "op", "status", "error", "src", "ms"}
    assert bool(spy.calls) == case["dispatched"]
    for line in lines:
        assert MARK not in line
    if case["id"] != "log-op-error":  # 그 행의 message 는 dispatch 가 만든 문구이고 4.1 대로 본문에 실린다
        assert MARK.encode() not in out


def test_round_trip_fields_and_body(lines):
    # 7.2 ops.dispatch 의 계약: 성공이면 연산 필드에 서버가 ok: true 를 붙인다. 상태는 200.
    spy = Spy(lambda req: {"peers": [], "n": 3})

    async def go():
        async with Running(Server(spy)) as running:
            return await running.exchange(http("get_peers", {"room_id": "abc", "peer_id": 1}))

    status, payload = parse(run(go()))
    assert (status, payload) == (200, {"ok": True, "peers": [], "n": 3})
    assert spy.calls == [Request("get_peers", {"room_id": "abc", "peer_id": 1})]


NO_LINE_CASES = [
    dict(id="nolog-read-timeout", pieces=[http("get_peers", {"room_id": "abc"})[:20]], eof=False,
         counter="http_read_timeout",
         note="3.4: 시간 안에 다 받지 못하면 응답 없이 닫고 http_read_timeout 을 올린다. 응답이 없으니 줄도 없다"),
    dict(id="nolog-eof", pieces=[http("get_peers", {"room_id": "abc"})[:20]], eof=True, counter=None,
         note="7.2: 다 받기 전에 닫혔다. 응답 없음, 카운터 없음"),
]


@pytest.mark.parametrize("case", NO_LINE_CASES, ids=[c["id"] for c in NO_LINE_CASES])
def test_no_line_without_answer(case, lines):
    spy = Spy()

    async def go():
        server = Server(spy, read_timeout_s=SHORT_S)
        async with Running(server) as running:
            reader, writer = await asyncio.open_connection(HOST, running.port)
            for piece in case["pieces"]:
                writer.write(piece)
            await writer.drain()
            if case["eof"]:
                writer.write_eof()
            out = await asyncio.wait_for(reader.read(), NO_ANSWER_WAIT_S)
            writer.close()
            await until(lambda: server.inflight == 0, "inflight == 0")
        return server, out

    server, out = run(go())
    assert out == b"" and spy.calls == []
    assert records(lines, "http.request") == []
    want = {case["counter"]: 1} if case["counter"] else {}
    assert {k: v for k, v in server.counters.items() if v} == want


def test_ms_is_elapsed_milliseconds(lines):
    # 7.5 ms: 수락부터 응답을 보내기까지의 경과. 시계를 주입해 0.25초를 250 으로 찍는지 본다.
    ticks = iter([10.0, 10.25])

    async def go():
        async with Running(Server(Spy(), clock=lambda: next(ticks))) as running:
            return await running.exchange(http("get_peers", {"room_id": "abc"}))

    run(go())
    assert [r["ms"] for r in records(lines, "http.request")] == ["250"]


# ---------------------------------------------------------------- 카운터 (7.5)

def batches(got: list[str]) -> list[list[tuple[str, str]]]:
    """counter 줄을 여섯씩 묶는다. 묶음마다 COUNTER_NAMES 순서여야 한다."""
    recs = [(r["name"], r["value"]) for r in records(got, "counter")]
    assert len(recs) % len(COUNTER_NAMES) == 0, recs
    out = [recs[i:i + len(COUNTER_NAMES)] for i in range(0, len(recs), len(COUNTER_NAMES))]
    for b in out:
        assert [n for n, _ in b] == list(COUNTER_NAMES)
    return out


def test_counters_periodic_and_at_stop(lines):
    """7.5: 60초마다와 종료 시 전량. 0 인 것도 낸다. 주기를 0.1초로 주입한다."""
    async def go():
        async with Running(Server(Spy()), period_s=0.1):
            await until(lambda: len(records(lines, "counter")) >= 2 * len(COUNTER_NAMES), "주기 출력 두 번")
            before = len(records(lines, "counter")) // len(COUNTER_NAMES)
        return before

    before = run(go())
    got = batches(lines)
    assert len(got) == before + 1  # 멈출 때 한 번 더
    assert all(v == "0" for b in got for _, v in b)


def test_counters_at_cancel(lines):
    """Windows 의 Ctrl+C 는 asyncio.run 이 주 태스크를 취소하는 것으로 온다. 그때도 전량을 낸다."""
    async def go():
        running = Running(Server(Spy()))
        await running.__aenter__()
        running.task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await running.task

    run(go())
    assert len(batches(lines)) == 1


def test_counter_values(lines):
    # 6.4 rate_table_full 은 속도 제한 표가, unavailable 등은 서버가, elapsed_wall_fallback 은 ops 가 센다.
    server = Server(Spy(), service_counters=lambda: {"elapsed_wall_fallback": 3})
    for i in range(MAX_RATE_ENTRIES + 1):
        server.rate.spend(f"10.{i >> 16 & 255}.{i >> 8 & 255}.{i & 255}")
    server.counters["unavailable"] += 2
    server.emit_counters()
    assert dict(batches(lines)[0]) == {"http_read_timeout": "0", "internal_error": "0", "rate_limited": "0",
                                       "rate_table_full": "1", "elapsed_wall_fallback": "3", "unavailable": "2"}


def test_counter_values_without_service_counters(lines):
    # ops 가 카운터를 주지 않으면 elapsed_wall_fallback 은 0 이다. 키가 아직 없어도 0 이다.
    Server(Spy()).emit_counters()
    Server(Spy(), service_counters=dict).emit_counters()
    assert [dict(b)["elapsed_wall_fallback"] for b in batches(lines)] == ["0", "0"]


# ---------------------------------------------------------------- 설정 (7.6)

BASE_ENV = {"SANGTACHI_CP_TABLE": "t1", "AWS_REGION": "ap-northeast-2"}


def env(**kw) -> dict:
    out = dict(BASE_ENV)
    for k, v in kw.items():
        if v is None:
            out.pop(k, None)
        else:
            out[k] = v
    return out


# control_plane.md 7.6 설정 표 네 행. expect 가 문자열이면 그 변수 이름이 오류 문구에 나와야 한다.
ENV_CASES = [
    dict(id="env-defaults", env=env(), expect=(CONTROL_PORT, "t1", "ap-northeast-2", None),
         note="SANGTACHI_CP_PORT 기본값 8000, SANGTACHI_CP_ENDPOINT 없으면 리전 기본"),
    dict(id="env-port-set", env=env(SANGTACHI_CP_PORT="8080"), expect=(8080, "t1", "ap-northeast-2", None),
         note="SANGTACHI_CP_PORT: bind 포트"),
    dict(id="env-port-1", env=env(SANGTACHI_CP_PORT="1"), expect=(1, "t1", "ap-northeast-2", None),
         note="포트 하한 경계"),
    dict(id="env-port-65535", env=env(SANGTACHI_CP_PORT="65535"), expect=(65535, "t1", "ap-northeast-2", None),
         note="포트 상한 경계"),
    dict(id="env-endpoint", env=env(SANGTACHI_CP_ENDPOINT="http://127.0.0.1:8001"),
         expect=(CONTROL_PORT, "t1", "ap-northeast-2", "http://127.0.0.1:8001"),
         note="SANGTACHI_CP_ENDPOINT: 로컬 시험용 덮어쓰기"),
    dict(id="env-no-table", env=env(SANGTACHI_CP_TABLE=None), expect="SANGTACHI_CP_TABLE",
         note="SANGTACHI_CP_TABLE 필수"),
    dict(id="env-empty-table", env=env(SANGTACHI_CP_TABLE=""), expect="SANGTACHI_CP_TABLE",
         note="빈 값은 없는 것과 같다 (store.Store.from_env 와 같은 판정)"),
    dict(id="env-no-region", env=env(AWS_REGION=None), expect="AWS_REGION", note="AWS_REGION 필수"),
    dict(id="env-port-0", env=env(SANGTACHI_CP_PORT="0"), expect="SANGTACHI_CP_PORT", note="포트 0 은 bind 포트가 아니다"),
    dict(id="env-port-65536", env=env(SANGTACHI_CP_PORT="65536"), expect="SANGTACHI_CP_PORT", note="범위 밖"),
    dict(id="env-port-empty", env=env(SANGTACHI_CP_PORT=""), expect="SANGTACHI_CP_PORT",
         note="빈 값은 판정 불가. 기본값으로 바꾸지 않는다"),
    dict(id="env-port-word", env=env(SANGTACHI_CP_PORT="http"), expect="SANGTACHI_CP_PORT", note="숫자 아님"),
    dict(id="env-port-sign", env=env(SANGTACHI_CP_PORT="+8000"), expect="SANGTACHI_CP_PORT",
         note="부호. int() 는 받는다"),
    dict(id="env-port-space", env=env(SANGTACHI_CP_PORT=" 8000"), expect="SANGTACHI_CP_PORT",
         note="앞 공백. int() 는 받는다"),
    dict(id="env-port-fullwidth", env=env(SANGTACHI_CP_PORT="８０００"), expect="SANGTACHI_CP_PORT",
         note="전각 숫자. int() 는 받는다"),
    dict(id="env-port-leading-zero", env=env(SANGTACHI_CP_PORT="08000"), expect="SANGTACHI_CP_PORT",
         note="선행 0. 10진 표기를 하나로 둔다"),
] + [
    dict(id=f"env-endpoint-ok-{name}", env=env(SANGTACHI_CP_ENDPOINT=url),
         expect=(CONTROL_PORT, "t1", "ap-northeast-2", url), note=f"SANGTACHI_CP_ENDPOINT 를 받는다: {why}")
    for name, url, why in [
        ("https", "https://dynamodb.example:443", "https 와 포트"),
        ("slash", "http://127.0.0.1:8001/", "끝의 / 하나는 경로가 없는 것과 같다"),
        ("no-port", "http://localhost", "포트 없음"),
    ]
] + [
    dict(id=f"env-endpoint-{name}", env=env(SANGTACHI_CP_ENDPOINT=url), expect="SANGTACHI_CP_ENDPOINT",
         note=f"SANGTACHI_CP_ENDPOINT 형식 위반은 설정 오류다: {why}")
    for name, url, why in [
        ("ftp", "ftp://h:21", "scheme 은 http 나 https"),
        ("no-scheme", "127.0.0.1:8001", "scheme 없음"),
        ("no-host", "http://:8001", "호스트 없음"),
        ("path", "http://h:8001/v1", "경로"),
        ("query", "http://h:8001?a=1", "질의"),
        ("fragment", "http://h:8001#x", "조각"),
        ("userinfo", "http://u:p@h:8001", "사용자 정보"),
        ("port-word", "http://h:abc", "포트가 숫자가 아님"),
        ("port-range", "http://h:99999", "포트 범위 밖"),
        ("newline", "http://h:8001\n", "줄바꿈. urlsplit 은 지우고 읽는다"),
        ("tab", "http://h\t:8001", "탭. urlsplit 은 지우고 읽는다"),
        ("non-ascii", "http://ｈ:8001", "ASCII 밖의 글자"),
        ("non-ascii-zone", "http://[fe80::1%ｅ]:8001", "ASCII 밖의 글자. IPv6 영역 이름은 ipaddress 가 그대로 받는다"),
        ("backslash-host", "http://" + MARK + "\\host:8001", "호스트 허용 목록 밖의 글자(\\). boto3 가 값을 담아 터진다"),
        ("underscore-host", "http://a_b:8001", "호스트 허용 목록 밖의 글자(_)"),
        ("bracket-ipv4", "http://[1.2.3.4]:8001", "괄호 안은 IPv6 리터럴만"),
        ("bracket-ipvfuture", "http://[v1.fe]:8001", "urlsplit 은 IPvFuture 를 받는다. 괄호 안은 IPv6 리터럴만"),
    ]
] + [
    dict(id="env-endpoint-ok-ipv6", env=env(SANGTACHI_CP_ENDPOINT="http://[::1]:8001"),
         expect=(CONTROL_PORT, "t1", "ap-northeast-2", "http://[::1]:8001"),
         note="괄호 안의 IPv6 리터럴은 받는다"),
    dict(id="env-region-slash", env=env(AWS_REGION=MARK.lower() + "/secret"), expect="AWS_REGION",
         note="AWS_REGION 은 [a-z0-9-]+ 전체 일치. boto3 는 틀린 리전의 값을 예외에 싣는다"),
    dict(id="env-region-upper", env=env(AWS_REGION="AP-NORTHEAST-2"), expect="AWS_REGION",
         note="대문자는 리전 이름이 아니다"),
]


@pytest.mark.parametrize("case", ENV_CASES, ids=[c["id"] for c in ENV_CASES])
def test_config(case):
    if isinstance(case["expect"], str):
        with pytest.raises(cp_main.ConfigError, match=case["expect"]):
            cp_main.load_config(case["env"])
        return
    cfg = cp_main.load_config(case["env"])
    assert (cfg.port, cfg.table, cfg.region, cfg.endpoint) == case["expect"]


@pytest.mark.parametrize("name", ["SANGTACHI_CP_TABLE", "AWS_REGION"])
def test_main_fails_fast(name, capsys):
    # 7.6 필수 변수가 없으면 시작하지 않는다. 종료 코드 2 와 변수 이름이 있는 한 줄.
    assert cp_main.main(env(**{name: None})) == cp_main.EXIT_CONFIG
    err = capsys.readouterr().err
    assert name in err and err.count("\n") == 1


LEAK_CASES = [
    dict(id="leak-backslash-host", env=env(SANGTACHI_CP_ENDPOINT="http://" + MARK + "\\host:8001"), raises=None,
         note="호스트 허용 목록 밖. 검사가 없으면 Store.from_env 가 URL 을 담은 ValueError 를 던졌다"),
    dict(id="leak-region", env=env(AWS_REGION=MARK + "/secret"), raises=None,
         note="리전 형식 밖. 검사가 없으면 boto3 가 InvalidRegionError 에 값을 실었다"),
    dict(id="leak-store-raises", env=env(), raises=ValueError("bad endpoint " + MARK),
         note="설정 검사를 통과한 뒤 Store.from_env 가 값을 담은 예외를 던진다. 포괄 처리가 고정 문구로 바꾼다"),
]


@pytest.mark.parametrize("case", LEAK_CASES, ids=[c["id"] for c in LEAK_CASES])
def test_main_does_not_leak_config_values(case, capsys, monkeypatch):
    # 7.6 설정 오류는 종료 코드 2 와 고정 문구 한 줄이다. 값과 Traceback 이 출력에 없다.
    from controlplane import store as store_mod

    def boom(*args, **kwargs):
        raise case["raises"]

    if case["raises"] is not None:
        monkeypatch.setattr(store_mod.Store, "from_env", boom)
    assert cp_main.main(case["env"]) == cp_main.EXIT_CONFIG
    out = capsys.readouterr()
    text = out.out + out.err
    assert out.err.startswith("controlplane: 설정 오류: ") and out.err.count("\n") == 1
    assert MARK not in text and MARK.lower() not in text and "Traceback" not in text


def test_build_store_hides_cause(monkeypatch):
    # 포괄 처리는 연쇄 없이 바꾼다. 원래 예외의 문구가 __cause__ 나 표시되는 __context__ 로 남지 않는다.
    from controlplane import store as store_mod

    def boom(*args, **kwargs):
        raise RuntimeError(MARK)

    monkeypatch.setattr(store_mod.Store, "from_env", boom)
    with pytest.raises(cp_main.ConfigError) as info:
        cp_main.build_store(env())
    assert MARK not in str(info.value)
    assert info.value.__cause__ is None and info.value.__suppress_context__


BRACKET_CASES = [
    dict(id="bracket-open", url="http://[", note="닫는 괄호도 호스트도 없음. urlsplit 이 ValueError"),
    dict(id="bracket-word", url="http://[" + MARK + "]", note="괄호 안이 IP 가 아님. ValueError 문구에 값이 실린다"),
    dict(id="bracket-unclosed-v6", url="http://[::1", note="닫는 괄호 없음. urlsplit 이 ValueError"),
]


@pytest.mark.parametrize("case", BRACKET_CASES, ids=[c["id"] for c in BRACKET_CASES])
def test_main_bracket_endpoint(case, capsys):
    # 7.6 엔드포인트의 URL 해석 실패도 설정 오류다. 종료 코드 2, 고정 문구, 값과 traceback 이 출력에 없다.
    assert cp_main.main(env(SANGTACHI_CP_ENDPOINT=case["url"])) == cp_main.EXIT_CONFIG
    out = capsys.readouterr()
    text = out.out + out.err
    assert "SANGTACHI_CP_ENDPOINT" in out.err and out.err.count("\n") == 1
    assert case["url"] not in text and MARK not in text and "Traceback" not in text
    with pytest.raises(cp_main.ConfigError) as info:
        cp_main.load_config(env(SANGTACHI_CP_ENDPOINT=case["url"]))
    assert info.value.__cause__ is None and info.value.__suppress_context__


def test_main_bad_endpoint(capsys):
    # 형식이 틀린 엔드포인트도 종료 코드 2 다. 오류 문구에 값을 싣지 않는다 (값이 이상할 수 있다).
    assert cp_main.main(env(SANGTACHI_CP_ENDPOINT="ftp://" + MARK + ":1")) == cp_main.EXIT_CONFIG
    err = capsys.readouterr().err
    assert "SANGTACHI_CP_ENDPOINT" in err and MARK not in err and err.count("\n") == 1


def test_run_binds_all_ipv4(lines):
    # control_plane.md 3.2 주소 표: 0.0.0.0:8000. 127.0.0.1 이면 인스턴스 밖에서 닿지 않는다.
    # serve 가 끝나면 run 은 exit(0) 을 부른다 (시험은 exit 를 바꿔 끼운다).
    seen = []
    exits = []

    class Fake:
        async def serve(self, host, port, stop):
            seen.append((host, port))

    asyncio.run(cp_main.run(Fake(), CONTROL_PORT, exit=exits.append))
    assert seen == [("0.0.0.0", CONTROL_PORT)] and BIND_HOST == "0.0.0.0"
    assert exits == [0]


# ---------------------------------------------------------------- 멈춤은 처리 중인 dispatch 를 기다리지 않는다

def _free_port() -> int:
    with socket.socket() as s:
        s.bind((HOST, 0))
        return s.getsockname()[1]


class Blackhole:
    """연결을 받고 응답을 아주 천천히 흘리는 TCP 서버. boto3 호출이 여기서 막힌다.

    서버는 DynamoDB 호출에 1초 read 시간 제한을 건다(control_plane.md 7.6). 아무것도 보내지 않으면 1초 뒤
    호출이 끝나 '막힌 dispatch' 가 성립하지 않는다. 그래서 헤더를 보낸 뒤 본문을 0.3초에 한 바이트씩 흘려
    읽기마다의 시간 제한이 걸리지 않게 한다.
    """

    def __init__(self):
        self.sock = socket.socket()
        self.sock.bind((HOST, 0))
        self.sock.listen()
        self.port = self.sock.getsockname()[1]
        self.accepted: list[socket.socket] = []
        self.closed = threading.Event()
        self.thread = threading.Thread(target=self._accept, daemon=True)
        self.thread.start()

    def _accept(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            self.accepted.append(conn)
            threading.Thread(target=self._drip, args=(conn,), daemon=True).start()

    def _drip(self, conn):
        try:
            conn.recv(65536)
            conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/x-amz-json-1.0\r\n"
                         b"Content-Length: 1000000\r\n\r\n")
            while not self.closed.wait(0.3):
                conn.sendall(b" ")
        except OSError:
            return

    def close(self):
        self.closed.set()
        self.sock.close()
        for c in self.accepted:
            c.close()


STOP_S = 5.0  # 멈춤 신호부터 프로세스가 끝나기까지의 상한


def _stop_signal(proc: subprocess.Popen) -> None:
    """Windows 는 Ctrl+Break, 그 밖은 SIGTERM. Windows 의 Ctrl+C 는 새 프로세스 그룹에 보낼 수 없다."""
    if sys.platform == "win32":
        proc.send_signal(signal.CTRL_BREAK_EVENT)
    else:
        proc.send_signal(signal.SIGTERM)


BIND_IN_USE = f"controlplane: 기동 실패: {errno.errorcode[errno.EADDRINUSE]} "  # 7.6 기동 실패 한 줄
LAUNCH_TRIES = 3


def _launch(env: dict, root: Path) -> tuple[subprocess.Popen, list[str], int]:
    """python -m controlplane 을 띄우고 server.started 줄까지 기다린다. (프로세스, 표준 오류 줄, 포트).

    SANGTACHI_CP_PORT 는 0 을 받지 않으므로(7.6) 빈 포트를 골라 놓고 넘긴다. 그 사이 다른 프로세스가 그 포트를
    가져가면 bind 가 실패한다. 종료 코드 1 이고 표준 오류가 BIND_IN_USE 로 시작하는 실패만 다른 포트로 다시 띄운다.
    다른 실패는 그대로 떨어진다.
    """
    flags = subprocess.CREATE_NEW_PROCESS_GROUP if sys.platform == "win32" else 0
    for _ in range(LAUNCH_TRIES):
        port = _free_port()
        proc = subprocess.Popen([sys.executable, "-m", "controlplane"], cwd=root,
                                env=dict(env, SANGTACHI_CP_PORT=str(port)), stderr=subprocess.PIPE,
                                stdout=subprocess.DEVNULL, creationflags=flags)
        err_lines: list[str] = []
        threading.Thread(target=lambda p=proc, out=err_lines: out.extend(
            line.decode("utf-8", "replace").rstrip() for line in p.stderr), daemon=True).start()
        deadline = time.monotonic() + 20
        while not any(line.startswith("INFO server.started ") for line in err_lines):
            if proc.poll() is not None:
                time.sleep(0.2)  # 표준 오류의 나머지를 읽을 틈
                if proc.returncode == cp_main.EXIT_START and any(line.startswith(BIND_IN_USE) for line in err_lines):
                    break  # 진단된 bind 충돌. 다른 포트로 다시
                raise AssertionError(f"기동 전에 끝났다: {err_lines}")
            if time.monotonic() > deadline:
                proc.kill()
                raise AssertionError(f"기동 줄이 나오지 않았다: {err_lines}")
            time.sleep(0.05)
        else:
            started = records(err_lines, "server.started")[0]
            assert started["port"] == str(port)
            return proc, err_lines, port
    raise AssertionError(f"bind 충돌이 {LAUNCH_TRIES}번 났다")


def test_process_stops_without_waiting_for_dispatch(tmp_path):
    """7.6: 멈춤 신호 뒤 처리 중인 boto3 호출을 기다리지 않고 끝난다. 끝나기 전에 카운터 전량을 낸다.

    python -m controlplane 을 서브프로세스로 띄운다. 저장소 엔드포인트는 연결을 받고 답하지 않는 Blackhole 이라
    create_room 의 dispatch 가 응답 본문을 기다리며 막힌다. 자격 증명은 aws_isolation 이 넣은 가짜 값이다.
    """
    hole = Blackhole()
    env = dict(os.environ, SANGTACHI_CP_TABLE="t-stop", AWS_REGION="local-only",
               SANGTACHI_CP_ENDPOINT=f"http://{HOST}:{hole.port}", PYTHONUNBUFFERED="1")
    root = Path(cp_main.__file__).resolve().parents[1]  # 변이 실행기의 복사본이면 그쪽을 띄운다
    proc = None
    client = None
    try:
        proc, err_lines, port = _launch(env, root)
        client = socket.create_connection((HOST, port), timeout=DEADLINE_S)
        client.sendall(http("create_room", {"client_nonce": "d" * 32}))
        deadline = time.monotonic() + 20
        while not hole.accepted:
            assert time.monotonic() < deadline, "dispatch 가 저장소에 닿지 않았다"
            time.sleep(0.05)
        _stop_signal(proc)
        t0 = time.monotonic()
        code = proc.wait(timeout=STOP_S)
        took = time.monotonic() - t0
        time.sleep(0.2)  # 표준 오류의 나머지를 읽을 틈. 프로세스는 이미 끝났다
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        if client is not None:
            client.close()
        hole.close()
    assert code == 0 and took < STOP_S
    names = [r["name"] for r in records(err_lines, "counter")]
    assert names == list(COUNTER_NAMES), err_lines


def test_launch_retries_on_bind_conflict(monkeypatch):
    # 고른 포트를 다른 프로세스가 먼저 가져간 경우. 첫 포트는 이미 리슨 중이고 다음 포트는 비어 있다.
    busy = socket.socket()
    busy.bind(("0.0.0.0", 0))
    busy.listen()
    ports = iter([busy.getsockname()[1], _free_port()])
    monkeypatch.setattr(sys.modules[__name__], "_free_port", lambda: next(ports))
    env = dict(os.environ, SANGTACHI_CP_TABLE="t-launch", AWS_REGION="local-only")
    proc = None
    try:
        proc, err_lines, port = _launch(env, Path(cp_main.__file__).resolve().parents[1])
        assert port != busy.getsockname()[1]
    finally:
        if proc is not None:
            proc.kill()
            proc.wait()
        busy.close()


def test_launch_retries_only_on_bind_conflict(tmp_path):
    # _launch 의 재시도 조건 자체를 본다. 설정 오류로 끝나는 프로세스는 다시 띄우지 않고 그대로 떨어진다.
    env = dict(os.environ, AWS_REGION="local-only")
    env.pop("SANGTACHI_CP_TABLE", None)
    with pytest.raises(AssertionError, match="기동 전에 끝났다"):
        _launch(env, Path(cp_main.__file__).resolve().parents[1])

def test_start_failure_port_in_use():
    """7.6 기동 실패: 설정 검사 뒤 리슨이 실패하면 종료 코드 1, 고정 문구 한 줄, OS 오류 이름만. 값과 Traceback 없음.

    포트를 실제로 점유하고 python -m controlplane 을 띄운다.
    """
    busy = socket.socket()
    busy.bind(("0.0.0.0", 0))
    busy.listen()
    port = busy.getsockname()[1]
    env = dict(os.environ, SANGTACHI_CP_TABLE="t-" + MARK.lower(), AWS_REGION="local-only",
               SANGTACHI_CP_PORT=str(port), SANGTACHI_CP_ENDPOINT="http://127.0.0.1:9")
    try:
        proc = subprocess.run([sys.executable, "-m", "controlplane"], cwd=Path(cp_main.__file__).resolve().parents[1],
                              env=env, capture_output=True, timeout=30)
    finally:
        busy.close()
    err = proc.stderr.decode("utf-8", "replace").replace("\r\n", "\n")  # Windows 의 텍스트 모드 줄 끝
    assert proc.returncode == cp_main.EXIT_START
    assert err == f"controlplane: 기동 실패: {errno.errorcode[errno.EADDRINUSE]} (control_plane.md 7.6)\n"
    assert str(port) not in err and MARK.lower() not in err and "Traceback" not in err
    assert proc.stdout == b""


def test_start_failure_signal_setup(monkeypatch, capsys):
    # 신호 처리 설정이 값을 담은 OSError 로 실패한다. 종료 코드 1, 오류 이름(EINVAL)만.
    def boom(loop, stop):
        raise OSError(errno.EINVAL, "signal setup failed " + MARK)

    monkeypatch.setattr(cp_main, "install_stop_signals", boom)
    assert cp_main.main(env(SANGTACHI_CP_PORT=str(_free_port()))) == cp_main.EXIT_START
    out = capsys.readouterr()
    assert out.err == "controlplane: 기동 실패: EINVAL (control_plane.md 7.6)\n"
    assert MARK not in out.out + out.err and "Traceback" not in out.out + out.err

def test_start_failure_store_import(monkeypatch, capsys):
    # store 의 import(boto3 를 끌어온다)가 값을 담은 RuntimeError 로 실패한다. 리슨 전 경계 안이라 종료 코드 1, 이름만.
    import types

    fake = types.ModuleType("controlplane.store")

    def broken(name):
        raise RuntimeError("import failed " + MARK)

    fake.__getattr__ = broken
    monkeypatch.setitem(sys.modules, "controlplane.store", fake)
    assert cp_main.main(env(SANGTACHI_CP_PORT=str(_free_port()))) == cp_main.EXIT_START
    out = capsys.readouterr()
    assert out.err == "controlplane: 기동 실패: RuntimeError (control_plane.md 7.6)\n"
    assert MARK not in out.out + out.err and "Traceback" not in out.out + out.err

# 하위 프로세스에서 controlplane.clock 의 import 를 값을 담은 OSError 로 바꾸고 python -m controlplane 과 같이 띄운다.
CLOCK_FAILS = (
    "import errno, runpy, sys, types\n"
    "fake = types.ModuleType('controlplane.clock')\n"
    "def broken(name):\n"
    "    raise OSError(errno.EIO, 'clock import failed {mark}')\n"
    "fake.__getattr__ = broken\n"
    "sys.modules['controlplane.clock'] = fake\n"
    "runpy.run_module('controlplane', run_name='__main__', alter_sys=True)\n"
)


def test_start_failure_clock_import():
    """7.6: __main__ 의 맨 위에는 표준 라이브러리 import 만 있다. controlplane 의 import(여기서는 clock 이 모듈 수준에서
    secrets.token_hex 를 부른다)는 main 의 경계 안에서 일어난다. 그 import 가 값을 담은 OSError 로 실패해도 종료 코드 1,
    한 줄, 이름(EIO)만이다. 맨 위에서 import 하는 구현은 traceback 을 낸다. 그래서 하위 프로세스로 모듈 맨 위부터 실행한다."""
    env = dict(os.environ, SANGTACHI_CP_TABLE="t-" + MARK.lower(), AWS_REGION="local-only",
               SANGTACHI_CP_PORT=str(_free_port()))
    proc = subprocess.run([sys.executable, "-c", CLOCK_FAILS.format(mark=MARK)],
                          cwd=Path(cp_main.__file__).resolve().parents[1], env=env, capture_output=True, timeout=30)
    err = proc.stderr.decode("utf-8", "replace").replace("\r\n", "\n")
    assert proc.returncode == cp_main.EXIT_START, err
    assert err == "controlplane: 기동 실패: EIO (control_plane.md 7.6)\n"
    assert MARK not in err and MARK.lower() not in err and "Traceback" not in err


@pytest.mark.parametrize("exc,name", [(OSError(errno.EADDRINUSE, "x"), errno.errorcode[errno.EADDRINUSE]),
                                      (OSError("no errno"), "OSError"), (OSError(987654, "x"), "OSError"),
                                      (RuntimeError(MARK), "RuntimeError")],
                         ids=["known-errno", "no-errno", "unknown-errno", "not-oserror"])
def test_error_name(exc, name):
    # 오류 이름은 errno.errorcode 의 이름이다. 모르면 OSError, OSError 가 아니면 클래스 이름. 예외 문자열은 쓰지 않는다.
    assert cp_main.error_name(exc) == name



def test_defaults():
    # 3.4 읽기·송신 시간 제한 SERVER_READ_TIMEOUT_S, 7.2 MAX_INFLIGHT, 7.5 60초 주기.
    import inspect

    server = Server(Spy())
    assert (server.read_timeout_s, server.write_timeout_s, server.max_inflight, server.linger_s) == (
        SERVER_READ_TIMEOUT_S, SERVER_READ_TIMEOUT_S, MAX_INFLIGHT, LINGER_S)
    assert server.max_rejecting == MAX_REJECTING == 64
    assert server.pool._max_workers == MAX_INFLIGHT
    assert inspect.signature(Server.serve).parameters["counter_period_s"].default == COUNTER_PERIOD_S == 60


# ---------------------------------------------------------------- 실제 ops 와 DynamoDB local

@pytest.mark.store
def test_smoke_with_real_ops(store, lines):
    """수락 루프부터 ops.Service, store.py, DynamoDB local 까지 한 바퀴. create_room 과 join_room.

    7.5: 어느 줄에도 room_id, peer_token, client_nonce 가 나오지 않는다.
    """
    from controlplane.ops import Service

    service = Service(store)
    host_nonce, join_nonce = "a" * 32, "b" * 32

    async def go():
        server = Server(service.dispatch, service_counters=service.counters)
        async with Running(server) as running:
            created = parse(await running.exchange(http("create_room", {"client_nonce": host_nonce})))
            room_id = created[1].get("room_id", "")
            joined = parse(await running.exchange(http("join_room", {"room_id": room_id,
                                                                     "client_nonce": join_nonce})))
        return created, joined

    created, joined = run(go())
    assert created[0] == 200 and created[1]["ok"] is True and created[1]["virtual_ip"] == "10.100.0.1"
    assert joined[0] == 200 and joined[1]["ok"] is True and joined[1]["room_id"] == created[1]["room_id"]
    assert [(r["op"], r["status"]) for r in records(lines, "http.request")] == [("create_room", "200"),
                                                                                 ("join_room", "200")]
    secrets = [created[1]["room_id"], created[1]["peer_token"], joined[1]["peer_token"], host_nonce, join_nonce]
    for line in lines:
        assert not any(s in line for s in secrets), line
    assert len(batches(lines)) == 1


# ---------------------------------------------------------------- lingering close (RFC 9112 9.6)
#
# 요청을 다 읽지 않은 채 답하는 경로는 응답 뒤에 쓰기 쪽을 먼저 닫고(FIN) 남은 입력을 읽어 버린다. 아니면
# 서버가 닫은 뒤 도착한 바이트에 OS 가 RST 로 답하고, 상대는 이미 받은 응답을 읽기 전에 연결 오류를 받는다.
# 이 기기(Windows)에서 링거 없이 돌려 503 과 413 이 20번 중 20번 WinError 10053 으로 사라지는 것을 봤다.
#
# 변이 판정은 통제된 writer 시험(LINGER_UNIT_CASES, test_linger_timeout_value)이 한다. 늦은 바이트는 write_eof
# 가 불린 뒤에야 reader 에 들어가므로 순서가 결정적이다. 소켓 통합 시험(LINGER_CASES, LINGER_RULE_CASES)은 실제
# 소켓에서 같은 동작을 한 번 더 본다. 시간에 기대므로 kills 에 걸지 않는다.

HEAD_5000 = b"POST /v1/get_peers HTTP/1.1\r\nContent-Length: 5000\r\n\r\n"
HEAD_OVER = b"POST /v1/get_peers HTTP/1.1\r\nX-Pad: " + b"a" * 2100 + b"\r\n"
BAD_BODY = b"POST /v1/get_peers HTTP/1.1\r\nContent-Length: 2\r\n\r\n[]"
LINGER_UNIT_S = 30.0  # 통제된 시험의 링거 시간. UNIT_BOUND_S 보다 길다. 시간 상한에 기대면 시험이 떨어진다

LINGER_UNIT_CASES = [
    dict(id="unit-503", max_inflight=0, first=b"", late=http("get_peers", {"room_id": "abc"}), status=503,
         events=["write", "drain", "write_eof", "close"],
         note="7.2 unavailable 은 요청을 읽지 않고 답한다. 응답 -> 쓰기 쪽 닫기 -> 늦은 바이트를 EOF 까지 버림 -> 닫기"),
    dict(id="unit-413", max_inflight=MAX_INFLIGHT, first=HEAD_5000, late=b"x" * 5000, status=413,
         events=["write", "drain", "write_eof", "close"],
         note="3.3 표 8행: 본문을 읽지 않고 413. 뒤에 온 본문을 버리며 읽은 뒤 닫는다"),
    dict(id="unit-431", max_inflight=MAX_INFLIGHT, first=HEAD_OVER, late=b"X-More: b\r\n\r\n", status=431,
         events=["write", "drain", "write_eof", "close"],
         note="3.3 표 10행: 머리 상한 초과 431. 머리 나머지를 버리며 읽은 뒤 닫는다"),
    dict(id="unit-400", max_inflight=MAX_INFLIGHT, first=BAD_BODY, late=b"junk" * 16, status=400,
         events=["write", "drain", "write_eof", "close"],
         note="3.3 그 밖의 파싱 실패. 본문 뒤에 더 온 바이트를 버리며 읽은 뒤 닫는다"),
    dict(id="unit-full-read", max_inflight=MAX_INFLIGHT, first=http("get_peers", {"room_id": "abc"}), late=None,
         status=200, events=["write", "drain", "close"],
         note="요청을 다 읽고 답한 연결은 링거하지 않는다. 처리 중 자리를 링거 시간만큼 붙들지 않는다"),
]


@pytest.mark.parametrize("case", LINGER_UNIT_CASES, ids=[c["id"] for c in LINGER_UNIT_CASES])
def test_linger_controlled(case, lines):
    def late():
        if case["late"] is not None:
            writer.reader.feed_data(case["late"])
            writer.reader.feed_eof()

    writer = FakeWriter(on_eof=late)
    reader = drive(Server(Spy(), max_inflight=case["max_inflight"], linger_s=LINGER_UNIT_S), case["first"], writer)
    assert writer.events == case["events"]
    assert writer.data.startswith(b"HTTP/1.1 %d " % case["status"])
    if case["late"] is not None:
        assert reader.at_eof()  # 늦은 바이트를 EOF 까지 다 읽어 버렸다


def test_linger_byte_cap_controlled(lines):
    # 링거는 LINGER_MAX_BYTES 를 넘게 읽지 않는다. 그만큼 읽으면 시간 상한을 기다리지 않고 닫는다.
    sent = 20000

    def late():
        writer.reader.feed_data(b"z" * sent)  # EOF 를 넣지 않는다. 계속 보내는 상대

    writer = FakeWriter(on_eof=late)
    reader = drive(Server(Spy(), max_inflight=0, linger_s=LINGER_UNIT_S), b"", writer)
    assert writer.events == ["write", "drain", "write_eof", "close"]

    async def rest():
        return await reader.read(sent)

    assert len(run(rest())) == sent - LINGER_MAX_BYTES


@pytest.mark.parametrize("linger_s", [LINGER_S, 0.25], ids=["default", "injected"])
def test_linger_timeout_value(linger_s, lines):
    # 링거의 시간 상한은 linger_s(기본 LINGER_S) 다. 시간 제한 장치를 주입해 넘긴 값을 직접 본다.
    seen = []

    def timeout(value):
        seen.append(value)
        return asyncio.timeout(0.01)  # 실제로는 곧바로 끝낸다. 상대가 닫지 않는 경우다

    kwargs = {} if linger_s == LINGER_S else {"linger_s": linger_s}
    writer = FakeWriter()
    drive(Server(Spy(), max_inflight=0, timeout=timeout, **kwargs), b"", writer)
    assert seen == [linger_s]
    assert writer.events == ["write", "drain", "write_eof", "close"]


# 소켓 통합 시험. linger_s 를 3초로 늘려 "곧바로" 와 "링거 끝" 을 가른다.
LINGER_TEST_S = 3.0
QUICK_S = 1.5   # 허용 오차. 링거를 기다리지 않았다는 판정의 상한이다. LINGER_TEST_S 의 절반이다


def _read_response(s: socket.socket) -> bytes:
    """응답 하나를 Content-Length 까지 읽는다. EOF 를 기다리지 않는다."""
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = s.recv(65536)
        if not chunk:
            return data
        data += chunk
    head, _, body = data.partition(b"\r\n\r\n")
    length = int(head.split(b"Content-Length: ")[1].split(b"\r\n")[0])
    while len(body) < length:
        chunk = s.recv(65536)
        if not chunk:
            break
        body += chunk
    return head + b"\r\n\r\n" + body


def late_client(port: int, first: bytes, later: bytes) -> tuple[bytes, str | None, float]:
    """first 를 보내고 응답을 다 받은 뒤에 later 를 보내고 EOF 까지 읽는다.
    (응답, 연결 오류, later 를 보낸 뒤 EOF 까지 초). 늦은 바이트는 응답이 도착한 뒤에 나간다."""
    with socket.create_connection((HOST, port), timeout=DEADLINE_S) as s:
        try:
            if first:
                s.sendall(first)
            data = _read_response(s)
            s.sendall(later)
            t0 = time.monotonic()
            while s.recv(65536):
                pass
            return data, None, time.monotonic() - t0
        except (ConnectionError, TimeoutError) as exc:
            return b"", type(exc).__name__, 0.0


LINGER_CASES = [
    dict(id="linger-503-late-request", max_inflight=0, first=b"", later=http("get_peers", {"room_id": "abc"}),
         status=503, note="7.2 unavailable 은 요청을 읽지 않고 답한다. 그 뒤에 도착한 요청 바이트가 503 을 지우면 안 된다"),
    dict(id="linger-413-late-body", max_inflight=MAX_INFLIGHT, first=HEAD_5000, later=b"x" * 5000, status=413,
         note="3.3 표 8행: 본문을 읽지 않고 413. 본문을 보내는 클라이언트도 413 을 읽는다"),
    dict(id="linger-431-late-head", max_inflight=MAX_INFLIGHT, first=HEAD_OVER, later=b"X-More: b\r\n\r\n", status=431,
         note="3.3 표 10행: 머리 상한 초과 431. 머리 나머지를 보내는 클라이언트도 431 을 읽는다"),
    dict(id="linger-400-late-bytes", max_inflight=MAX_INFLIGHT, first=BAD_BODY, later=b"junk" * 16, status=400,
         note="3.3 그 밖의 파싱 실패. 본문 뒤에 더 온 바이트가 400 을 지우면 안 된다"),
]


@pytest.mark.parametrize("case", LINGER_CASES, ids=[c["id"] for c in LINGER_CASES])
def test_linger_keeps_response(case, lines):
    async def go():
        server = Server(Spy(), max_inflight=case["max_inflight"], linger_s=LINGER_TEST_S)
        async with Running(server) as running:
            return await asyncio.to_thread(late_client, running.port, case["first"], case["later"])

    data, error, to_eof = run(go())
    assert error is None
    status, payload = parse(data)
    assert status == case["status"] and payload["ok"] is False
    assert to_eof < QUICK_S  # 쓰기 쪽을 먼저 닫았다. 링거가 끝나기를 기다리지 않고 EOF 가 온다


def _hold_after_answer(port: int, raw: bytes, extra: bytes) -> bytes:
    """raw 를 보내고 응답(EOF 까지)을 받은 뒤 extra 를 보내고 연결을 연 채 둔다. 받은 바이트."""
    s = socket.create_connection((HOST, port), timeout=DEADLINE_S)
    s.sendall(raw)
    data = b""
    while chunk := s.recv(65536):
        data += chunk
    if extra:
        s.sendall(extra)
    _held.append(s)
    return data


_held: list[socket.socket] = []

LINGER_RULE_CASES = [
    dict(id="linger-not-on-full-read", linger_s=LINGER_TEST_S, raw=http("get_peers", {"room_id": "abc"}), extra=b"",
         note="요청을 다 읽고 답한 연결은 링거하지 않는다. 처리 중 자리가 QUICK_S 안에 풀린다"),
    dict(id="linger-time-cap", linger_s=SHORT_S, raw=http("get_peers", {"room_id": "abc"}).replace(b"POST", b"GET"),
         extra=b"", note="링거는 시간 상한에서 끝난다. 0.3초를 주입하고 QUICK_S(1.5초) 안에 끝나는지만 본다. "
                         "허용 오차가 1.2초라 상한을 몇 배로 늘린 구현은 여기서 걸리지 않는다. 그것은 "
                         "test_linger_timeout_value 가 본다"),
    dict(id="linger-byte-cap", linger_s=LINGER_TEST_S,
         raw=http("get_peers", {"room_id": "abc"}).replace(b"POST", b"GET"), extra=b"z" * 20000,
         note="링거는 LINGER_MAX_BYTES 를 넘게 읽지 않는다. 계속 보내는 상대 때문에 시간 상한까지 붙들리지 않는다"),
]


@pytest.mark.parametrize("case", LINGER_RULE_CASES, ids=[c["id"] for c in LINGER_RULE_CASES])
def test_linger_bounds(case, lines):
    async def go():
        server = Server(Spy(), linger_s=case["linger_s"])
        async with Running(server) as running:
            data = await asyncio.to_thread(_hold_after_answer, running.port, case["raw"], case["extra"])
            await until(lambda: server.inflight == 0, "링거 끝", timeout=QUICK_S)
            return data

    try:
        data = run(go())
    finally:
        while _held:
            _held.pop().close()
    assert data.startswith(b"HTTP/1.1 ")


# ---------------------------------------------------------------- server.started (7.5)

def test_started_line(lines):
    """7.5 server.started: 리슨이 성공한 뒤 INFO 한 줄. bind 는 주소, port 는 실제로 bind 한 포트다."""
    async def go():
        async with Running(Server(Spy())) as running:
            return running.port

    port = run(go())
    assert records(lines, "server.started") == [{"level": "INFO", "bind": HOST, "port": str(port)}]
    assert lines[0].startswith("INFO server.started ")


def test_no_started_line_when_bind_fails(lines):
    # 리슨이 실패하면 기동 줄이 없다. 이미 쓰는 포트에 다시 bind 한다.
    async def go():
        busy = await asyncio.start_server(lambda r, w: None, HOST, 0)
        try:
            port = busy.sockets[0].getsockname()[1]
            with pytest.raises(OSError):
                await Server(Spy()).serve(HOST, port, asyncio.Event())
        finally:
            busy.close()

    run(go())
    assert records(lines, "server.started") == []
