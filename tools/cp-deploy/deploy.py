"""제어 서버나 텔레메트리 서비스를 EC2 한 대에 배포한다. 출처는 docs/kor/control_plane.md 7.6 설정과 배포,
docs/kor/architecture.md 3.4 텔레메트리 서비스의 같은 인스턴스의 자원과 설정과 배포다.

쓰는 법 (레포 루트에서, Git Bash 나 PowerShell):

    python tools/cp-deploy/deploy.py --host 203.0.113.10 --key path/to/key.pem \
        --table my-table --region us-west-2
    python tools/cp-deploy/deploy.py --target tm --host 203.0.113.10 --key path/to/key.pem \
        --table my-telemetry-table --cp-table my-table --region us-west-2

하는 일:
    1. 입력을 검사한다. 모르면 멈춘다
    2. 배포할 파일이 커밋된 상태인지 본다. 작업 트리의 고친 내용은 배포하지 않는다
    3. git archive HEAD 로 대상의 경로(TARGETS)를 묶는다. 유닛과 설정 파일의 내용은 여기서 만든다
    4. 원격에 이 배포만의 임시 폴더를 만들어 scp 로 올리고, ssh 로 원격 스크립트(REMOTE_SCRIPT)를 돌린다.
       원격 스크립트는 끝나거나 끊기면 임시 폴더와 반쯤 만든 릴리스를 지운다
       - python3-venv 가 없으면 apt 로 설치한다
       - 대상의 시스템 사용자를 만든다(없을 때만)
       - /opt/<서비스>/releases/<커밋> 에 풀고 venv 를 만들어 requirements.txt 를 설치한다
       - /etc/<서비스>.env 와 systemd 유닛을 새 파일에 쓴 뒤 이름을 바꿔 한 번에 바꾼다
       - /opt/<서비스>/current 를 그 릴리스로 바꾸고 서비스를 다시 띄운다
       - 이번 실행(InvocationID)의 journal 에 server.started 가 나오는지 기다린다
       - 설정을 바꾸기 시작한 뒤 어디서든 실패하거나 끊기면 앞 릴리스와 앞 설정으로 되돌린다. 처음
         배포였으면 서비스를 멈추고 지운다
       - 텔레메트리 대상은 서비스가 뜬 뒤 journald 전체 상한 드롭인을 쓴다(인스턴스 전체 설정)
    5. 이 기기에서 그 주소의 TCP 포트로 요청 하나를 보내 응답을 본다. 저장소에 쓰지 않는 요청이다

자격 증명은 다루지 않는다. 인스턴스의 IAM 역할을 쓴다(control_plane.md 7.6). 공개 레포라 주소,
키 경로, 테이블 이름은 인자로만 받는다.
"""

from __future__ import annotations

import argparse
import ipaddress
import json
import re
import shlex
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


@dataclass(frozen=True)
class Target:
    service: str             # systemd 서비스, 시스템 사용자, /opt 와 /etc 의 이름
    paths: tuple[str, ...]   # git archive 할 경로. 원격이 앞 폴더 하나를 벗겨 릴리스 폴더 바로 아래에 둔다
    module: str              # python -m 으로 띄우는 패키지
    port: int                # 유닛은 포트 변수를 주지 않아 기본값을 쓴다
    limits: tuple[str, ...]  # 유닛의 자원 제한 줄
    description: str


TARGETS = {
    # control_plane.md 7.6. 제어 서버 유닛에는 자원 제한을 걸지 않는다(architecture.md 3.4 같은 인스턴스의 자원).
    "cp": Target("sangtachi-cp", ("control-server/controlplane", "control-server/requirements.txt"),
                 "controlplane", 8000, (), "Sangtachi control plane (docs/kor/control_plane.md)"),
    # architecture.md 3.4. 텔레메트리는 제어 서버의 HTTP 와 수락 루프 코드를 import 하므로 controlplane 을 같이 싣는다.
    "tm": Target("sangtachi-tm", ("control-server/controlplane", "telemetry-server/telemetry",
                                  "telemetry-server/requirements.txt"),
                 "telemetry", 8001, ("MemoryMax=256M", "CPUQuota=20%", "IOWeight=50"),
                 "Sangtachi telemetry (docs/kor/architecture.md 3.4)"),
}
PORT = TARGETS["cp"].port  # control_plane.md 2.6 CONTROL_PORT

