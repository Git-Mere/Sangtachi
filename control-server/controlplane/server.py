"""HTTP 부분집합 파싱, 응답 직렬화, 속도 제한 표, 요청 처리 한 바퀴.

출처는 control_plane.md 3.3 HTTP 부분집합, 3.4 서버 쪽 시간 제한, 6.4 속도 제한, 7.2 요청 처리
한 바퀴다. 케이스 표는 tests/test_http.py 와 tests/test_rate_limit.py 에 있다.

수락 루프와 MAX_INFLIGHT 는 아직 여기 없다. 이 파일은 연결 하나에서 응답 바이트를 만들기까지다.
"""

from __future__ import annotations

import asyncio
import json
import re
import time
from collections import Counter
from contextlib import AbstractAsyncContextManager
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from http import HTTPStatus
from typing import Callable, Mapping

from .constants import (
    MAX_BODY_BYTES,
    MAX_HEADER_BYTES,
    MAX_INFLIGHT,
    MAX_REJECTING,
    LINGER_MAX_BYTES,
    LINGER_S,
    MAX_RATE_ENTRIES,
    OPS,
    RATE_LIMIT_BUCKET,
    RATE_LIMIT_REFILL_PER_MIN,
    SERVER_READ_TIMEOUT_S,
)
from .clock import FALLBACK_COUNTER
from .errors import OpError
from . import log


@dataclass(frozen=True)
class Request:
    op: str
    body: dict


# ---------------------------------------------------------------- 3.3 파싱

_PATHS = {("/v1/" + op).encode("ascii"): op for op in OPS}
# 메서드 SP 대상 SP HTTP/1.1. 토큰은 공백과 제어 문자가 없는 ASCII 다. 탭과 공백 둘은 여기서 걸린다.
_REQUEST_LINE = re.compile(rb"([!-~]+) ([!-~]+) HTTP/1\.1")
# 3.3 요청 형식에 있는 헤더와 Transfer-Encoding. 이 이름이 두 번 오면 거부한다. 나머지는 무시한다.
_TABLE_HEADERS = frozenset({b"host", b"content-type", b"content-length", b"connection", b"transfer-encoding"})
_DIGITS = re.compile(rb"[0-9]+")
_HEAD_END = b"\r\n\r\n"


class Closed(Exception):
    """상대가 요청을 다 보내기 전에 연결을 닫았다. 응답하지 않는다."""


def _bad(message: str) -> OpError:
    # message 는 고정 문자열이다. 요청 바이트를 넣지 않는다 (3.3, 4.1).
    return OpError("bad_request", message)


async def _read_head(reader: asyncio.StreamReader) -> tuple[bytes, bytes, bool]:
    """요청 줄과 헤더를 빈 줄까지 읽는다. (머리, 그 뒤에 이미 받은 바이트, 상한 초과) 를 돌려준다.

    MAX_HEADER_BYTES 를 넘겨 읽지 않는다. 머리 길이는 빈 줄의 CRLF 까지 센다.
    """
    buf = b""
    while True:
        end = buf.find(_HEAD_END)
        if end >= 0:
            return buf[: end + 4], buf[end + 4 :], False
        if len(buf) >= MAX_HEADER_BYTES:
            return buf, b"", True
        chunk = await reader.read(MAX_HEADER_BYTES - len(buf))
        if not chunk:
            raise Closed
        buf += chunk


def _check_request_line(line: bytes) -> str:
    """검사 순서 1~3. 연산 이름을 돌려준다."""
    m = _REQUEST_LINE.fullmatch(line)
    if m is None:
        raise _bad("bad request line")
    method, target = m.groups()
    # 절대 형식, 끝에 붙은 /, 퍼센트 인코딩은 바이트 형식 위반이다 (1).
    if not target.startswith(b"/") or target.endswith(b"/") or b"%" in target:
        raise _bad("bad request line")
    _check_method(method)
    op = _check_path(target)
    return op


def _check_method(method: bytes) -> None:
    if method != b"POST":
        raise OpError("method_not_allowed")


def _check_path(target: bytes) -> str:
    op = _PATHS.get(target)
    if op is None:
        raise OpError("unknown_op")
    return op


