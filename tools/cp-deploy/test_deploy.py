"""deploy.py 의 입력 검사. 판정 함수라 케이스 표를 먼저 둔다(CLAUDE.md 규칙 7)."""

import argparse
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import deploy  # noqa: E402

HOSTS = [
    ("100.20.98.101", True, "공인 유니캐스트"),
    ("8.8.8.8", True, "공인 유니캐스트"),
    ("10.0.0.5", False, "사설"),
    ("172.31.40.109", False, "사설. 인스턴스의 사설 주소를 잘못 주는 경우"),
    ("127.0.0.1", False, "루프백"),
    ("100.64.0.1", False, "CGNAT 공유 대역"),
    ("192.0.2.1", False, "문서용 대역"),
    ("224.0.0.1", False, "멀티캐스트. is_global 이 참인 대역이다"),
    ("0.0.0.0", False, "미지정"),
    ("255.255.255.255", False, "브로드캐스트"),
    ("010.020.098.101", False, "선행 0. 정규 표기가 아니다"),
    ("100.20.98", False, "옥텟 셋"),
    ("ec2-1-2-3-4.compute.amazonaws.com", False, "이름은 받지 않는다"),
    ("::1", False, "IPv6"),
    ("100.20.98.101 ", False, "뒤 공백"),
    ("", False, "빈 값"),
]


@pytest.mark.parametrize("host,ok,note", HOSTS, ids=[f"h{i}" for i in range(len(HOSTS))])
def test_check_host(host, ok, note):
    if ok:
        assert deploy.check_host(host) == host
    else:
        with pytest.raises(deploy.DeployError):
            deploy.check_host(host)


def ns(tmp_path, **over):
    key = tmp_path / "k.pem"
    key.write_text("x")
    base = dict(host="100.20.98.101", key=str(key), table="sangtachi-control-plane", region="us-west-2",
                user="ubuntu", target="cp", cp_table=None)
    base.update(over)
    return argparse.Namespace(**base)


ARGS = [
    ({}, True),
    ({"table": "ab"}, False),
    ({"table": "a b c"}, False),
    ({"table": "x;rm -rf /"}, False),
    ({"region": "us-west"}, False),
    ({"region": "US-WEST-2"}, False),
    ({"region": "us-west-2; id"}, False),
    ({"user": "root$(id)"}, False),
    ({"key": "no-such-file.pem"}, False),
    ({"cp_table": "sangtachi-control-plane"}, False),
    ({"target": "tm", "table": "sangtachi-telemetry", "cp_table": "sangtachi-control-plane"}, True),
    ({"target": "tm", "table": "sangtachi-telemetry"}, False),
    ({"target": "tm", "table": "sangtachi-telemetry", "cp_table": "x;id"}, False),
    ({"target": "tm", "table": "a b", "cp_table": "sangtachi-control-plane"}, False),
]


@pytest.mark.parametrize("over,ok", ARGS, ids=[f"a{i}" for i in range(len(ARGS))])
def test_check_args(tmp_path, over, ok):
    n = ns(tmp_path, **over)
    if ok:
        assert deploy.check_args(n) is n
    else:
        with pytest.raises(deploy.DeployError):
            deploy.check_args(n)


# ---------------------------------------------------------------- 대상 (architecture.md 3.4 같은 인스턴스의 자원, 설정과 배포)

LIMITS = ("MemoryMax=256M", "CPUQuota=20%", "IOWeight=50")


def unit_lines(target):
    return deploy.unit_text(deploy.TARGETS[target]).splitlines()


def test_cp_unit_has_no_limits():
    lines = unit_lines("cp")
    assert not any(line.split("=")[0] in {l.split("=")[0] for l in LIMITS} for line in lines)
    assert "ExecStart=/opt/sangtachi-cp/current/.venv/bin/python -m controlplane" in lines
    assert "User=sangtachi-cp" in lines and "EnvironmentFile=/etc/sangtachi-cp.env" in lines
    assert "Restart=always" in lines


def test_tm_unit():
    lines = unit_lines("tm")
    service = lines[lines.index("[Service]"):lines.index("[Install]")]
    for limit in LIMITS:
        assert limit in service
    assert "ExecStart=/opt/sangtachi-tm/current/.venv/bin/python -m telemetry" in lines
    assert "User=sangtachi-tm" in lines and "Group=sangtachi-tm" in lines
    assert "EnvironmentFile=/etc/sangtachi-tm.env" in lines
    assert "Restart=always" in lines


def test_env_files(tmp_path):
    cp = deploy.check_args(ns(tmp_path))
    assert deploy.env_text(cp) == "SANGTACHI_CP_TABLE=sangtachi-control-plane\nAWS_REGION=us-west-2\n"
    tm = deploy.check_args(ns(tmp_path, target="tm", table="sangtachi-telemetry", cp_table="sangtachi-control-plane"))
    assert deploy.env_text(tm) == ("SANGTACHI_TM_TABLE=sangtachi-telemetry\nSANGTACHI_TM_CP_TABLE=sangtachi-control-plane\n"
                                   "AWS_REGION=us-west-2\n")


def test_upload_files(tmp_path):
    cp = deploy.check_args(ns(tmp_path))
    assert set(deploy.upload_files(cp, deploy.TARGETS["cp"])) == {"env", "unit"}
    tm = deploy.check_args(ns(tmp_path, target="tm", table="sangtachi-telemetry", cp_table="sangtachi-control-plane"))
    files = deploy.upload_files(tm, deploy.TARGETS["tm"])
    assert files["journald"] == "[Journal]\nSystemMaxUse=200M\nRuntimeMaxUse=200M\n"


def test_targets_ship_their_code():
    root = deploy.REPO
    assert deploy.TARGETS["cp"].paths == ("control-server/controlplane", "control-server/requirements.txt")
    tm = deploy.TARGETS["tm"]
    # 텔레메트리는 controlplane 을 import 한다. 원격은 앞 폴더 하나를 벗기므로 패키지 둘이 릴리스 바로 아래에 놓인다
    assert {p.split("/", 1)[1] for p in tm.paths} == {"controlplane", "telemetry", "requirements.txt"}
    for target in deploy.TARGETS.values():
        for p in target.paths:
            assert (root / p).exists(), p
    assert (deploy.TARGETS["cp"].port, tm.port) == (8000, 8001)


def test_remote_script_has_no_fixed_service():
    assert "sangtachi-cp" not in deploy.REMOTE_SCRIPT
    assert 'SERVICE="$2"' in deploy.REMOTE_SCRIPT


def test_tm_smoke_body_is_rejected_by_check():
    """연기 시험은 저장소에 쓰지 않아야 한다. 텔레메트리의 레코드 검사가 bad_request 로 끝내는지 본다."""
    import json

    sys.path.insert(0, str(deploy.REPO / "control-server"))
    sys.path.insert(0, str(deploy.REPO / "telemetry-server"))
    from controlplane.errors import OpError
    from telemetry.ingest import check_record

    with pytest.raises(OpError) as e:
        check_record(json.loads(deploy.TM_SMOKE_BODY))
    assert e.value.code == "bad_request"


def test_verify_load_record_is_valid_and_out_of_metrics():
    import verify
    from telemetry.ingest import check_record
    from telemetry.metrics import samples

    r = check_record(verify.load_record("0" * 32, 3))
    assert (r["role"], r["kind"], r["room_id"], r["build_kind"]) == ("host", "interval", None, "test")
    r["room_known"] = None
    assert samples(r) == []
