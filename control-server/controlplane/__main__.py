"""python -m controlplane. 제어 서버 프로세스 하나 (control_plane.md 7.2, 7.6 설정과 배포).

설정은 환경 변수 넷이다 (7.6). 자격 증명을 받는 변수는 없다. boto3 표준 경로(EC2 에서는 IAM 역할)에
맡긴다. 필수 변수가 없거나 포트를 읽을 수 없으면 시작하지 않고 그 이름을 적어 끝난다 (종료 코드 2).

SIGINT 와 SIGTERM 에 멈추고 카운터 전량을 낸다 (7.5). Windows 에는 add_signal_handler 가 없다. 그때 Ctrl+C
는 asyncio.run 이 주 태스크를 취소하는 것으로 오고, Server.serve 의 finally 가 카운터를 낸다. Windows 의
Ctrl+Break(SIGBREAK)도 멈춤 신호로 받는다.

멈출 때 처리 중인 dispatch 를 기다리지 않는다. 리슨을 닫고 카운터를 낸 뒤 표준 출력과 표준 오류를 비우고
os._exit(0) 로 끝낸다. 정상 반환하면 asyncio.run 과 concurrent.futures 의 종료 처리가 막혀 있는 boto3 호출의
스레드를 join 한다.
"""

from __future__ import annotations

import asyncio
import errno
import ipaddress
import os
import re
import signal
import sys
from collections.abc import Callable, Mapping
from urllib.parse import urlsplit
from dataclasses import dataclass


# 10진수 1~65535. 선행 0, 부호, 공백, ASCII 밖의 숫자를 받지 않는다. int() 에 형식 판정을 맡기지 않는다.
_PORT = re.compile(r"[1-9][0-9]{0,4}")
_PORT_MAX = 65535
# AWS 리전 이름. 소문자, 숫자, '-' 만. boto3 는 틀린 리전의 값을 예외 문구에 싣는다.
_REGION = re.compile(r"[a-z0-9-]+")
# 엔드포인트 호스트 이름. 영문자, 숫자, '-', '.' 만. 괄호 안 IPv6 리터럴은 따로 본다.
_HOSTNAME = re.compile(r"[A-Za-z0-9.-]+")

EXIT_CONFIG = 2
EXIT_START = 1


class ConfigError(Exception):
    pass


@dataclass(frozen=True)
class Config:
    port: int
    table: str
    region: str
    endpoint: str | None


def load_config(environ: Mapping[str, str]) -> Config:
    """7.6 설정 표. 모르는 값은 통과시키지 않고 ConfigError 다."""
    from .constants import CONTROL_PORT

    raw_port = environ.get("SANGTACHI_CP_PORT")
    if raw_port is None:
        port = CONTROL_PORT
    else:
        if _PORT.fullmatch(raw_port) is None or int(raw_port) > _PORT_MAX:
            raise ConfigError("SANGTACHI_CP_PORT 는 1~65535 의 10진수여야 한다")
        port = int(raw_port)
    table = environ.get("SANGTACHI_CP_TABLE", "")
    if not table:
        raise ConfigError("SANGTACHI_CP_TABLE 이 없다. DynamoDB 테이블 이름을 넣는다")
    region = environ.get("AWS_REGION", "")
    if _REGION.fullmatch(region) is None:  # 없거나 빈 값도 여기서 걸린다
        raise ConfigError("AWS_REGION 이 없거나 틀렸다. 소문자, 숫자, '-' 로 된 리전 이름을 넣는다")
    endpoint = environ.get("SANGTACHI_CP_ENDPOINT") or None
    if endpoint is not None:
        check_endpoint(endpoint)
    return Config(port, table, region, endpoint)


def check_endpoint(url: str) -> None:
    """SANGTACHI_CP_ENDPOINT. http 나 https, 호스트가 있고, 경로·질의·조각·사용자 정보가 없다.

    호스트는 허용 목록이다. 영문자·숫자·'-'·'.' 로 된 이름, 또는 괄호 안의 IPv6 리터럴(ipaddress 로 파싱)만 받는다.

    urlsplit 은 탭과 줄바꿈을 지우고 읽으므로 공백과 ASCII 밖의 글자를 먼저 거부한다. 오류 문구에 값을 싣지 않는다.
    """
    bad = ConfigError("SANGTACHI_CP_ENDPOINT 는 http://호스트[:포트] 나 https://호스트[:포트] 여야 한다")
    if not url.isascii() or any(ch.isspace() for ch in url):
        raise bad
    try:
        # 괄호 호스트가 틀리면 urlsplit 이, 포트가 숫자가 아니거나 범위 밖이면 .port 가 ValueError 를 던진다. 그 문구에는
        # 값이 들어 있으므로 연쇄 없이 고정 문구로 바꾼다.
        parts = urlsplit(url)
        hostname = parts.hostname
        parts.port
    except ValueError:
        raise bad from None
    if parts.scheme not in ("http", "https"):
        raise bad
    if not hostname:
        raise bad
    if parts.netloc.startswith("["):
        try:
            ipaddress.IPv6Address(hostname)  # 형식 파싱에만 쓴다 (control_plane.md 7.1)
        except ValueError:
            raise bad from None
    elif _HOSTNAME.fullmatch(hostname) is None:
        raise bad
    if parts.username is not None or parts.password is not None:
        raise bad
    if parts.path not in ("", "/") or parts.query or parts.fragment:
        raise bad