# architecture.md 3.4 같은 인스턴스의 자원. journald 전체 사용량. 영속(/var/log/journal)과 휘발(/run/log/journal) 둘 다 묶는다. 인스턴스 전체 설정이라 제어 서버 로그도 같이 묶인다.
JOURNALD_DROPIN = "[Journal]\nSystemMaxUse=200M\nRuntimeMaxUse=200M\n"

_TABLE = re.compile(r"[A-Za-z0-9_.-]{3,255}")       # DynamoDB 테이블 이름 규칙
_REGION = re.compile(r"[a-z]{2}(-[a-z]+)+-[0-9]")  # us-west-2 꼴
_USER = re.compile(r"[a-z_][a-z0-9_-]{0,31}")
_SHA = re.compile(r"[0-9a-f]{40}")


class DeployError(Exception):
    pass


def check_host(text: str) -> str:
    """공인 IPv4 리터럴만 받는다. 이름은 받지 않는다(control_plane.md 7.6 공인 주소)."""
    try:
        addr = ipaddress.IPv4Address(text)
    except ValueError:
        raise DeployError("--host 는 IPv4 리터럴이어야 한다") from None
    if str(addr) != text:
        raise DeployError("--host 는 정규 표기의 IPv4 리터럴이어야 한다")
    # 허용 목록: 전역 유니캐스트로 보이는 것만. is_global 은 멀티캐스트에 참이라 따로 뺀다(CLAUDE.md 규칙 7).
    if not addr.is_global or addr.is_multicast:
        raise DeployError("--host 는 공인 유니캐스트 주소여야 한다")
    return text


def check_args(ns: argparse.Namespace) -> argparse.Namespace:
    check_host(ns.host)
    key = Path(ns.key)
    if not key.is_file():
        raise DeployError("--key 파일이 없다")
    if not _TABLE.fullmatch(ns.table):
        raise DeployError("--table 은 DynamoDB 테이블 이름 형식이어야 한다")
    if ns.target == "tm":
        if ns.cp_table is None or not _TABLE.fullmatch(ns.cp_table):
            raise DeployError("--target tm 은 --cp-table 에 DynamoDB 테이블 이름을 받아야 한다")
    elif ns.cp_table is not None:
        raise DeployError("--cp-table 은 --target tm 에만 쓴다")
    if not _REGION.fullmatch(ns.region):
        raise DeployError("--region 은 us-west-2 꼴이어야 한다")
    if not _USER.fullmatch(ns.user):
        raise DeployError("--user 형식이 아니다")
    return ns


def run(cmd: list[str], *, input_text: str | None = None, timeout: float = 600) -> str:
    # 입력은 바이트로 넘긴다. 텍스트 모드는 Windows 에서 줄 끝을 CRLF 로 바꿔 원격 bash 가 깨진다(실측).
    data = input_text.encode("utf-8") if input_text is not None else None
    proc = subprocess.run(cmd, input=data, capture_output=True, timeout=timeout)
    out = proc.stdout.decode("utf-8", "replace")
    if proc.returncode != 0:
        tail = (out + proc.stderr.decode("utf-8", "replace"))[-2000:]
        raise DeployError(f"명령 실패 ({proc.returncode}): {cmd[0]} ...\n{tail}")
    return out


def env_text(ns: argparse.Namespace) -> str:
    """설정 파일 내용. 변수는 control_plane.md 7.6 과 architecture.md 3.4 설정과 배포의 표다."""
    if ns.target == "tm":
        return f"SANGTACHI_TM_TABLE={ns.table}\nSANGTACHI_TM_CP_TABLE={ns.cp_table}\nAWS_REGION={ns.region}\n"
    return f"SANGTACHI_CP_TABLE={ns.table}\nAWS_REGION={ns.region}\n"


