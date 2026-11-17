"""설정과 기동 (architecture.md 3.4 설정과 배포, 읽는 규칙은 control_plane.md 7.6)."""

import pytest

from controlplane.__main__ import ConfigError
from telemetry import __main__ as main_mod

GOOD = {"SANGTACHI_TM_TABLE": "tm", "SANGTACHI_TM_CP_TABLE": "cp", "AWS_REGION": "us-west-2"}

CONFIG_CASES = [
    ("defaults", {}, (8001, None)),
    ("port", {"SANGTACHI_TM_PORT": "18001"}, (18001, None)),
    ("endpoint", {"SANGTACHI_TM_ENDPOINT": "http://127.0.0.1:8001"}, (8001, "http://127.0.0.1:8001")),
    ("empty-endpoint", {"SANGTACHI_TM_ENDPOINT": ""}, (8001, None)),
    ("port-empty", {"SANGTACHI_TM_PORT": ""}, "SANGTACHI_TM_PORT"),
    ("port-zero", {"SANGTACHI_TM_PORT": "0"}, "SANGTACHI_TM_PORT"),
    ("port-big", {"SANGTACHI_TM_PORT": "65536"}, "SANGTACHI_TM_PORT"),
    ("port-plus", {"SANGTACHI_TM_PORT": "+8001"}, "SANGTACHI_TM_PORT"),
    ("no-table", {"SANGTACHI_TM_TABLE": None}, "SANGTACHI_TM_TABLE"),
    ("empty-table", {"SANGTACHI_TM_TABLE": ""}, "SANGTACHI_TM_TABLE"),
    ("no-cp-table", {"SANGTACHI_TM_CP_TABLE": None}, "SANGTACHI_TM_CP_TABLE"),
    ("no-region", {"AWS_REGION": None}, "AWS_REGION"),
    ("bad-region", {"AWS_REGION": "US WEST"}, "AWS_REGION"),
    ("bad-endpoint", {"SANGTACHI_TM_ENDPOINT": "ftp://x/secretpath"}, "SANGTACHI_TM_ENDPOINT"),
    ("cp-vars-ignored", {"SANGTACHI_TM_TABLE": None, "SANGTACHI_CP_TABLE": "x"}, "SANGTACHI_TM_TABLE"),
]


@pytest.mark.parametrize("over,expected", [c[1:] for c in CONFIG_CASES], ids=[c[0] for c in CONFIG_CASES])
def test_load_config(over, expected):
    env = dict(GOOD)
    for k, v in over.items():
        if v is None:
            env.pop(k, None)
        else:
            env[k] = v
    if isinstance(expected, str):
        with pytest.raises(ConfigError) as e:
            main_mod.load_config(env)
        assert expected in str(e.value)
        assert "secretpath" not in str(e.value)
    else:
        c = main_mod.load_config(env)
        assert (c.port, c.endpoint) == expected
        assert (c.table, c.cp_table, c.region) == ("tm", "cp", "us-west-2")


def test_main_config_error_exit_2(capsys):
    assert main_mod.main({}) == 2
    err = capsys.readouterr().err
    assert err.startswith("telemetry: 설정 오류: SANGTACHI_TM_TABLE") and err.count("\n") == 1


def test_clients():
    c = main_mod.load_config({**GOOD, "SANGTACHI_TM_ENDPOINT": "http://127.0.0.1:8001"})
    ddb, cw = main_mod.build_clients(c)
    assert ddb.meta.endpoint_url == "http://127.0.0.1:8001"
    assert cw.meta.endpoint_url != "http://127.0.0.1:8001"
    for client in (ddb, cw):
        cfg = client.meta.config
        assert (cfg.connect_timeout, cfg.read_timeout) == (1, 1)
        assert cfg.retries["total_max_attempts"] == 1
