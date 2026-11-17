"""입력 상한의 케이스 표 (architecture.md 3.4 입력 상한, 설정과 배포의 로그).

진짜 수락 루프(controlplane.server.Server)를 루프백 포트 0 에 띄우고 원시 HTTP 를 보낸다. dispatch 는 가짜다.
"""

import asyncio
import json

import pytest

from controlplane import log
from controlplane.errors import OpError
from telemetry import server as tm


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


def http(path="/v1/report", body=b"{}", extra=b""):
    return (b"POST " + path.encode() + b" HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
            + b"Content-Length: " + str(len(body)).encode() + b"\r\nConnection: close\r\n" + extra + b"\r\n" + body)


async def send(port, raw):
    reader, writer = await asyncio.open_connection("127.0.0.1", port)
    writer.write(raw)
    await writer.drain()
    out = await reader.read()
    writer.close()
    head, _, body = out.partition(b"\r\n\r\n")
    return int(head.split(b" ")[1]), json.loads(body)


class Spy:
    """dispatch. body 에 bad 가 있으면 bad_request."""

    def __init__(self):
        self.calls = 0

    def __call__(self, req):
        self.calls += 1
        if "bad" in req.body:
            raise OpError("bad_request", "x")
        return {}


async def running(server, fn):
    stop = asyncio.Event()
    ready = asyncio.get_running_loop().create_future()
    task = asyncio.create_task(server.serve("127.0.0.1", 0, stop, on_ready=ready.set_result))
    try:
        return await fn(await ready)
    finally:
        stop.set()
        await task


@pytest.fixture
def lines():
    got = []
    log.set_sink(got.append)
    yield got
    log.set_sink(None)


def make(spy=None, *, global_clock=None, rate_clock=None, **kw):
    spy = spy or Spy()
    g = tm.GlobalRate(clock=global_clock or Clock())
    return tm.build_server(spy, lambda: {"metrics_put_error": 2}, global_rate=g,
                           rate_clock=rate_clock or Clock(), **kw), spy, g


def test_source_budget_counts_every_request_and_refills(lines):
    rc, gc = Clock(), Clock()
    server, spy, _ = make(global_clock=gc, rate_clock=rc)

    async def go(port):
        out = []
        for i in range(30):
            gc.t += 1  # 전체 상한은 따로 본다. 여기서는 늘 남게 한다
            out.append(await send(port, http(body=b'{"bad":1}' if i % 2 else b"{}")))
        gc.t += 1
        out.append(await send(port, http()))
        rc.t += 2  # 2초에 1개
        gc.t += 1
        out.append(await send(port, http()))
        gc.t += 1
        out.append(await send(port, http()))
        return out

    out = asyncio.run(running(server, go))
    assert [s for s, _ in out[:30]] == [200, 400] * 15  # 성공과 bad_request 둘 다 쓴다
    assert out[30] == (429, {"ok": False, "error": "rate_limited", "message": "rate_limited"})
    assert out[31][0] == 200
    assert out[32][1]["error"] == "rate_limited"
    assert spy.calls == 31


def test_parse_failure_does_not_spend():
    rc, gc = Clock(), Clock()
    server, spy, _ = make(global_clock=gc, rate_clock=rc)

    async def go(port):
        for _ in range(40):
            assert (await send(port, http(path="/v1/get_peers")))[1]["error"] == "unknown_op"
        gc.t += 1
        return await send(port, http())

    assert asyncio.run(running(server, go))[0] == 200


def test_global_rate_unavailable(lines):
    gc = Clock()
    server, spy, g = make(global_clock=gc)

    async def go(port):
        out = [await send(port, http()) for _ in range(6)]
        gc.t += 0.2  # 초당 5개. 0.2초에 1개
        out.append(await send(port, http()))
        out.append(await send(port, http()))
        return out

    out = asyncio.run(running(server, go))
    assert [s for s, _ in out] == [200] * 5 + [503, 200, 503]
    assert out[5][1]["error"] == "unavailable"
    assert spy.calls == 6
    assert server.counter_values()["unavailable"] == 2


GLOBAL_CASES = [
    # (id, 시각 목록, 기대 결과)
    ("burst-5", [0] * 6, [True] * 5 + [False]),
    ("refill-continuous", [0] * 5 + [0.125, 0.25, 0.375, 0.5], [True] * 5 + [False, True, False, True]),
    ("cap-5", [0] * 5 + [10] * 6, [True] * 10 + [False]),
    ("rejected-does-not-delay", [0] * 5 + [0.125] * 3 + [0.25], [True] * 5 + [False] * 3 + [True]),
]


@pytest.mark.parametrize("times,expected", [c[1:] for c in GLOBAL_CASES], ids=[c[0] for c in GLOBAL_CASES])
def test_global_rate_table(times, expected):
    c = Clock()
    c.t = base = 0.0
    g = tm.GlobalRate(clock=c)
    got = []
    for t in times:
        c.t = base + t
        got.append(g.take())
    assert got == expected
    assert g.rejected == expected.count(False)


def test_inflight_unavailable():
    server, spy, _ = make(max_inflight=0)

    async def go(port):
        return await send(port, http())

    assert asyncio.run(running(server, go)) == (503, {"ok": False, "error": "unavailable", "message": "unavailable"})
    assert spy.calls == 0
    assert server.counter_values()["unavailable"] == 1


SIZE_CASES = [
    ("body-4096", http(body=b'{"a":"' + b"x" * 4088 + b'"}'), 200, None),
    ("body-4097", http(body=b'{"a":"' + b"x" * 4089 + b'"}'), 413, "too_large"),
    ("header-over-2048", http(extra=b"X-Pad: " + b"x" * 2048 + b"\r\n"), 431, "too_large"),
    ("wrong-op", http(path="/v1/create_room"), 404, "unknown_op"),
    ("get", http().replace(b"POST", b"GET", 1), 405, "method_not_allowed"),
]


@pytest.mark.parametrize("raw,status,error", [c[1:] for c in SIZE_CASES], ids=[c[0] for c in SIZE_CASES])
def test_size_and_path(raw, status, error):
    server, spy, _ = make()

    async def go(port):
        return await send(port, raw)

    s, body = asyncio.run(running(server, go))
    assert s == status
    assert body.get("error") == error


def test_log_has_no_source_ip_and_counters(lines):
    server, spy, _ = make()

    async def go(port):
        return await send(port, http())

    asyncio.run(running(server, go))
    req = [l for l in lines if " http.request " in l]
    assert req == [req[0]] and req[0].startswith("INFO http.request op=report status=200 error=- ms=")
    assert not any("127.0.0.1" in l or "src=" in l for l in lines if " server.started " not in l)
    assert any(l.startswith("INFO server.started bind=127.0.0.1 port=") for l in lines)
    counters = [l.split(" ", 2)[2] for l in lines if " counter " in l]
    assert counters == [f"name={n} value={2 if n == 'metrics_put_error' else 0}" for n in tm.COUNTER_NAMES]