def unit_text(target: Target) -> str:
    base = f"/opt/{target.service}"
    limits = "".join(line + "\n" for line in target.limits)
    return f"""[Unit]
Description={target.description}
After=network-online.target
Wants=network-online.target

[Service]
User={target.service}
Group={target.service}
EnvironmentFile=/etc/{target.service}.env
WorkingDirectory={base}/current
ExecStart={base}/current/.venv/bin/python -m {target.module}
Restart=always
RestartSec=2
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
PrivateDevices=yes
{limits}
[Install]
WantedBy=multi-user.target
"""


def committed_sha(target: Target) -> str:
    """배포할 경로에 커밋하지 않은 변경이 없을 때만 HEAD 를 돌려준다."""
    dirty = run(["git", "-C", str(REPO), "status", "--porcelain", "--", *target.paths])
    if dirty.strip():
        raise DeployError("배포할 경로에 커밋하지 않은 변경이 있다:\n" + dirty)
    sha = run(["git", "-C", str(REPO), "rev-parse", "HEAD"]).strip()
    if not _SHA.fullmatch(sha):
        raise DeployError("HEAD 를 읽지 못했다")
    return sha


def ssh_base(ns: argparse.Namespace) -> list[str]:
    return ["ssh", "-i", ns.key, "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", f"{ns.user}@{ns.host}"]