def _parse_headers(lines: list[bytes]) -> dict[bytes, bytes]:
    """검사 순서 5. 표에 있는 헤더만 소문자 이름으로 모은다. 값은 콜론 뒤 바이트 그대로다."""
    seen: dict[bytes, bytes] = {}
    for line in lines:
        if line[:1] in (b" ", b"\t"):
            raise _bad("obs-fold")
        name, sep, value = line.partition(b":")
        if not sep:
            raise _bad("bad header line")
        if name[-1:] in (b" ", b"\t"):
            raise _bad("whitespace before colon")  # RFC 9112 5.1
        key = name.lower()
        if key in _TABLE_HEADERS:
            if key in seen:
                raise _bad("duplicate header")
            seen[key] = value
    return seen


def _parse_length(raw: bytes) -> int:
    """Content-Length 값. 콜론 뒤 공백 하나를 떼고 ^[0-9]+$ 만 받는다. int() 를 쓰지 않는다."""
    value = raw[1:] if raw.startswith(b" ") else raw
    if _DIGITS.fullmatch(value) is None:
        raise _bad("bad content-length")
    n = 0
    for d in value:
        n = n * 10 + (d - 0x30)
        if n > MAX_BODY_BYTES:
            break  # 상한을 넘었으면 더 셀 필요가 없다
    return n


def _reject_constant(name: str) -> None:
    raise ValueError("not json")  # NaN, Infinity 는 JSON 이 아니다


def _object_no_dup(pairs: list[tuple[str, object]]) -> dict:
    """객체 안의 중복 키를 거부한다. 중첩 객체마다 불린다."""
    obj = dict(pairs)
    if len(obj) != len(pairs):
        raise ValueError("duplicate key")
    return obj


def _parse_body(body: bytes) -> dict:
    """검사 순서 9."""
    try:
        obj = json.loads(body.decode("utf-8"), parse_constant=_reject_constant,
                         object_pairs_hook=_object_no_dup)
    except (UnicodeDecodeError, ValueError):
        raise _bad("body is not a utf-8 json object") from None
    if not isinstance(obj, dict):
        raise _bad("body is not a utf-8 json object")
    return obj


async def read_request(reader: asyncio.StreamReader) -> Request:
    """3.3 검사 순서 1~9 를 그대로 돈다. 실패는 OpError, 상대가 닫으면 Closed.

    시간 제한(검사 순서 8, 3.4)은 부르는 쪽이 이 코루틴 전체에 건다.
    """
    head, rest, over = await _read_head(reader)
    line_end = head.find(b"\r\n")
    if line_end < 0:
        # 요청 줄이 상한 안에서 끝나지 않았다. 1~3 을 판정할 수 없다.
        raise OpError("too_large", "header too large", 431)
    op = _check_request_line(head[:line_end])
    if over:
        raise OpError("too_large", "header too large", 431)
    headers = _parse_headers(head[:-4].split(b"\r\n")[1:])
    if b"transfer-encoding" in headers:
        raise _bad("transfer-encoding")
    raw_length = headers.get(b"content-length")
    if raw_length is None:
        raise OpError("length_required")
    length = _parse_length(raw_length)
    if length > MAX_BODY_BYTES:
        raise OpError("too_large", "body too large", 413)
    body = rest[:length]
    if len(body) < length:
        try:
            body += await reader.readexactly(length - len(body))
        except asyncio.IncompleteReadError:
            raise Closed from None
    return Request(op, _parse_body(body))


# ---------------------------------------------------------------- 3.3 응답

def response(status: int, payload: dict) -> bytes:
    body = json.dumps(payload, separators=(",", ":")).encode("ascii")
    head = (
        f"HTTP/1.1 {status} {HTTPStatus(status).phrase}\r\n"
        "Content-Type: application/json\r\n"
        f"Content-Length: {len(body)}\r\n"
        "Connection: close\r\n"
        "\r\n"
    )
    return head.encode("ascii") + body


def error_response(exc: OpError) -> bytes:
    return response(exc.status, exc.envelope())


# ---------------------------------------------------------------- 6.4 속도 제한

REFILL_INTERVAL_S = 60 / RATE_LIMIT_REFILL_PER_MIN  # 6초에 1개
COUNTED_ERRORS = frozenset({"room_not_found", "room_expired", "unauthorized"})


