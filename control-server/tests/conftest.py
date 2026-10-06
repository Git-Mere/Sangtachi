"""시험이 어느 controlplane 을 import 하는지 정하고, 저장소 시험의 하네스를 둔다.

변이 시험(tests/mutate.py)은 원본을 임시 폴더에 복사해 고친 뒤 CP_SRC 로 그 폴더를 넘긴다.
원본이 대신 import 되면 변이가 조용히 살아남은 것처럼 보이므로, 실제로 import 된 위치를
확인하고 어긋나면 시험 전체를 멈춘다.
"""

import contextlib
import os
import sys
import uuid
from pathlib import Path
from urllib.parse import urlsplit

_SRC = Path(os.environ.get("CP_SRC") or Path(__file__).resolve().parents[1]).resolve()
sys.path.insert(0, str(_SRC))

import controlplane  # noqa: E402
import pytest  # noqa: E402

_loaded = Path(controlplane.__file__).resolve()
if not _loaded.is_relative_to(_SRC):
    raise RuntimeError(f"controlplane 을 {_SRC} 가 아니라 {_loaded} 에서 읽었다")


def pytest_collection_modifyitems(config, items):
    """건너뛰는 행은 store 표시가 있는 것뿐이어야 한다. 다른 이유의 skip 은 수집에서 멈춘다."""
    bad = [
        item.nodeid
        for item in items
        if any(item.iter_markers(name="skip")) or any(item.iter_markers(name="skipif"))
        if not any(item.iter_markers(name="store"))
    ]
    if bad:
        raise pytest.UsageError(f"store 표시 없이 건너뛰는 시험이 있다: {bad}")
    _skip_store_items(config, items)


# ---------------------------------------------------------------- 저장소 하네스
#
# --store 를 주면 store 표시가 있는 시험이 DynamoDB local 에서 돈다. 주지 않으면 건너뛴다.
# 엔드포인트는 루프백이어야 한다 (control_plane.md 7.6). 허용 목록이고 모르는 호스트는 거부한다.

LOOPBACK_HOSTS = frozenset({"127.0.0.1", "localhost", "::1"})
DEFAULT_STORE_ENDPOINT = "http://127.0.0.1:8001"
# DynamoDB local 3.x 는 키 ID 에 영문자와 숫자만 받는다. 아니면 UnrecognizedClientException 이다.
FAKE_KEY_ID = "fakeLocalOnlyNotAKey"
FAKE_SECRET = "fakeLocalOnlyNotASecret"
FAKE_REGION = "local-only"  # 엔드포인트가 빠져도 실제 AWS 호스트로 해석되지 않는 이름


def pytest_addoption(parser):
    parser.addoption("--store", action="store_true", default=False,
                     help="store 표시가 있는 시험을 DynamoDB local 에서 돌린다")
    parser.addoption("--store-endpoint", default=DEFAULT_STORE_ENDPOINT,
                     help="DynamoDB local 의 URL. 루프백만 받는다")


def loopback_endpoint(url: str) -> str:
    """루프백 URL 을 검사하고 검사한 조각으로 다시 만든 URL 을 돌려준다. 아니면 ValueError.

    urlsplit 은 탭과 줄바꿈을 지우고 읽으므로 받은 문자열을 그대로 넘기지 않는다.
    """
    if not isinstance(url, str) or not url.isascii() or any(ch.isspace() for ch in url):
        raise ValueError("엔드포인트에 공백이나 ASCII 밖의 글자가 있다")
    parts = urlsplit(url)
    if parts.scheme != "http":
        raise ValueError("http 만 받는다")
    if parts.username is not None or parts.password is not None or parts.query or parts.fragment:
        raise ValueError("사용자 정보, 질의, 조각을 받지 않는다")
    if parts.path not in ("", "/"):
        raise ValueError("경로를 받지 않는다")
    host = parts.hostname
    if host not in LOOPBACK_HOSTS:
        raise ValueError(f"루프백이 아니다: {host!r}")
    port = parts.port  # 숫자가 아니거나 범위 밖이면 ValueError
    if port is None or port == 0:
        raise ValueError("포트를 적어야 한다")
    return f"http://[{host}]:{port}" if ":" in host else f"http://{host}:{port}"