REMOTE_SCRIPT = r"""
set -euo pipefail
SHA="$1"; SERVICE="$2"; UPDIR="$3"
TARBALL="$UPDIR/app.tar.gz"
BASE="/opt/$SERVICE"
REL="$BASE/releases/$SHA"
ENVF="/etc/$SERVICE.env"
JOURNALD_DROPIN=/etc/systemd/journald.conf.d/sangtachi.conf
UNIT="/etc/systemd/system/$SERVICE.service"
TMPREL=""
MUTATED=0      # 설정, 유닛, current 가운데 하나라도 바꾸기 시작했다
SUCCESS=0
PREV=""        # 바꾸기 전 current 가 가리키던 릴리스. 처음 배포면 비어 있다
HAD_ENV=0      # 바꾸기 전 설정 파일이 있었다
HAD_UNIT=0     # 바꾸기 전 유닛이 있었다
KEEP_PREV=0    # 되돌리기가 .prev 를 제자리에 놓지 못했다. 손으로 되살릴 수 있게 지우지 않는다
OWN_PREV=0     # 이 배포가 .prev 를 만들었다. 남이 남긴 .prev 는 지우지 않는다
OWN_NEW=0      # 이 배포가 .new 를 쓰기 시작했다. 잠금을 얻기 전에는 남의 것일 수 있다

started() {
  # 이번에 띄운 실행(InvocationID)의 줄만 본다. 앞 실행의 server.started 를 잘못 읽지 않는다.
  local inv i
  inv="$(systemctl show -p InvocationID --value "$SERVICE")"
  [ -n "$inv" ] || return 1
  for i in $(seq 1 30); do
    # grep -q 는 찾자마자 파이프를 닫아 pipefail 아래에서 journalctl 이 SIGPIPE 로 실패할 수 있다. 끝까지 읽는다.
    if sudo journalctl "_SYSTEMD_INVOCATION_ID=$inv" --no-pager -o cat | grep '^INFO server.started ' >/dev/null \
       && [ "$(systemctl show -p InvocationID --value "$SERVICE")" = "$inv" ] \
       && systemctl is-active --quiet "$SERVICE"; then
      sudo journalctl "_SYSTEMD_INVOCATION_ID=$inv" --no-pager -o cat | grep '^INFO server.started '
      return 0
    fi
    sleep 1
  done
  sudo journalctl "_SYSTEMD_INVOCATION_ID=$inv" --no-pager -o cat | tail -20 || true
  return 1
}

rollback() {
  # 바꾸기 전 상태로 되돌린다. 하나가 실패해도 나머지를 계속한다(set +e).
  set +e
  local failed=0
  echo "배포가 끝나지 못했다. 바꾸기 전 상태로 되돌린다" >&2
  if [ "$HAD_UNIT" != 1 ]; then
    # 처음 배포였다. 유닛을 지우기 전에 멈추고 등록을 푼다. 남기면 Restart=always 가 계속 다시 띄운다.
    sudo systemctl disable --now "$SERVICE" || failed=1
  fi
  if [ "$HAD_ENV" = 1 ]; then
    sudo mv -f "$ENVF.prev" "$ENVF" || { failed=1; KEEP_PREV=1; }
  else
    sudo rm -f "$ENVF" || failed=1
  fi
  if [ "$HAD_UNIT" = 1 ]; then
    sudo mv -f "$UNIT.prev" "$UNIT" || { failed=1; KEEP_PREV=1; }
  else
    sudo rm -f "$UNIT" || failed=1
  fi
  if [ -n "$PREV" ]; then
    { sudo ln -sfn "$PREV" "$BASE/current.new" && sudo mv -T "$BASE/current.new" "$BASE/current"; } || failed=1
  else
    sudo rm -f "$BASE/current" || failed=1
  fi
  sudo systemctl daemon-reload || failed=1
  if [ "$failed" = 1 ]; then
    echo "되돌리기의 일부가 실패했다. $ENVF.prev, $UNIT.prev 와 current 를 손으로 확인하라" >&2
    return
  fi
  if [ "$HAD_ENV" = 1 ] && [ "$HAD_UNIT" = 1 ] && [ -n "$PREV" ]; then
    # 실패한 릴리스가 Restart=always 로 거듭 재시작하는 동안 systemd 의 시작 횟수 제한에 걸린다. 그대로
    # restart 하면 "Start request repeated too quickly" 로 거부된다(실측). 먼저 실패 상태를 지운다.
    sudo systemctl reset-failed "$SERVICE"
    if sudo systemctl restart "$SERVICE" && started >/dev/null; then
      echo "앞 상태로 되돌렸고 서버가 떴다" >&2
    else
      echo "앞 상태로 되돌렸으나 서버가 뜨지 않았다" >&2
    fi
  elif [ "$HAD_UNIT" != 1 ]; then
    echo "처음 배포였으므로 서비스를 멈추고 지웠다" >&2
  else
    echo "앞 상태로 되돌렸다. 앞 설정이나 앞 릴리스가 없어 서비스를 다시 띄우지 않았다" >&2
  fi
}

on_exit() {
  local code=$?
  trap - EXIT
  if [ "$MUTATED" = 1 ] && [ "$SUCCESS" != 1 ]; then
    rollback
  fi
  set +e
  rm -rf -- "$UPDIR"
  case "$TMPREL" in "$BASE"/releases/.new.*) sudo rm -rf -- "$TMPREL" ;; esac
  if [ "$OWN_NEW" = 1 ]; then
    sudo rm -f "$ENVF.new" "$UNIT.new"
  fi
  if [ "$OWN_PREV" = 1 ] && [ "$KEEP_PREV" != 1 ]; then
    sudo rm -f "$ENVF.prev" "$UNIT.prev"
  fi
  exit "$code"
}
trap on_exit EXIT
trap 'exit 130' INT TERM HUP

# 배포는 한 번에 하나다. .new, .prev, current.new 를 같이 쓰기 때문이다.
exec 9>"/run/lock/$SERVICE-deploy.lock"
if ! flock -n 9; then
  echo "다른 배포가 돌고 있다" >&2
  exit 1
fi
# 앞 배포의 되돌리기가 남긴 복구 파일이 있으면 손으로 확인하기 전에는 배포하지 않는다.
if [ -e "$ENVF.prev" ] || [ -e "$UNIT.prev" ]; then
  echo "$ENVF.prev 나 $UNIT.prev 가 남아 있다. 앞 배포의 되돌리기가 실패한 흔적이다. 확인한 뒤 지우고 다시 하라" >&2
  exit 1
fi

if ! python3 -c 'import ensurepip' 2>/dev/null; then
  sudo apt-get update -qq
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq python3-venv
fi
if ! id -u "$SERVICE" >/dev/null 2>&1; then
  sudo useradd --system --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin "$SERVICE"
fi

sudo mkdir -p "$BASE/releases"
if [ ! -d "$REL" ]; then
  TMPREL="$(sudo mktemp -d "$BASE/releases/.new.XXXXXX")"
  sudo tar -xzf "$TARBALL" -C "$TMPREL" --strip-components=1
  sudo python3 -m venv "$TMPREL/.venv"
  sudo "$TMPREL/.venv/bin/pip" install --quiet --disable-pip-version-check -r "$TMPREL/requirements.txt"
  sudo mv "$TMPREL" "$REL"
  TMPREL=""
fi
# mktemp -d 는 0700 으로 만든다. 서비스 사용자가 읽고 들어가야 한다(실측: CHDIR Permission denied).
sudo chown -R root:root "$REL"
sudo chmod -R u=rwX,go=rX "$REL"

# 새 설정과 유닛을 .new 에 다 쓴다. 아직 아무것도 바꾸지 않았다. 내용은 deploy.py 가 만들어 올렸다.
OWN_NEW=1
sudo install -m 0640 -o root -g "$SERVICE" "$UPDIR/env" "$ENVF.new"
sudo install -m 0644 -o root -g root "$UPDIR/unit" "$UNIT.new"

# 바꾸기 전 상태를 적어 둔다. 여기부터 실패하면 on_exit 이 되돌린다.
PREV="$(readlink "$BASE/current" 2>/dev/null || true)"
OWN_PREV=1
if [ -f "$ENVF" ]; then
  sudo cp -p "$ENVF" "$ENVF.prev"
  HAD_ENV=1
fi
if [ -f "$UNIT" ]; then
  sudo cp -p "$UNIT" "$UNIT.prev"
  HAD_UNIT=1
fi
MUTATED=1
sudo mv -f "$ENVF.new" "$ENVF"
sudo mv -f "$UNIT.new" "$UNIT"
sudo ln -sfn "$REL" "$BASE/current.new"
sudo mv -T "$BASE/current.new" "$BASE/current"
sudo systemctl daemon-reload
sudo systemctl enable --quiet "$SERVICE"
sudo systemctl reset-failed "$SERVICE" || true  # 앞 실패로 시작 횟수 제한에 걸려 있을 수 있다
sudo systemctl restart "$SERVICE"
started
SUCCESS=1

# journald 전체 상한(architecture.md 3.4). 인스턴스 전체 설정이라 서비스 되돌리기와 묶지 않고 서비스가 뜬 뒤에 쓴다.
if [ -f "$UPDIR/journald" ] && ! sudo cmp -s "$UPDIR/journald" "$JOURNALD_DROPIN"; then
  sudo install -d -m 0755 "$(dirname "$JOURNALD_DROPIN")"
  sudo install -m 0644 -o root -g root "$UPDIR/journald" "$JOURNALD_DROPIN"
  sudo systemctl restart systemd-journald
  echo "journald 상한을 썼다: $JOURNALD_DROPIN"
fi
"""