class RateTable:
    """출발지 IP -> [토큰, 마지막 보충 시각]. clock 은 초 단위 단조 시계다."""

    def __init__(self, clock: Callable[[], float] = time.monotonic, counters: Counter | None = None) -> None:
        self._clock = clock
        self._entries: dict[str, list] = {}
        self.counters = counters if counters is not None else Counter()

    def __len__(self) -> int:
        return len(self._entries)

    def peek(self, src: str) -> int | None:
        """저장된 토큰 수. 항목이 없으면 None. 보충하지 않는다 (시험용)."""
        e = self._entries.get(src)
        return None if e is None else e[0]

    def _refill(self, src: str, now: float) -> list | None:
        """연속 보충. 기준 시각은 보충이 일어난 시각이고, 상한에 닿으면 항목을 지운다."""
        e = self._entries.get(src)
        if e is None:
            return None
        steps = int((now - e[1]) // REFILL_INTERVAL_S)
        if steps > 0:
            e[0] += steps
            e[1] += steps * REFILL_INTERVAL_S
        if e[0] >= RATE_LIMIT_BUCKET:  # 상한에 닿은 항목은 없는 것과 같다. 그래서 상한을 따로 자르지 않는다
            del self._entries[src]
            return None
        return e

    def exhausted(self, src: str) -> bool:
        """조회만 한다. 항목을 만들지 않고 보충 기준 시각을 바꾸지 않는다."""
        now = self._clock()
        e = self._refill(src, now)
        return e is not None and e[0] <= 0

    def spend(self, src: str) -> None:
        now = self._clock()
        e = self._refill(src, now)
        if e is not None:
            e[0] = max(0, e[0] - 1)
            return
        if len(self._entries) >= MAX_RATE_ENTRIES:
            self._sweep(now)
        if len(self._entries) >= MAX_RATE_ENTRIES:
            # 밀어내지 않는다. 새 출발지를 제한 없이 통과시킨다.
            self.counters["rate_table_full"] += 1
            return
        self._entries[src] = [RATE_LIMIT_BUCKET - 1, now]

    def _sweep(self, now: float) -> None:
        """다시 오지 않는 출발지의 항목도 보충으로 상한에 닿으면 지운다."""
        for src in list(self._entries):
            self._refill(src, now)


# ---------------------------------------------------------------- 7.2 한 바퀴 (수락 뒤)

Dispatch = Callable[[Request], dict]


async def handle_request(
    reader: asyncio.StreamReader,
    src: str,
    *,
    rate: RateTable,
    dispatch: Dispatch,
    counters: Counter,
    read_timeout_s: float = SERVER_READ_TIMEOUT_S,
    seen: dict | None = None,
) -> bytes | None:
    """연결 하나의 요청을 읽고 응답 바이트를 돌려준다. None 은 응답 없이 닫는다는 뜻이다.

    dispatch 는 연산 필드 dict 를 돌려주거나 OpError 를 던진다. 블로킹이므로 스레드로 보낸다.
    seen 을 주면 파싱이 끝난 요청의 연산 이름을 seen["op"] 에 적는다 (7.5 http.request 의 op).
    """
    try:
        try:
            req = await asyncio.wait_for(read_request(reader), read_timeout_s)
        except OpError as exc:
            return error_response(exc)
        except TimeoutError:
            counters["http_read_timeout"] += 1
            return None
        except Closed:
            return None
        if seen is not None:
            seen["op"] = req.op
        if rate.exhausted(src):
            counters["rate_limited"] += 1
            return error_response(OpError("rate_limited"))
        try:
            fields = await asyncio.to_thread(dispatch, req)
        except OpError as exc:
            if exc.code in COUNTED_ERRORS:
                rate.spend(src)
            return error_response(exc)
        return response(200, dict(fields, ok=True))
    except Exception:
        counters["internal_error"] += 1
        return error_response(OpError("internal"))  # 예외 내용을 본문에 싣지 않는다


# ---------------------------------------------------------------- 7.2 수락 루프, 7.5 로그와 카운터

BIND_HOST = "0.0.0.0"  # control_plane.md 3.2 주소 표. IPv4 전체. 127.0.0.1 이면 밖에서 닿지 않는다 (windows-prereq.md 6절)
COUNTER_PERIOD_S = 60  # 7.5 "60초마다"
# 7.5 카운터 여섯. 이 순서로 찍는다. elapsed_wall_fallback 은 ops 쪽(7.4 시계)이 센다.
COUNTER_NAMES = ("http_read_timeout", "internal_error", "rate_limited", "rate_table_full", FALLBACK_COUNTER,
                 "unavailable")
_STATUS_LINE = re.compile(rb"HTTP/1\.1 ([0-9]{3}) ")


def _status_and_error(out: bytes) -> tuple[int, str]:
    """response() 가 만든 바이트에서 (상태, 오류 코드). 성공이면 오류 코드는 '-' 다 (7.5)."""
    status = int(_STATUS_LINE.match(out).group(1))
    if status == 200:
        return status, "-"
    return status, json.loads(out.partition(_HEAD_END)[2])["error"]


class Server:
    """한 프로세스, 이벤트 루프 하나, 스레드 풀 하나 (7.2). 속도 제한 표와 카운터를 여기 하나만 둔다.

    dispatch 는 7.2 "ops.dispatch 의 계약" 이다. service_counters 는 ops 쪽 카운터의 사본을 돌려주는 함수
    (ops.Service.counters) 이고 elapsed_wall_fallback 을 거기서 읽는다. 함수가 없거나 그 키가 없으면 0 이다.
    clock 은 http.request 의 ms 를 재는 초 단위
    단조 시계다. 시간 값과 상한은 시험이 줄이려고 주입한다.

    요청을 다 읽지 않은 채 답하는 경로(503, 그리고 3.3 파싱 실패)는 응답 뒤에 lingering close 를 한다
    (RFC 9112 9.6). 쓰기 쪽을 먼저 닫고, 남은 입력을 linger_s 동안 LINGER_MAX_BYTES 까지 읽어 버린 뒤 닫는다.
    읽지 않은 입력이 남은 채 닫으면 OS 가 RST 를 보내고, 상대는 이미 받은 응답을 읽기 전에 연결 오류를 받는다.

    503 으로 거절하는 연결은 inflight 밖이다. 그 수를 max_rejecting 으로 막는다. 거절 중인 연결이 그만큼이면
    새 연결은 응답 없이 끊고(abort) unavailable 을 올린다.

    dispatch 는 이 Server 의 스레드 풀(max_inflight 개)에서 돈다. serve 가 그 풀을 이벤트 루프의 기본 실행기로
    건다. handle_request 의 asyncio.to_thread 가 그 풀로 간다.
    """

    def __init__(
        self,
        dispatch: Dispatch,
        *,
        service_counters: Callable[[], Mapping[str, int]] | None = None,
        read_timeout_s: float = SERVER_READ_TIMEOUT_S,
        write_timeout_s: float = SERVER_READ_TIMEOUT_S,  # 3.4 "응답 송신에도 같은 5초"
        max_inflight: int = MAX_INFLIGHT,
        max_rejecting: int = MAX_REJECTING,
        linger_s: float = LINGER_S,
        timeout: Callable[[float], AbstractAsyncContextManager] = asyncio.timeout,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.dispatch = dispatch
        self.counters: Counter = Counter()
        self.rate = RateTable(counters=self.counters)
        self.service_counters = service_counters
        self.read_timeout_s = read_timeout_s
        self.write_timeout_s = write_timeout_s
        self.max_inflight = max_inflight
        self.max_rejecting = max_rejecting
        self.pool = ThreadPoolExecutor(max_workers=max_inflight or 1, thread_name_prefix="cp-dispatch")
        self.linger_s = linger_s
        self._timeout = timeout  # 링거의 시간 제한 장치. 시험이 넘긴 값을 보려고 주입한다
        self._clock = clock
        self.inflight = 0
        self.rejecting = 0
        self.listening = False  # 리슨이 성공했는가. 기동 실패와 그 뒤의 실패를 가른다 (__main__)

    # ------------------------------------------------------------ 카운터

    def counter_values(self) -> dict[str, int]:
        """7.5 카운터 여섯의 현재 값. 0 인 것도 넣는다."""
        ext = self.service_counters() if self.service_counters is not None else {}
        out = {}
        for name in COUNTER_NAMES:
            out[name] = ext.get(name, 0) if name == FALLBACK_COUNTER else self.counters[name]
        return out

    def emit_counters(self) -> None:
        for name, value in self.counter_values().items():
            log.emit("INFO", "counter", name=name, value=value)

    async def _tick(self, period_s: float) -> None:
        while True:
            await asyncio.sleep(period_s)
            self.emit_counters()

    # ------------------------------------------------------------ 연결 하나

    async def on_connection(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        start = self._clock()
        peer = writer.get_extra_info("peername")
        src = peer[0] if isinstance(peer, tuple) and peer else "-"
        try:
            if self.inflight >= self.max_inflight:
                # 수락 직후, 파싱 전이다. 요청을 읽지 않고 저장소를 건드리지 않는다 (7.2).
                self.counters["unavailable"] += 1
                if self.rejecting >= self.max_rejecting:
                    writer.transport.abort()  # 거절할 자리도 없다. 응답 없이 끊는다
                    return
                self.rejecting += 1
                try:
                    if await self._answer(writer, error_response(OpError("unavailable")), "-", src, start):
                        await self._linger(reader, writer)
                finally:
                    self.rejecting -= 1
                return
            self.inflight += 1
            try:
                seen: dict = {}
                out = await handle_request(reader, src, rate=self.rate, dispatch=self.dispatch,
                                           counters=self.counters, read_timeout_s=self.read_timeout_s, seen=seen)
                if out is not None and await self._answer(writer, out, seen.get("op", "-"), src, start):
                    if "op" not in seen:  # 파싱이 끝나지 않았다. 요청 바이트가 남아 있을 수 있다
                        await self._linger(reader, writer)
            finally:
                self.inflight -= 1
        finally:
            writer.close()

    async def _answer(self, writer: asyncio.StreamWriter, out: bytes, op: str, src: str, start: float) -> bool:
        """응답을 보내고 http.request 한 줄을 남긴다. 송신에 write_timeout_s 를 건다 (3.4).

        시간 안에 다 못 보냈거나 상대가 끊었으면 남은 바이트를 버리고 연결을 끊는다. close 는 버퍼를 다
        보낼 때까지 연결을 붙들고 있으므로 abort 를 쓴다. 줄에는 연산 이름, 상태, 오류 코드, 출발지,
        경과만 싣는다. 요청 바이트는 싣지 않는다 (7.5). 다 보냈으면 True.
        """
        writer.write(out)
        sent = True
        try:
            await asyncio.wait_for(writer.drain(), self.write_timeout_s)
        except (TimeoutError, OSError):
            writer.transport.abort()
            sent = False
        status, error = _status_and_error(out)
        ms = int((self._clock() - start) * 1000)
        log.emit("INFO", "http.request", op=op, status=status, error=error, src=src, ms=ms)
        return sent

    async def _linger(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        """lingering close. 상대가 닫거나, linger_s 가 지나거나, LINGER_MAX_BYTES 를 읽으면 끝난다."""
        left = LINGER_MAX_BYTES
        try:
            writer.write_eof()
            async with self._timeout(self.linger_s):
                while left > 0:
                    chunk = await reader.read(left)
                    if not chunk:
                        break
                    left -= len(chunk)
        except (TimeoutError, OSError):
            pass

    # ------------------------------------------------------------ 수락 루프

    async def serve(self, host: str, port: int, stop: asyncio.Event, *,
                    counter_period_s: float = COUNTER_PERIOD_S,
                    on_ready: Callable[[int], None] | None = None) -> None:
        """stop 이 켜지거나 이 태스크가 취소될 때까지 받는다. 끝날 때 카운터 전량을 한 번 더 낸다.

        on_ready 는 bind 한 포트를 받는다. port 0 으로 띄운 시험이 쓴다.
        """
        asyncio.get_running_loop().set_default_executor(self.pool)
        srv = await asyncio.start_server(self.on_connection, host, port)
        bound = srv.sockets[0].getsockname()[1]
        self.listening = True
        log.emit("INFO", "server.started", bind=host, port=bound)
        ticker = asyncio.create_task(self._tick(counter_period_s))
        try:
            if on_ready is not None:
                on_ready(bound)
            await stop.wait()
        finally:
            ticker.cancel()
            srv.close()
            self.emit_counters()
