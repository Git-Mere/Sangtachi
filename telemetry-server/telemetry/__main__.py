"""python -m telemetry. 텔레메트리 서비스 프로세스 하나 (architecture.md 3.4 설정과 배포).

설정을 읽는 규칙, 종료 코드, 기동 실패 문구, 종료 처리는 제어 서버와 같다 (control_plane.md 7.6). 그 코드는
controlplane.__main__ 을 그대로 쓴다. 여기 있는 것은 변수 이름과 boto3 클라이언트 둘, 지표 전송 주기다.
"""

from __future__ import annotations

import asyncio
import os
import sys
from collections.abc import Mapping
from dataclasses import dataclass

from controlplane.__main__ import (
    EXIT_CONFIG,
    EXIT_START,
    _PORT,
    _PORT_MAX,
    _REGION,
    ConfigError,
    check_endpoint,
    error_name,
    run,
)


@dataclass(frozen=True)
class Config:
    port: int
    table: str
    cp_table: str
    region: str
    endpoint: str | None


def load_config(environ: Mapping[str, str]) -> Config:
    from .server import TM_PORT

    raw_port = environ.get("SANGTACHI_TM_PORT")
    if raw_port is None:
        port = TM_PORT
    else:
        if _PORT.fullmatch(raw_port) is None or int(raw_port) > _PORT_MAX:
            raise ConfigError("SANGTACHI_TM_PORT 는 1~65535 의 10진수여야 한다")
        port = int(raw_port)
    table = environ.get("SANGTACHI_TM_TABLE", "")
    if not table:
        raise ConfigError("SANGTACHI_TM_TABLE 이 없다. 텔레메트리 테이블 이름을 넣는다")
    cp_table = environ.get("SANGTACHI_TM_CP_TABLE", "")
    if not cp_table:
        raise ConfigError("SANGTACHI_TM_CP_TABLE 이 없다. 방 대조로 읽는 제어 평면 테이블 이름을 넣는다")
    region = environ.get("AWS_REGION", "")
    if _REGION.fullmatch(region) is None:
        raise ConfigError("AWS_REGION 이 없거나 틀렸다. 소문자, 숫자, '-' 로 된 리전 이름을 넣는다")
    endpoint = environ.get("SANGTACHI_TM_ENDPOINT") or None
    if endpoint is not None:
        check_endpoint(endpoint, "SANGTACHI_TM_ENDPOINT")
    return Config(port, table, cp_table, region, endpoint)


CLIENTS_FAILED = "AWS 클라이언트를 만들지 못했다. AWS_REGION, SANGTACHI_TM_ENDPOINT 를 확인한다"


def build_clients(config: Config, session=None):
    """(dynamodb, cloudwatch). 시간 제한과 SDK 횟수는 제어 서버의 DynamoDB 호출과 같다 (control_plane.md 7.6).
    엔드포인트 덮어쓰기는 DynamoDB 에만 건다. 예외는 값 없는 고정 문구로 바꾼다."""
    import boto3.session
    from botocore.config import Config as BotoConfig

    from controlplane.constants import DDB_CONNECT_TIMEOUT_S, DDB_MAX_ATTEMPTS, DDB_READ_TIMEOUT_S

    boto = BotoConfig(connect_timeout=DDB_CONNECT_TIMEOUT_S, read_timeout=DDB_READ_TIMEOUT_S,
                      retries={"mode": "standard", "total_max_attempts": DDB_MAX_ATTEMPTS})
    try:
        session = session or boto3.session.Session()
        dynamodb = session.client("dynamodb", region_name=config.region, endpoint_url=config.endpoint, config=boto)
        cloudwatch = session.client("cloudwatch", region_name=config.region, config=boto)
    except Exception:
        raise ConfigError(CLIENTS_FAILED) from None
    return dynamodb, cloudwatch


async def flush_forever(metrics, period_s: float) -> None:
    while True:
        await asyncio.sleep(period_s)
        await asyncio.to_thread(metrics.flush)


async def serve(server, metrics, port: int) -> None:
    from .metrics import FLUSH_PERIOD_S

    flusher = asyncio.create_task(flush_forever(metrics, FLUSH_PERIOD_S))
    try:
        await run(server, port)
    finally:
        flusher.cancel()


def main(environ: Mapping[str, str] | None = None) -> int:
    """경계는 controlplane.__main__.main 과 같다. 리슨 성공 전의 실패는 이름 하나만 내고 끝난다."""
    server = None
    try:
        from .ingest import Ingest
        from .metrics import Metrics
        from .server import build_server

        env = os.environ if environ is None else environ
        config = load_config(env)
        dynamodb, cloudwatch = build_clients(config)
        metrics = Metrics(cloudwatch)
        ingest = Ingest(dynamodb, config.table, config.cp_table, metrics)
        server = build_server(ingest.dispatch, metrics.counters)
        asyncio.run(serve(server, metrics, config.port))
    except ConfigError as exc:
        print(f"telemetry: 설정 오류: {exc} (architecture.md 3.4)", file=sys.stderr)
        return EXIT_CONFIG
    except KeyboardInterrupt:
        pass
    except Exception as exc:
        if server is not None and server.listening:
            raise
        print(f"telemetry: 기동 실패: {error_name(exc)} (architecture.md 3.4)", file=sys.stderr)
        return EXIT_START
    return 0


if __name__ == "__main__":
    sys.exit(main())