_UPDIR = re.compile(r"/tmp/cp-deploy\.[A-Za-z0-9]{6,}")


def upload_files(ns: argparse.Namespace, target: Target) -> dict[str, str]:
    """원격 임시 폴더에 올릴 텍스트 파일. 이름 -> 내용. 묶음(app.tar.gz)은 따로 만든다."""
    files = {"env": env_text(ns), "unit": unit_text(target)}
    if ns.target == "tm":
        files["journald"] = JOURNALD_DROPIN
    return files


def remote_deploy(ns: argparse.Namespace, sha: str) -> str:
    target = TARGETS[ns.target]
    # 원격에 이 배포만의 폴더를 만든다. 고정된 /tmp 경로는 겹친 배포가 서로의 묶음을 덮거나 지운다.
    updir = run([*ssh_base(ns), "mktemp -d /tmp/cp-deploy.XXXXXXXX"]).strip()
    if not _UPDIR.fullmatch(updir):
        raise DeployError("원격 임시 폴더 이름이 예상과 다르다")
    try:
        with tempfile.TemporaryDirectory(prefix="cp-deploy-") as tmp:
            tarball = Path(tmp) / "app.tar.gz"
            # 묶음 안의 경로는 control-server/... 나 telemetry-server/... 다. 원격이 --strip-components=1 로 앞 폴더를
            # 벗겨 릴리스 폴더 바로 아래에 둔다. 텔레메트리 릴리스에는 controlplane/ 과 telemetry/ 가 나란히 놓인다.
            run(["git", "-C", str(REPO), "archive", "--format=tar.gz", "-o", str(tarball), sha, *target.paths])
            local = [tarball]
            for name, text in upload_files(ns, target).items():
                path = Path(tmp) / name
                path.write_bytes(text.encode("utf-8"))  # 바이트로 쓴다. 텍스트 모드는 Windows 에서 CRLF 가 된다
                local.append(path)
            run(["scp", "-i", ns.key, "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", *map(str, local),
                 f"{ns.user}@{ns.host}:{updir}/"])
        args = " ".join(shlex.quote(a) for a in (sha, target.service, updir))
        return run([*ssh_base(ns), f"bash -s -- {args}"], input_text=REMOTE_SCRIPT, timeout=900)
    except BaseException:
        # 원격 스크립트가 시작하지 못하면 그 스크립트의 정리가 없다. 여기서도 지운다. 이미 지워졌으면 아무 일도 없다.
        subprocess.run([*ssh_base(ns), f"rm -rf -- {shlex.quote(updir)}"], capture_output=True, timeout=60)
        raise