def pytest_configure(config):
    config.store_endpoint = None
    if not config.getoption("--store"):
        return
    try:
        config.store_endpoint = loopback_endpoint(config.getoption("--store-endpoint"))
    except ValueError as exc:
        raise pytest.UsageError(f"--store-endpoint 를 받을 수 없다: {exc}") from None


def _skip_store_items(config, items):
    if config.store_endpoint is not None:
        return
    skip = pytest.mark.skip(reason="--store 가 없다. 저장소 시험을 건너뛴다")
    for item in items:
        if any(item.iter_markers(name="store")):
            item.add_marker(skip)


# 모든 시험에 건다. boto3 클라이언트를 만드는 것만으로도 사용자 프로파일, credential_process, IMDS,
# 프록시 설정을 읽을 수 있다. 저장소에 닿지 않는 시험(설정 시험)도 같다. 그래서 하나의 픽스처가
#   - AWS_ 로 시작하는 환경 변수를 전부 지우고 (허용 목록. 모르는 변수를 남기지 않는다)
#   - 빈 설정 파일과 빈 자격 증명 파일을 가리키게 하고
#   - 가짜 자격 증명과 AWS_EC2_METADATA_DISABLED=true 를 넣고
#   - 프록시 변수를 지우고 NO_PROXY 에 루프백을 넣고 (Windows 시스템 프록시도 NO_PROXY 로 우회한다)
#   - boto3.DEFAULT_SESSION 을 비운 뒤 시험이 끝나면 되돌린다.
# 가짜 자격 증명은 pytest_configure 의 루프백 검사 뒤에만 들어간다. 시험이 그 뒤에 돌기 때문이다.
PROXY_VARS = ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy")
NO_PROXY_VALUE = "127.0.0.1,localhost,::1"


@contextlib.contextmanager
def isolate_aws(monkeypatch, tmp_path):
    import boto3

    for name in list(os.environ):
        if name.upper().startswith("AWS_"):
            monkeypatch.delenv(name, raising=False)
    for name in PROXY_VARS:
        monkeypatch.delenv(name, raising=False)
    config_file = tmp_path / "aws-config-empty"
    credentials_file = tmp_path / "aws-credentials-empty"
    config_file.write_text("", encoding="ascii")
    credentials_file.write_text("", encoding="ascii")
    monkeypatch.setenv("AWS_CONFIG_FILE", str(config_file))
    monkeypatch.setenv("AWS_SHARED_CREDENTIALS_FILE", str(credentials_file))
    monkeypatch.setenv("AWS_ACCESS_KEY_ID", FAKE_KEY_ID)
    monkeypatch.setenv("AWS_SECRET_ACCESS_KEY", FAKE_SECRET)
    monkeypatch.setenv("AWS_EC2_METADATA_DISABLED", "true")
    monkeypatch.setenv("NO_PROXY", NO_PROXY_VALUE)
    monkeypatch.setenv("no_proxy", NO_PROXY_VALUE)
    saved = boto3.DEFAULT_SESSION
    boto3.DEFAULT_SESSION = None
    try:
        yield
    finally:
        boto3.DEFAULT_SESSION = saved


@pytest.fixture(autouse=True)
def aws_isolation(monkeypatch, tmp_path):
    with isolate_aws(monkeypatch, tmp_path):
        yield


def no_proxy_config(**kwargs):
    """시험이 만드는 클라이언트의 Config. 프록시를 끈다. 빈 dict 는 botocore 가 환경을 보지 않게 한다."""
    from botocore.config import Config

    return Config(proxies={}, **kwargs)