STORE_FAILED = ("저장소 클라이언트를 만들지 못했다. SANGTACHI_CP_TABLE, AWS_REGION, SANGTACHI_CP_ENDPOINT 를 "
                "확인한다")


def build_store(environ: Mapping[str, str]):
    """Store.from_env. 설정 검사를 통과한 뒤에도 클라이언트 생성이 값을 담은 예외를 낼 수 있다(boto3 의 리전, 엔드포인트
    오류). 어떤 예외든 값 없는 고정 문구의 ConfigError 로 바꾼다. 다음에 검사의 구멍이 또 있어도 값이 새지 않는다."""
    from .store import Store  # 실패하면 ConfigError 가 아니다. main 의 경계가 기동 실패로 받는다

    try:
        return Store.from_env(environ)
    except Exception:
        raise ConfigError(STORE_FAILED) from None


def install_stop_signals(loop: asyncio.AbstractEventLoop, stop: asyncio.Event) -> Callable[[], None]:
    """SIGINT, SIGTERM, 그리고 Windows 의 SIGBREAK 가 stop 을 켜게 한다. 되돌리는 함수를 돌려준다."""
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, stop.set)
        except NotImplementedError:
            pass  # Windows. 위 모듈 설명
    if not hasattr(signal, "SIGBREAK"):
        return lambda: None
    old_break = signal.signal(signal.SIGBREAK, lambda signum, frame: loop.call_soon_threadsafe(stop.set))
    return lambda: signal.signal(signal.SIGBREAK, old_break)


async def run(server: Server, port: int, *, exit: Callable[[int], None] = os._exit) -> None:
    """serve 를 돌리고 멈추면 exit(0) 한다. 시험은 exit 를 바꿔 끼운다."""
    from .server import BIND_HOST

    stop = asyncio.Event()
    restore = install_stop_signals(asyncio.get_running_loop(), stop)
    try:
        await server.serve(BIND_HOST, port, stop)
    except asyncio.CancelledError:
        pass  # Windows 의 Ctrl+C. serve 의 finally 가 카운터를 이미 냈다
    finally:
        restore()
    sys.stdout.flush()
    sys.stderr.flush()
    exit(0)


def error_name(exc: BaseException) -> str:
    """기동 실패 문구에 붙일 이름. OSError 면 errno.errorcode 의 이름(모르면 OSError), 아니면 예외 클래스 이름.

    예외 문자열은 쓰지 않는다. bind 실패의 문자열에는 주소와 포트가 들어 있다.
    """
    if isinstance(exc, OSError):
        return errno.errorcode.get(exc.errno, "OSError") if isinstance(exc.errno, int) else "OSError"
    return type(exc).__name__


def main(environ: Mapping[str, str] | None = None) -> int:
    """main 진입부터 리슨 성공까지의 모든 줄이 아래 try 하나 안에 있다 (7.6).

    ConfigError 는 설정 오류(종료 코드 2). 그 밖의 예외는 리슨 성공 전이면 기동 실패(종료 코드 1)이고, 둘 다 고정 문구
    한 줄만 낸다. import(boto3 를 끌어오는 store, ops 포함)도 이 경계 안에서 한다. 리슨 성공 뒤의 예외는 그대로 올린다.
    이 모듈의 맨 위에는 표준 라이브러리 import 만 있다. controlplane 의 import 는 전부 이 try 안에서 일어난다
    (load_config, build_store, run 안의 import 도 이 try 안에서 불린다).
    """
    server = None
    try:
        from .server import Server

        env = os.environ if environ is None else environ
        config = load_config(env)
        # 설정을 본 뒤에 만든다. 설정 오류는 boto3 를 import 하기 전에 끝난다.
        store = build_store(env)
        from .ops import Service

        service = Service(store)
        server = Server(service.dispatch, service_counters=service.counters)
        asyncio.run(run(server, config.port))
    except ConfigError as exc:
        print(f"controlplane: 설정 오류: {exc} (control_plane.md 7.6)", file=sys.stderr)
        return EXIT_CONFIG
    except KeyboardInterrupt:
        pass
    except Exception as exc:
        if server is not None and server.listening:
            raise  # 리슨이 성공한 뒤의 실패. 처리를 바꾸지 않는다
        # 설정 검사 뒤, 리슨 성공 전의 기동 실패 (7.6). 값과 traceback 없이 이름만 낸다.
        print(f"controlplane: 기동 실패: {error_name(exc)} (control_plane.md 7.6)", file=sys.stderr)
        return EXIT_START
    return 0

if __name__ == "__main__":
    sys.exit(main())