def post(url: str, body: bytes, timeout: float = 5.0) -> dict:
    req = urllib.request.Request(url, data=body, method="POST", headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            payload = resp.read()
    except urllib.error.HTTPError as exc:
        payload = exc.read()
    return json.loads(payload)


def smoke(host: str, timeout: float = 5.0) -> dict:
    """없는 방의 get_peers 를 보낸다. 형식은 맞으므로 room_not_found 가 와야 한다.

    속도 제한 예산(6.4)을 하나 쓴다. 저장소를 한 번 읽고 아무것도 쓰지 않는다.
    """
    body = json.dumps({"room_id": "ZZZZZZ", "peer_id": 1, "peer_token": "0" * 32}).encode()
    data = post(f"http://{host}:{PORT}/v1/get_peers", body, timeout)
    if data.get("error") != "room_not_found":
        raise DeployError(f"연기 시험의 응답이 room_not_found 가 아니다: {data!r}")
    return data


# 6.3 서버의 검사에서 v 가 틀린 레코드. 검사에서 끝나므로 저장소와 CloudWatch 에 닿지 않는다.
TM_SMOKE_BODY = b'{"v":2}'


def smoke_tm(host: str, timeout: float = 5.0) -> dict:
    """텔레메트리에 쓰기 없는 report 를 보낸다. bad_request 가 와야 한다. 출발지 예산(3.4)을 하나 쓴다."""
    data = post(f"http://{host}:{TARGETS['tm'].port}/v1/report", TM_SMOKE_BODY, timeout)
    if data.get("error") != "bad_request":
        raise DeployError(f"연기 시험의 응답이 bad_request 가 아니다: {data!r}")
    return data


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="제어 서버나 텔레메트리 서비스를 EC2 한 대에 배포한다")
    p.add_argument("--target", choices=sorted(TARGETS), default="cp", help="cp 는 제어 서버, tm 은 텔레메트리 서비스")
    p.add_argument("--host", required=True)
    p.add_argument("--key", required=True)
    p.add_argument("--table", required=True, help="그 대상의 테이블. tm 이면 텔레메트리 테이블")
    p.add_argument("--cp-table", help="tm 이 방 대조로 읽는 제어 평면 테이블")
    p.add_argument("--region", required=True)
    p.add_argument("--user", default="ubuntu")
    ns = p.parse_args(argv)
    try:
        check_args(ns)
        sha = committed_sha(TARGETS[ns.target])
        print(f"배포 대상 {TARGETS[ns.target].service} 커밋 {sha}")
        print(remote_deploy(ns, sha).strip())
        time.sleep(1)
        print("연기 시험:", (smoke_tm if ns.target == "tm" else smoke)(ns.host))
    except DeployError as exc:
        print(f"ERROR {exc}", file=sys.stderr)
        return 1
    print("VERDICT: pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