_LOADER = []


def isolated_session():
    """새 boto3 Session. 자격 증명과 설정은 새로 읽고, 서비스 모델 로더만 나눠 쓴다 (시험 속도).
    로더는 botocore 가 내려받은 JSON 모델을 읽는 것뿐이고 자격 증명을 들고 있지 않다.
    """
    import boto3.session
    import botocore.loaders
    import botocore.session

    if not _LOADER:
        _LOADER.append(botocore.loaders.create_loader())
    core = botocore.session.get_session()
    core.register_component("data_loader", _LOADER[0])
    return boto3.session.Session(botocore_session=core)


def proxy_for(client, url: str):
    """그 클라이언트가 url 에 쓸 프록시. 없으면 None. botocore 내부 속성을 읽는다 (시험 전용)."""
    return client._endpoint.http_session._proxy_config.proxy_url_for(url)


@pytest.fixture
def store(request, monkeypatch, aws_isolation):
    """테스트마다 새 테이블 하나. control_plane.md 6.2 테이블의 키와 TTL, 7.6 의 용량 25/25.

    자격 증명과 프록시 격리는 aws_isolation 이 이미 했다. 여기서는 엔드포인트와 테이블만 정한다.
    """
    if request.config.store_endpoint is None:
        pytest.fail("--store 없이 store 픽스처를 불렀다")
    if request.node.get_closest_marker("store") is None:
        pytest.fail("store 표시 없는 시험이 store 픽스처를 불렀다")
    from botocore.exceptions import BotoCoreError

    from controlplane.store import Store

    endpoint = request.config.store_endpoint
    table = "cp-test-" + uuid.uuid4().hex
    monkeypatch.setenv("AWS_REGION", FAKE_REGION)
    monkeypatch.setenv("SANGTACHI_CP_TABLE", table)
    monkeypatch.setenv("SANGTACHI_CP_ENDPOINT", endpoint)
    admin = isolated_session().client(
        "dynamodb", region_name=FAKE_REGION, endpoint_url=endpoint,
        # DynamoDB local 은 CreateTable 이 동시에 몰리면 InternalFailure(500)를 낸다. 변이 실행기의 -j 에서
        # 실측했다. 관리용 클라이언트만 표준 재시도로 감싼다. 시험 대상 store 의 클라이언트는 건드리지 않는다.
        config=no_proxy_config(connect_timeout=2, read_timeout=10, retries={"max_attempts": 5, "mode": "standard"}))
    try:
        admin.create_table(
            TableName=table,
            KeySchema=[{"AttributeName": "pk", "KeyType": "HASH"}, {"AttributeName": "sk", "KeyType": "RANGE"}],
            AttributeDefinitions=[{"AttributeName": "pk", "AttributeType": "S"},
                                  {"AttributeName": "sk", "AttributeType": "S"}],
            ProvisionedThroughput={"ReadCapacityUnits": 25, "WriteCapacityUnits": 25})
    except BotoCoreError as exc:
        pytest.fail(f"--store 인데 {endpoint} 에 닿지 않는다: {type(exc).__name__}")
    try:
        admin.update_time_to_live(TableName=table,
                                  TimeToLiveSpecification={"Enabled": True, "AttributeName": "ttl"})
        st = Store.from_env(config=no_proxy_config(), session=isolated_session())
        # 시험 대상이 루프백이 아닌 곳으로 가면 여기서 멈춘다. 설정 경로가 엔드포인트를 버리는 결함도 이것이 막는다.
        if st.client.meta.endpoint_url != endpoint:
            pytest.fail(f"store 의 엔드포인트가 {endpoint} 가 아니다")
        if proxy_for(st.client, endpoint) is not None or proxy_for(admin, endpoint) is not None:
            pytest.fail("store 시험의 클라이언트가 프록시를 쓴다")
        yield st
    finally:
        admin.delete_table(TableName=table)
