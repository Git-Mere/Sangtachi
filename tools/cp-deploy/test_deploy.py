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
                user="ubuntu")
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
]


@pytest.mark.parametrize("over,ok", ARGS, ids=[f"a{i}" for i in range(len(ARGS))])
def test_check_args(tmp_path, over, ok):
    n = ns(tmp_path, **over)
    if ok:
        assert deploy.check_args(n) is n
    else:
        with pytest.raises(deploy.DeployError):
            deploy.check_args(n)
