"""제어 서버 시험 하네스. 시험 전용이고 배포하지 않는다 (tools/cp-deploy 는 controlplane/ 만 묶는다).

진짜 서버를 띄운다. controlplane.server.Server + ops.Service + store.Store 가 DynamoDB local 의 새 테이블
위에서 127.0.0.1 에만 묶여 돈다. 서버 코드는 고치지 않는다. 주입은 이미 있는 자리만 쓴다.

| 주입 | 자리 |
|------|------|
| 응답 지연, 오류 응답(저장소 앞 또는 성공한 dispatch 뒤) | Server 에 넘기는 dispatch 를 감싼다 (control_plane.md 7.2 ops.dispatch 의 계약) |
| 커밋 뒤 실패 | Store(on_write=...) 의 "after" 주입점에서 OpError("internal") 을 던진다 (store.py 머리) |
| 서버 벽시계 | Service(now_ms=...) |
| 속도 제한 보충 시계 | RateTable(clock=...) 를 만들어 Server.rate 에 끼운다 |
| 연결을 붙들거나 답 없이 닫기 | Server 인스턴스의 on_connection 을 감싼다 (serve 가 self.on_connection 을 쓴다) |

주입은 따로 연 루프백 관리 포트의 줄 프로토콜로 건다. 한 줄 명령, 한 줄 JSON 응답이다. 명령 표는
COMMANDS 다. STUN 응답기도 같은 프로세스가 연다 (protocol.md 13장 STUN 사용 범위의 Binding 만). 응답기가 적는
매핑 주소가 루프백이 아닌 이유는 DEFAULT_MAPPED_IP 위에 있다.

로그. 서버의 줄(control_plane.md 7.5)은 그대로 표준 오류에 나간다. 하네스의 줄은 `HARNESS <이벤트> 이름=값`
이고 같은 표준 오류에 나간다. **하네스 줄에도 room_id, peer_token, client_nonce 를 싣지 않는다.** 같은 흐름에서
roadmap Phase 3 의 "서버 로그에 room_id 와 peer_token 이 없다" 를 grep 하기 때문이다. 관리 응답에는 실릴 수 있다.

정리. 테이블은 끝날 때 지운다. 관리 명령 quit, Ctrl+C, Ctrl+Break, 그리고 --exit-on-stdin-eof 를 주면 표준
입력의 EOF(부른 프로세스가 죽음)에 멈춘다. 강제 종료(TerminateProcess)만은 정리하지 못한다. DynamoDB local 은
-inMemory 라 컨테이너를 멈추면 같이 사라진다.

DynamoDB local 에 닿지 않으면 건너뛰지 않는다. 종료 코드 3 과 띄우는 명령을 낸다.

    .venv/Scripts/python tests/harness/cp_harness.py [--ddb http://127.0.0.1:8001] [--port 0] [--admin-port 0]
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import re
import secrets
import shutil
import sys
import tempfile
import threading
import time
import uuid
from collections import Counter
from pathlib import Path

_CS = Path(__file__).resolve().parents[2]  # control-server/
if os.environ.get("CP_SRC"):
    _CS = Path(os.environ["CP_SRC"]).resolve()
if str(_CS) not in sys.path:
    sys.path.insert(0, str(_CS))

from controlplane import clock as cp_clock  # noqa: E402
from controlplane import ids, log  # noqa: E402
from controlplane.constants import (DDB_CONNECT_TIMEOUT_S, DDB_MAX_ATTEMPTS, DDB_READ_TIMEOUT_S,  # noqa: E402
                                    OPS, SERVER_READ_TIMEOUT_S)
from controlplane.errors import STATUS, OpError  # noqa: E402

HOST = "127.0.0.1"
DEFAULT_DDB = "http://127.0.0.1:8001"
DOCKER_CMD = ("docker run -d --rm --name sangtachi-ddb -p 127.0.0.1:8001:8000 amazon/dynamodb-local:3.3.1 "
              "-jar DynamoDBLocal.jar -inMemory -sharedDb")
EXIT_USAGE = 2
EXIT_NO_DDB = 3

# store.py 의 쓰기 이름(on_write 의 label). 커밋 뒤 실패를 걸 수 있는 자리의 허용 목록이다.
WRITE_LABELS = ("create_room", "join_room", "register_candidates", "release_peer", "reclaim_peer", "confirm_pair",
                "renew_room", "renew_item")
# 오류 주입으로 돌려줄 수 있는 코드. errors.STATUS 의 키 전부다 (too_large 는 status 가 둘이라 뺀다).
ERROR_CODES = tuple(STATUS)
KINDS = ("delay", "error", "after_commit", "hold", "drop")
MAX_COUNT = 100
MAX_DELAY_MS = 60_000
MAX_OFFSET_MS = 10 * 365 * 86_400_000
MAX_BURN = 50
HOLD_MAX_S = 30.0
FAKE_KEY_ID = "fakeLocalOnlyNotAKey"
FAKE_SECRET = "fakeLocalOnlyNotASecret"
FAKE_REGION = "local-only"
LOOPBACK_HOSTS = frozenset({"127.0.0.1", "localhost", "::1"})


# ---------------------------------------------------------------- 하네스 로그

_out_lock = threading.Lock()
_VALUE = re.compile(r"[!-<>-~]+")


def _write_line(line: str) -> None:
    with _out_lock:
        sys.stderr.write(line + "\n")
        sys.stderr.flush()


def hline(event: str, **fields: object) -> str:
    """`HARNESS <이벤트> 이름=값`. 값은 공백·'='·제어 문자 없는 ASCII 로 바꾼다. 빈 값은 '-'."""
    parts = ["HARNESS", event]
    for name, value in fields.items():
        text = str(value) if value not in ("", None) else "-"
        if not _VALUE.fullmatch(text):
            text = re.sub(r"[^!-<>-~]", "_", text) or "-"
        parts.append(f"{name}={text}")
    return " ".join(parts)


def hlog(event: str, **fields: object) -> None:
    _write_line(hline(event, **fields))


# ---------------------------------------------------------------- 주입 상태 (순수)

class Injections:
    """주입 규칙의 큐. take 가 그 종류에서 연산이 맞는 첫 규칙을 하나 쓴다. 스레드 안전하다.

    규칙의 op 는 연산 이름이거나 "*" 다. after_commit 의 op 는 store 의 쓰기 이름(WRITE_LABELS)이다.
    hold 와 drop 은 연결을 읽기 전에 정하므로 op 가 "*" 뿐이다.
    """

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._rules: list[dict] = []
        self.fired: Counter = Counter()
        self.offset_ms = 0

    def add(self, kind: str, op: str, count: int, **params: object) -> dict:
        check_rule(kind, op, count, params)
        rule = {"kind": kind, "op": op, "left": count, **params}
        with self._lock:
            self._rules.append(rule)
        return dict(rule)

    def take(self, kind: str, op: str, **match: object) -> dict | None:
        """match 를 주면 규칙의 그 인자도 같아야 한다 (error 의 when)."""
        with self._lock:
            for i, rule in enumerate(self._rules):
                if rule["kind"] == kind and rule["op"] in ("*", op) and all(rule.get(k) == v for k, v in match.items()):
                    rule["left"] -= 1
                    if rule["left"] <= 0:
                        del self._rules[i]
                    self.fired[kind] += 1
                    return dict(rule)
        return None

    def pending(self) -> list[dict]:
        with self._lock:
            return [dict(r) for r in self._rules]

    def clear(self) -> None:
        with self._lock:
            self._rules.clear()
            self.fired.clear()
            self.offset_ms = 0


def check_rule(kind: str, op: str, count: object, params: dict) -> None:
    """규칙 하나의 판정. 허용 목록이다. 모르는 것은 ValueError."""
    if kind not in KINDS:
        raise ValueError("unknown kind")
    if type(count) is not int or not 1 <= count <= MAX_COUNT:
        raise ValueError("count must be 1..100")
    allowed = {"delay": {"ms", "when"}, "error": {"code", "when"}, "after_commit": set(), "hold": set(),
               "drop": set()}[kind]
    if set(params) != allowed:
        raise ValueError("wrong parameters for " + kind)
    if kind == "after_commit":
        if op not in WRITE_LABELS:
            raise ValueError("after_commit needs a store write label")
    elif kind in ("hold", "drop"):
        if op != "*":
            raise ValueError(kind + " applies to any op only")
    elif op != "*" and op not in OPS:
        raise ValueError("unknown op")
    if kind == "delay":
        ms = params["ms"]
        if type(ms) is not int or not 0 <= ms <= MAX_DELAY_MS:
            raise ValueError("ms must be 0..60000")
    if kind in ("delay", "error") and params["when"] not in ("before", "after"):
        raise ValueError("when must be before or after")
    if kind == "error" and params["code"] not in ERROR_CODES:
        raise ValueError("unknown error code")


class RateClock:
    """RateTable 의 초 단위 시계. freeze 하면 그 값에 멈추고 thaw 하면 실제 단조 시계로 돌아간다(멈춘 동안 흐른
    시간이 한꺼번에 보충된다). advance 는 앞으로만 민다. 뒤로 가지 않는다(단조)."""

    def __init__(self, base=time.monotonic) -> None:
        self._base = base
        self._lock = threading.Lock()
        self._frozen: float | None = None
        self._ahead = 0.0

    def __call__(self) -> float:
        with self._lock:
            if self._frozen is not None:
                return self._frozen
            return self._base() + self._ahead

    @property
    def frozen(self) -> bool:
        return self._frozen is not None

    def freeze(self) -> None:
        now = self()
        with self._lock:
            self._frozen = now

    def thaw(self) -> None:
        with self._lock:
            if self._frozen is not None:
                # 멈춘 값보다 뒤로 가지 않게 한다
                self._ahead = max(self._ahead, self._frozen - self._base())
            self._frozen = None

    def advance(self, seconds: float) -> None:
        if not 0 <= seconds <= 86_400:
            raise ValueError("seconds must be 0..86400")
        with self._lock:
            self._ahead += seconds
            if self._frozen is not None:
                self._frozen += seconds


# ---------------------------------------------------------------- 관리 명령 (순수 파싱)

_TOKEN = re.compile(r"[A-Za-z0-9_*#.:-]+")
# 명령 -> {인자: 형}. 형은 "int", "str". 모든 인자는 필수다. 관리 응답은 한 줄 JSON 이다.
COMMANDS: dict[str, dict[str, str]] = {
    "ping": {},
    "delay": {"op": "str", "count": "int", "ms": "int", "when": "str"},
    "error": {"op": "str", "count": "int", "code": "str", "when": "str"},
    "after_commit": {"label": "str", "count": "int"},
    "hold": {"count": "int"},
    "drop": {"count": "int"},
    "clock": {"offset_ms": "int"},
    "rate_freeze": {},
    "rate_thaw": {},
    "rate_advance": {"s": "int"},
    "rate_reset": {},
    "rate": {},
    "burn": {"n": "int"},
    "count": {},
    "room": {"id": "str"},
    "pending": {},
    "clear": {},
    "quit": {},
}


def parse_command(line: str) -> tuple[str, dict]:
    """`동사 이름=값 ...`. 모르는 동사, 빠진 인자, 남는 인자, 형이 틀린 값은 ValueError."""
    if not isinstance(line, str) or not line.isascii():
        raise ValueError("ascii only")
    words = line.split()
    if not words:
        raise ValueError("empty command")
    verb, rest = words[0], words[1:]
    spec = COMMANDS.get(verb)
    if spec is None:
        raise ValueError("unknown command")
    args: dict = {}
    for word in rest:
        name, sep, value = word.partition("=")
        if not sep or name not in spec or name in args:
            raise ValueError("bad argument")
        if _TOKEN.fullmatch(value) is None:
            raise ValueError("bad value")
        if spec[name] == "int":
            if re.fullmatch(r"-?[0-9]{1,15}", value) is None:
                raise ValueError("bad integer")
            args[name] = int(value)
        else:
            args[name] = value
    if set(args) != set(spec):
        raise ValueError("missing argument")
    return verb, args


# ---------------------------------------------------------------- STUN 응답기

MAGIC = b"\x21\x12\xa4\x42"


# 응답기가 매핑으로 적는 주소. 출발지 포트는 그대로 두고 주소만 바꾼다.
#
# 루프백(127.0.0.1)을 그대로 적으면 클라이언트가 그것을 반사 후보로 등록하고, 서버의 후보 위생
# (control_plane.md 4.4, protocol.md 10.1)이 루프백을 거부해 모든 register_candidate 가 bad_request 가 된다.
# 그래서 문서용 대역(RFC 5737)의 주소를 적는다. 경로가 없는 대역이라 Phase 3 에서는 그리로 아무것도 나가지 않고,
# 위생 표의 거부 대역도 아니다. 받는 값은 이 세 대역과, 거부 경로를 일부러 밟을 때의 127.0.0.1 뿐이다 (허용 목록).
DEFAULT_MAPPED_IP = "192.0.2.1"
_MAPPED_ALLOWED = re.compile(r"(?:192\.0\.2|198\.51\.100|203\.0\.113)\.(?:25[0-4]|2[0-4][0-9]|1[0-9][0-9]|[1-9][0-9]|[1-9])"
                             r"|127\.0\.0\.1")


def check_mapped_ip(text: str) -> str:
    if not isinstance(text, str) or _MAPPED_ALLOWED.fullmatch(text) is None:
        raise ValueError("mapped ip must be in 192.0.2.0/24, 198.51.100.0/24, 203.0.113.0/24 (host part 1..254) "
                         "or be 127.0.0.1")
    return text


def stun_response(request: bytes, addr: tuple[str, int], mapped_ip: str | None = None) -> bytes | None:
    """Binding Request 면 XOR-MAPPED-ADDRESS 하나를 실은 Success Response. 아니면 None (RFC 5389).

    매핑의 포트는 출발지 포트다. 주소는 mapped_ip 이고 없으면 출발지 주소다."""
    if len(request) < 20 or request[0:2] != b"\x00\x01" or request[4:8] != MAGIC:
        return None
    host, port = (mapped_ip or addr[0]), addr[1]
    octets = bytes(int(x) for x in host.split("."))
    xport = (port ^ 0x2112).to_bytes(2, "big")
    xaddr = bytes(a ^ b for a, b in zip(octets, MAGIC))
    attr = b"\x00\x20\x00\x08" + b"\x00\x01" + xport + xaddr
    return b"\x01\x01" + len(attr).to_bytes(2, "big") + MAGIC + request[8:20] + attr


class StunResponder(asyncio.DatagramProtocol):
    def __init__(self, mapped_ip: str = DEFAULT_MAPPED_IP) -> None:
        self.transport = None
        self.port = 0
        self.mapped_ip = mapped_ip

    def connection_made(self, transport) -> None:
        self.transport = transport
        self.port = transport.get_extra_info("sockname")[1]

    def datagram_received(self, data: bytes, addr) -> None:
        out = stun_response(data, addr, self.mapped_ip)
        if out is None:
            return
        hlog("stun.request", port=self.port, src=f"{addr[0]}:{addr[1]}")
        self.transport.sendto(out, addr)


# ---------------------------------------------------------------- AWS 격리와 테이블

def loopback_endpoint(url: str) -> str:
    """tests/conftest.py 의 같은 이름 함수와 같은 규칙. 루프백 http URL 만 받는다."""
    from urllib.parse import urlsplit

    if not isinstance(url, str) or not url.isascii() or any(ch.isspace() for ch in url):
        raise ValueError("endpoint has spaces or non-ascii")
    parts = urlsplit(url)
    if parts.scheme != "http":
        raise ValueError("http only")
    if parts.username is not None or parts.password is not None or parts.query or parts.fragment:
        raise ValueError("no userinfo, query or fragment")
    if parts.path not in ("", "/"):
        raise ValueError("no path")
    host = parts.hostname
    if host not in LOOPBACK_HOSTS:
        raise ValueError("not a loopback host")
    port = parts.port
    if port is None or port == 0:
        raise ValueError("port required")
    return f"http://[{host}]:{port}" if ":" in host else f"http://{host}:{port}"


def isolate_aws(tmpdir: str) -> None:
    """이 프로세스의 AWS 설정을 가짜 값으로 바꾼다. 실제 자격 증명과 프로파일을 읽지 않는다 (conftest 와 같은 목록)."""
    for name in list(os.environ):
        if name.upper().startswith("AWS_"):
            del os.environ[name]
    for name in ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy"):
        os.environ.pop(name, None)
    cfg = Path(tmpdir) / "aws-config-empty"
    cred = Path(tmpdir) / "aws-credentials-empty"
    cfg.write_text("", encoding="ascii")
    cred.write_text("", encoding="ascii")
    os.environ.update({"AWS_CONFIG_FILE": str(cfg), "AWS_SHARED_CREDENTIALS_FILE": str(cred),
                       "AWS_ACCESS_KEY_ID": FAKE_KEY_ID, "AWS_SECRET_ACCESS_KEY": FAKE_SECRET,
                       "AWS_EC2_METADATA_DISABLED": "true", "NO_PROXY": "127.0.0.1,localhost,::1",
                       "no_proxy": "127.0.0.1,localhost,::1"})


def ddb_clients(endpoint: str):
    """(관리용, 서버용). 서버용은 배포와 같은 시간 제한과 시도 횟수다 (control_plane.md 7.6)."""
    import boto3.session
    from botocore.config import Config

    admin = boto3.session.Session().client(
        "dynamodb", region_name=FAKE_REGION, endpoint_url=endpoint,
        config=Config(proxies={}, connect_timeout=2, read_timeout=10, retries={"max_attempts": 5, "mode": "standard"}))
    server = boto3.session.Session().client(
        "dynamodb", region_name=FAKE_REGION, endpoint_url=endpoint,
        config=Config(proxies={}, connect_timeout=DDB_CONNECT_TIMEOUT_S, read_timeout=DDB_READ_TIMEOUT_S,
                      retries={"mode": "standard", "total_max_attempts": DDB_MAX_ATTEMPTS}))
    return admin, server


def create_table(admin, table: str) -> None:
    """테이블과 TTL. 테이블을 만든 뒤 TTL 설정이 실패하면 그 테이블을 지우고 예외를 그대로 올린다.

    지우기마저 실패하면 hlog 로 알리고 원래 예외를 올린다. 테이블 이름은 줄에 실린다(비밀이 아니다)."""
    admin.create_table(
        TableName=table,
        KeySchema=[{"AttributeName": "pk", "KeyType": "HASH"}, {"AttributeName": "sk", "KeyType": "RANGE"}],
        AttributeDefinitions=[{"AttributeName": "pk", "AttributeType": "S"},
                              {"AttributeName": "sk", "AttributeType": "S"}],
        ProvisionedThroughput={"ReadCapacityUnits": 25, "WriteCapacityUnits": 25})
    try:
        admin.update_time_to_live(TableName=table, TimeToLiveSpecification={"Enabled": True, "AttributeName": "ttl"})
    except BaseException:
        try:
            admin.delete_table(TableName=table)
            hlog("table.deleted", table=table)
        except Exception as exc:
            hlog("error", reason="table_delete_failed", table=table, exc=type(exc).__name__)
        raise


def _error_code(exc: BaseException) -> str | None:
    """botocore ClientError 의 오류 코드. 아니면 None."""
    response = getattr(exc, "response", None)
    if isinstance(response, dict):
        return response.get("Error", {}).get("Code")
    return None


class TableCleanup:
    """테이블을 한 번만 지운다. amain 의 finally 와 main 의 finally 가 둘 다 부른다. 먼저 부른 쪽이 지운다.

    amain 의 finally 에서 지우는 것은 asyncio.run 이 실행기 스레드를 기다리는 동안 막혀도 테이블이 먼저
    사라지게 하려는 것이다. 성공하면 True."""

    def __init__(self, admin, table: str) -> None:
        self.admin = admin
        self.table = table
        self._lock = threading.Lock()
        self.done: bool | None = None  # None 이면 아직 시도하지 않았다

    def __call__(self) -> bool:
        with self._lock:
            if self.done is None:
                try:
                    self.admin.delete_table(TableName=self.table)
                    hlog("table.deleted", table=self.table)
                    self.done = True
                except Exception as exc:  # 정리 실패는 알리고 끝낸다
                    if _error_code(exc) == "ResourceNotFoundException":  # 만들어지기 전에 멈췄다
                        hlog("table.absent", table=self.table)
                        self.done = True
                        return True
                    hlog("error", reason="table_delete_failed", table=self.table, exc=type(exc).__name__)
                    self.done = False
            return self.done


def sk_kind(sk: str) -> str:
    if sk == "ROOM":
        return "ROOM"
    if sk == "NONCE":
        return "NONCE"
    for prefix in ("PEER", "VIP", "PAIR"):
        if sk.startswith(prefix + "#"):
            return prefix
    return "OTHER"


# ---------------------------------------------------------------- 표준 입력 EOF

class StdinWatch:
    """--exit-on-stdin-eof. 표준 입력이 닫히면 closed 를 켜고 걸어 둔 함수를 부른다. 기동 초기에 시작한다.

    테이블을 만드는 중(boto3 호출 중)에 닫혀도 그 호출이 끝난 뒤 _run 이 closed 를 보고 정리한다."""

    def __init__(self) -> None:
        self.closed = threading.Event()
        self._lock = threading.Lock()
        self._callback = None

    def start(self) -> None:
        threading.Thread(target=self._watch, name="stdin-eof", daemon=True).start()

    def _watch(self) -> None:
        try:
            while sys.stdin.buffer.read(4096):
                pass
        except (OSError, ValueError):
            pass
        with self._lock:
            self.closed.set()
            callback = self._callback
        if callback is not None:
            callback()

    def on_close(self, callback) -> None:
        with self._lock:
            self._callback = callback
            fire = self.closed.is_set()
        if fire:
            callback()


_STDIN = StdinWatch()


# ---------------------------------------------------------------- 하네스 본체

class Harness:
    def __init__(self, admin_client, store, service, server, inj: Injections, rate_clock: RateClock) -> None:
        self.admin_client = admin_client
        self.store = store
        self.service = service
        self.server = server
        self.inj = inj
        self.rate_clock = rate_clock
        self.cp_port = 0
        self.stop: asyncio.Event | None = None
        # 멈출 때 켠다. 지연 주입은 이것을 기다리며 잔다. 깨지 않으면 asyncio.run 이 기본 실행기의 스레드를
        # 기다리느라 최대 MAX_DELAY_MS 동안 끝나지 않는다.
        self.stopping = threading.Event()

    # dispatch 를 감싼다. 스레드 풀에서 돈다.
    #
    # error 의 when=before 는 저장소에 닿기 전에 그 코드로 답한다. when=after 는 진짜 dispatch 가 성공한 뒤에만
    # 걸리고 그 응답을 그 코드로 바꾼다. 쓰기가 있었으면 커밋 뒤 실패이고, 멱등 재생(같은 nonce)이었으면 재생 뒤
    # 실패다. store 의 after_commit 은 쓰기가 있어야 걸리므로 재생에는 걸리지 않는다. 둘을 이어 쓰면 첫 시도는
    # 커밋 뒤, 둘째 시도는 재생 뒤에 실패하게 만들 수 있다.
    def dispatch(self, req):
        op = req.op if isinstance(req.op, str) else "-"
        delay = self.inj.take("delay", op)
        error = self.inj.take("error", op, when="before")
        if delay is not None and delay["when"] == "before":
            hlog("inject", kind="delay", op=op, ms=delay["ms"], when="before")
            self.stopping.wait(delay["ms"] / 1000)
        try:
            if error is not None:
                hlog("inject", kind="error", op=op, code=error["code"], when="before")
                raise OpError(error["code"], "harness injection")
            result = self.service.dispatch(req)
            if op in ("create_room", "join_room"):
                hlog("issued", op=op, peer_id=result.get("peer_id"), virtual_ip=result.get("virtual_ip"))
            late = self.inj.take("error", op, when="after")
            if late is not None:
                hlog("inject", kind="error", op=op, code=late["code"], when="after")
                raise OpError(late["code"], "harness injection")
            return result
        finally:
            if delay is not None and delay["when"] == "after":
                hlog("inject", kind="delay", op=op, ms=delay["ms"], when="after")
                self.stopping.wait(delay["ms"] / 1000)

    # Store 의 쓰기 주입점.
    def on_write(self, phase: str, label: str, sk: str) -> None:
        if phase != "after":
            return
        rule = self.inj.take("after_commit", label)
        if rule is not None:
            hlog("inject", kind="after_commit", label=label, sk=sk)
            raise OpError("internal", "harness after-commit injection")

    # Server.on_connection 을 감싼다.
    async def on_connection(self, reader, writer, *, _orig) -> None:
        rule = self.inj.take("hold", "*")
        kind = "hold"
        if rule is None:
            rule = self.inj.take("drop", "*")
            kind = "drop"
        if rule is None:
            await _orig(reader, writer)
            return
        from controlplane.server import Closed, read_request

        op = "-"
        try:
            try:
                req = await asyncio.wait_for(read_request(reader), SERVER_READ_TIMEOUT_S)
                op = req.op
            except (OpError, Closed, TimeoutError, OSError):
                pass
            hlog("held" if kind == "hold" else "dropped", op=op)
            if kind == "hold":
                try:
                    await asyncio.wait_for(reader.read(), HOLD_MAX_S)  # 상대가 닫을 때까지
                except (TimeoutError, OSError):
                    pass
        finally:
            writer.transport.abort() if kind == "drop" else writer.close()

    # ------------------------------------------------------------ 관리 명령

    async def handle_admin(self, verb: str, args: dict) -> dict:
        inj = self.inj
        if verb == "ping":
            return {"ok": True, "cp_port": self.cp_port}
        if verb == "delay":
            return {"ok": True, "rule": inj.add("delay", args["op"], args["count"], ms=args["ms"], when=args["when"])}
        if verb == "error":
            return {"ok": True, "rule": inj.add("error", args["op"], args["count"], code=args["code"],
                                                when=args["when"])}
        if verb == "after_commit":
            return {"ok": True, "rule": inj.add("after_commit", args["label"], args["count"])}
        if verb in ("hold", "drop"):
            return {"ok": True, "rule": inj.add(verb, "*", args["count"])}
        if verb == "clock":
            if abs(args["offset_ms"]) > MAX_OFFSET_MS:
                raise ValueError("offset too large")
            inj.offset_ms = args["offset_ms"]
            hlog("clock", offset_ms=inj.offset_ms)
            return {"ok": True, "offset_ms": inj.offset_ms}
        if verb == "rate_freeze":
            self.rate_clock.freeze()
            hlog("rate", action="freeze")
            return {"ok": True}
        if verb == "rate_thaw":
            self.rate_clock.thaw()
            hlog("rate", action="thaw")
            return {"ok": True}
        if verb == "rate_advance":
            self.rate_clock.advance(args["s"])
            hlog("rate", action="advance", s=args["s"])
            return {"ok": True}
        if verb == "rate_reset":
            self.reset_rate()
            hlog("rate", action="reset")
            return {"ok": True}
        if verb == "rate":
            return {"ok": True, "frozen": self.rate_clock.frozen, "tokens": self.server.rate.peek(HOST)}
        if verb == "burn":
            if not 1 <= args["n"] <= MAX_BURN:
                raise ValueError("n must be 1..50")
            return {"ok": True, "statuses": await self.burn(args["n"])}
        if verb == "count":
            return {"ok": True, "items": await asyncio.to_thread(self.count_items)}
        if verb == "room":
            room_id = ids.normalize_room_id(args["id"])
            return {"ok": True, **(await asyncio.to_thread(self.room_items, room_id))}
        if verb == "pending":
            return {"ok": True, "rules": inj.pending(), "fired": dict(inj.fired), "offset_ms": inj.offset_ms,
                    "rate_frozen": self.rate_clock.frozen}
        if verb == "clear":
            inj.clear()
            self.rate_clock.thaw()
            self.reset_rate()
            hlog("clear")
            return {"ok": True}
        if verb == "quit":
            self.stop.set()
            return {"ok": True}
        raise ValueError("unknown command")

    def reset_rate(self) -> None:
        from controlplane.server import RateTable

        self.server.rate = RateTable(clock=self.rate_clock, counters=self.server.counters)

    async def burn(self, n: int) -> list[int]:
        """없는 방 join_room 을 n 번 보낸다. 출발지는 127.0.0.1 이다 (6.4 속도 제한의 출발지)."""
        statuses = []
        for _ in range(n):
            while True:
                room_id = ids.new_room_id()
                if await asyncio.to_thread(self.store.get_room, room_id) is None:
                    break
            body = json.dumps({"room_id": room_id, "client_nonce": secrets.token_hex(16)}).encode("ascii")
            reader, writer = await asyncio.open_connection(HOST, self.cp_port)
            try:
                writer.write(b"POST /v1/join_room HTTP/1.1\r\nHost: harness\r\nContent-Type: application/json\r\n"
                             b"Content-Length: " + str(len(body)).encode("ascii") + b"\r\nConnection: close\r\n\r\n"
                             + body)
                await writer.drain()
                head = await asyncio.wait_for(reader.readuntil(b"\r\n"), 10)
                statuses.append(int(head.split(b" ")[1]))
            finally:
                writer.close()
        return statuses

    def count_items(self) -> dict:
        counts: Counter = Counter()
        start = None
        while True:
            kwargs = {"TableName": self.store.table, "ConsistentRead": True, "ProjectionExpression": "sk"}
            if start is not None:
                kwargs["ExclusiveStartKey"] = start
            resp = self.admin_client.scan(**kwargs)
            for item in resp.get("Items", []):
                counts[sk_kind(item["sk"]["S"])] += 1
            start = resp.get("LastEvaluatedKey")
            if not start:
                break
        return {k: counts.get(k, 0) for k in ("ROOM", "PEER", "VIP", "PAIR", "NONCE", "OTHER")}

    def room_items(self, room_id: str) -> dict:
        from boto3.dynamodb.types import TypeDeserializer

        de = TypeDeserializer()
        items = []
        start = None
        while True:
            kwargs = {"TableName": self.store.table, "ConsistentRead": True,
                      "KeyConditionExpression": "pk = :pk", "ExpressionAttributeValues": {":pk": {"S": room_id}}}
            if start is not None:
                kwargs["ExclusiveStartKey"] = start
            resp = self.admin_client.query(**kwargs)
            items += [{k: de.deserialize(v) for k, v in it.items()} for it in resp.get("Items", [])]
            start = resp.get("LastEvaluatedKey")
            if not start:
                break
        peers, vips, pairs, room = [], [], 0, None
        for it in items:
            kind = sk_kind(it["sk"])
            if kind == "ROOM":
                room = {"host_peer_id": int(it["host_peer_id"]), "expires_at_ms": int(it["expires_at_ms"])}
            elif kind == "PEER":
                peers.append({"peer_id": int(it["peer_id"]), "virtual_ip": it["virtual_ip"],
                              "candidates": len(it.get("candidates", []))})
            elif kind == "VIP":
                vips.append({"virtual_ip": it["sk"][4:], "peer_id": int(it["peer_id"])})
            elif kind == "PAIR":
                pairs += 1
        return {"room": room, "peers": peers, "vips": vips, "pairs": pairs}

    async def admin_connection(self, reader, writer) -> None:
        try:
            while True:
                line = await reader.readline()
                if not line:
                    break
                try:
                    text = line.decode("ascii").strip()
                    verb, args = parse_command(text)
                    reply = await self.handle_admin(verb, args)
                    hlog("admin", verb=verb, ok="true")
                except (ValueError, OpError, UnicodeDecodeError) as exc:
                    reply = {"ok": False, "error": type(exc).__name__ + ": " + str(exc)}
                    hlog("admin", verb="-", ok="false")
                except Exception as exc:  # 관리 연결 하나의 실패가 하네스를 죽이지 않는다
                    reply = {"ok": False, "error": type(exc).__name__}
                    hlog("admin", verb="-", ok="false")
                writer.write(json.dumps(reply, separators=(",", ":"), ensure_ascii=True).encode("ascii") + b"\n")
                await writer.drain()
        except OSError:
            pass
        finally:
            writer.close()


async def amain(args, admin_client, server_client, table: str, cleanup=None) -> None:
    from controlplane.__main__ import install_stop_signals
    from controlplane.ops import Service
    from controlplane.server import Server
    from controlplane.store import Store

    inj = Injections()
    rate_clock = RateClock()
    holder: dict = {}

    store = Store(table, server_client, on_write=lambda p, l, s: holder["h"].on_write(p, l, s))
    service = Service(store, now_ms=lambda: cp_clock.now_wall_ms() + inj.offset_ms)
    server = Server(lambda req: holder["h"].dispatch(req), service_counters=service.counters)
    h = Harness(admin_client, store, service, server, inj, rate_clock)
    holder["h"] = h
    h.reset_rate()
    orig = server.on_connection
    server.on_connection = lambda r, w: h.on_connection(r, w, _orig=orig)  # 인스턴스 속성. serve 가 이것을 쓴다

    loop = asyncio.get_running_loop()
    stop = asyncio.Event()
    h.stop = stop
    restore = install_stop_signals(loop, stop)
    _STDIN.on_close(lambda: loop.call_soon_threadsafe(stop.set))

    ready = loop.create_future()
    serve_task = asyncio.create_task(server.serve(HOST, args.port, stop, on_ready=ready.set_result))
    admin_srv = None
    stun = []
    try:
        done, _ = await asyncio.wait({serve_task, ready}, return_when=asyncio.FIRST_COMPLETED)
        if serve_task in done:
            serve_task.result()  # bind 실패 등. 그대로 올린다
        h.cp_port = ready.result()
        admin_srv = await asyncio.start_server(h.admin_connection, HOST, args.admin_port)
        admin_port = admin_srv.sockets[0].getsockname()[1]
        for _ in range(args.stun):
            transport, proto = await loop.create_datagram_endpoint(lambda: StunResponder(args.stun_mapped_ip),
                                                                   local_addr=(HOST, 0))
            stun.append((transport, proto))
        hlog("ready", cp_port=h.cp_port, admin_port=admin_port,
             stun_ports=",".join(str(p.port) for _, p in stun) or "-", table=table, mapped_ip=args.stun_mapped_ip)
        await serve_task
    finally:
        stop.set()
        h.stopping.set()  # 지연 주입으로 자는 dispatch 스레드를 깨운다
        restore()
        if cleanup is not None:
            cleanup()  # 실행기 종료를 기다리기 전에 지운다. 블로킹이지만 끝나는 중이다
        if admin_srv is not None:
            admin_srv.close()
        for transport, _ in stun:
            transport.close()
        if not serve_task.done():
            serve_task.cancel()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Sangtachi control-plane test harness (loopback only)")
    parser.add_argument("--ddb", default=DEFAULT_DDB, help="DynamoDB local URL. loopback only")
    parser.add_argument("--port", type=int, default=0, help="control-plane port on 127.0.0.1. 0 = OS chooses")
    parser.add_argument("--admin-port", type=int, default=0, help="admin port on 127.0.0.1. 0 = OS chooses")
    parser.add_argument("--stun", type=int, default=2, help="number of local STUN responders (0..4)")
    parser.add_argument("--stun-mapped-ip", default=DEFAULT_MAPPED_IP,
                        help="address the STUN responders report as mapped (documentation ranges or 127.0.0.1)")
    parser.add_argument("--exit-on-stdin-eof", action="store_true", help="stop when standard input closes")
    args = parser.parse_args(argv)
    for name in ("port", "admin_port"):
        if not 0 <= getattr(args, name) <= 65535:
            print(f"harness: --{name.replace('_', '-')} must be 0..65535", file=sys.stderr)
            return EXIT_USAGE
    if not 0 <= args.stun <= 4:
        print("harness: --stun must be 0..4", file=sys.stderr)
        return EXIT_USAGE
    try:
        check_mapped_ip(args.stun_mapped_ip)
    except ValueError as exc:
        print(f"harness: --stun-mapped-ip rejected: {exc}", file=sys.stderr)
        return EXIT_USAGE
    try:
        endpoint = loopback_endpoint(args.ddb)
    except ValueError as exc:
        print(f"harness: --ddb rejected: {exc}", file=sys.stderr)
        return EXIT_USAGE

    if args.exit_on_stdin_eof:
        _STDIN.start()
    tmp = tempfile.mkdtemp(prefix="cp-harness-")
    try:
        return _run(args, endpoint, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def _run(args, endpoint: str, tmp: str) -> int:
    """테이블을 만들고 서버를 돌린 뒤 지운다. 부르는 쪽이 tmp 를 지운다.

    테이블 생성 호출부터 끝까지가 한 try 안이다. 생성 호출 중이나 직후에 Ctrl+C 가 와도 finally 가 지운다.
    그때 테이블이 아직 없으면 TableCleanup 이 ResourceNotFoundException 을 "없음" 으로 받는다. DynamoDB 에
    닿지 않아 만들지 못한 경로만 지우기를 건너뛴다."""
    isolate_aws(tmp)
    log.set_sink(_write_line)
    from botocore.exceptions import BotoCoreError, ClientError

    admin_client, server_client = ddb_clients(endpoint)
    table = "cp-harness-" + uuid.uuid4().hex
    cleanup = TableCleanup(admin_client, table)
    unreachable = False
    rc = 0
    try:
        try:
            create_table(admin_client, table)
        except (BotoCoreError, ClientError) as exc:
            unreachable = True
            hlog("error", reason="ddb_unreachable", endpoint=endpoint, exc=type(exc).__name__)
            print(f"harness: DynamoDB local is not reachable at {endpoint} or refused the table setup "
                  f"({type(exc).__name__}).", file=sys.stderr)
            print(f"harness: start it with: {DOCKER_CMD}", file=sys.stderr)
            return EXIT_NO_DDB
        if _STDIN.closed.is_set():  # 테이블을 만드는 동안 부른 쪽이 끝났다
            return 0
        asyncio.run(amain(args, admin_client, server_client, table, cleanup))
    except KeyboardInterrupt:
        rc = 130
    except Exception as exc:
        hlog("error", reason="crashed", exc=type(exc).__name__)
        rc = 1
    finally:
        if not unreachable and not cleanup():
            rc = rc or 1
    return rc


if __name__ == "__main__":
    code = main()
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(code)  # 지연 주입으로 자고 있는 dispatch 스레드를 기다리지 않는다
