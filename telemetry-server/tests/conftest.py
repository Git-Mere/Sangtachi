"""시험이 어느 telemetry 를 import 하는지 정하고, 저장소 시험의 하네스를 둔다.

변이 시험(tests/mutate.py)은 telemetry 를 임시 폴더에 복사해 고친 뒤 TM_SRC 로 넘긴다. 실제로 import 된 위치가
TM_SRC 아래가 아니면 시험 전체를 멈춘다. controlplane 은 레포의 control-server 것을 읽는다.

AWS 격리와 루프백 검사는 control-server/tests/conftest.py 의 것을 그대로 쓴다.
"""

import importlib.util
import os
import sys
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CP_ROOT = ROOT.parent / "control-server"
_SRC = Path(os.environ.get("TM_SRC") or ROOT).resolve()
sys.path.insert(0, str(CP_ROOT))
sys.path.insert(0, str(_SRC))

import pytest  # noqa: E402
import telemetry  # noqa: E402

_loaded = Path(telemetry.__file__).resolve()
if not _loaded.is_relative_to(_SRC):
    raise RuntimeError(f"telemetry 를 {_SRC} 가 아니라 {_loaded} 에서 읽었다")

_spec = importlib.util.spec_from_file_location("cp_conftest", CP_ROOT / "tests" / "conftest.py")
cp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cp)

pytest_addoption = cp.pytest_addoption
pytest_configure = cp.pytest_configure
pytest_collection_modifyitems = cp.pytest_collection_modifyitems


@pytest.fixture(autouse=True)
def aws_isolation(monkeypatch, tmp_path):
    with cp.isolate_aws(monkeypatch, tmp_path):
        yield


@pytest.fixture
def tables(request, aws_isolation):
    """테스트마다 새 테이블 둘. 텔레메트리 테이블(pk S, seq N)과 방 대조로 읽는 제어 평면 테이블(pk S, sk S).

    돌려주는 것은 (시험 대상 dynamodb 클라이언트, 텔레메트리 테이블, 제어 평면 테이블, 관리용 클라이언트) 다.
    """
    if request.config.store_endpoint is None:
        pytest.fail("--store 없이 tables 픽스처를 불렀다")
    if request.node.get_closest_marker("store") is None:
        pytest.fail("store 표시 없는 시험이 tables 픽스처를 불렀다")
    from botocore.exceptions import BotoCoreError

    endpoint = request.config.store_endpoint
    tag = uuid.uuid4().hex
    tm, cpt = "tm-test-" + tag, "tm-cp-test-" + tag
    admin = cp.isolated_session().client(
        "dynamodb", region_name=cp.FAKE_REGION, endpoint_url=endpoint,
        config=cp.no_proxy_config(connect_timeout=2, read_timeout=10, retries={"max_attempts": 5, "mode": "standard"}))
    try:
        admin.create_table(TableName=tm,
                           KeySchema=[{"AttributeName": "pk", "KeyType": "HASH"},
                                      {"AttributeName": "seq", "KeyType": "RANGE"}],
                           AttributeDefinitions=[{"AttributeName": "pk", "AttributeType": "S"},
                                                 {"AttributeName": "seq", "AttributeType": "N"}],
                           ProvisionedThroughput={"ReadCapacityUnits": 5, "WriteCapacityUnits": 5})
    except BotoCoreError as exc:
        pytest.fail(f"--store 인데 {endpoint} 에 닿지 않는다: {type(exc).__name__}")
    try:
        admin.create_table(TableName=cpt,
                           KeySchema=[{"AttributeName": "pk", "KeyType": "HASH"},
                                      {"AttributeName": "sk", "KeyType": "RANGE"}],
                           AttributeDefinitions=[{"AttributeName": "pk", "AttributeType": "S"},
                                                 {"AttributeName": "sk", "AttributeType": "S"}],
                           ProvisionedThroughput={"ReadCapacityUnits": 5, "WriteCapacityUnits": 5})
        client = cp.isolated_session().client("dynamodb", region_name=cp.FAKE_REGION, endpoint_url=endpoint,
                                              config=cp.no_proxy_config())
        if cp.proxy_for(client, endpoint) is not None:
            pytest.fail("저장소 시험의 클라이언트가 프록시를 쓴다")
        yield client, tm, cpt, admin
    finally:
        for t in (tm, cpt):
            try:
                admin.delete_table(TableName=t)
            except Exception:
                pass
