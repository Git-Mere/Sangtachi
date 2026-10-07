"""control_plane.md 7.6 서버의 DynamoDB 호출 설정. 배포 테이블에서 잰 지연으로 정한 값이다."""

from controlplane import __main__ as main_mod
from controlplane import constants
from controlplane import store as store_mod


def test_build_store_passes_call_limits(monkeypatch):
    seen = {}

    def fake_from_env(environ, config=None, **kw):
        seen["config"] = config
        return "store"

    monkeypatch.setattr(store_mod.Store, "from_env", staticmethod(fake_from_env))
    assert main_mod.build_store({"SANGTACHI_CP_TABLE": "t", "AWS_REGION": "us-west-2"}) == "store"
    cfg = seen["config"]
    assert cfg.connect_timeout == constants.DDB_CONNECT_TIMEOUT_S == 1
    assert cfg.read_timeout == constants.DDB_READ_TIMEOUT_S == 1
    assert cfg.retries == {"mode": "standard", "total_max_attempts": constants.DDB_MAX_ATTEMPTS}
    assert constants.DDB_MAX_ATTEMPTS == 1
