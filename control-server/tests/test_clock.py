"""control_plane.md 7.4 시계의 케이스 표.

CLOCK_CASES 가 7.4 케이스 표의 원본이다. 문서는 이 파일을 가리킨다. 변이는 tests/mutants/clock.py 다.

deploy_only 가 참인 행은 실제 프로세스 재시작이나 인스턴스 재부팅이 있어야 끝까지 확인된다.
그 확인은 roadmap.md Phase 3 "시계와 로그" 의 배포 검증이 Linux 에서 한다. 여기서는 순수 함수가
그 상황에서 받는 입력으로 낼 값만 본다.
"""

import subprocess
import sys
from pathlib import Path

import pytest

from controlplane import clock

READY_MONO_NS = 10_000_000_000
READY_WALL_MS = 1_700_000_000_000
BOOT_A = "6f1c3c5e-0b0a-4c55-9a4e-2d1f7c7c0001"
BOOT_B = "6f1c3c5e-0b0a-4c55-9a4e-2d1f7c7c0002"

# 출처: control_plane.md 7.4 시계의 케이스 표. 문서 한 행이 여기 한 원소다.
#   situation, calc, result  문서의 세 열
#   boot        same: 준비 완료 때와 같은 boot_id. other: 다른 boot_id.
#               missing-file: boot_id 파일이 없어 current_boot_id() 가 프로세스 난수를 준다.
#               준비 완료는 다른 프로세스(재시작 전)가 그 프로세스의 난수로 기록했다
#   now_mono_ns, now_wall_ms  응답 시점의 두 시계
#   expect      (ms, source, 카운터 증가분)
#   deploy_only 참이면 그 상황 자체는 배포 검증(roadmap Phase 3 시계와 로그)이 만든다
CLOCK_CASES = [
    dict(id="same-boot", situation="같은 부팅, 프로세스 재시작 없음", calc="단조", result="정확",
         boot="same", now_mono_ns=READY_MONO_NS + 5_000_123_456, now_wall_ms=READY_WALL_MS + 65_000,
         expect=(5000, "mono", 0), deploy_only=False,
         note="벽시계가 그사이 1분 앞으로 갔어도(65초) 단조 값 5초를 낸다"),
    dict(id="same-boot-restart", situation="같은 부팅, 프로세스 재시작",
         calc="단조. `CLOCK_MONOTONIC` 은 프로세스와 무관하다", result="정확",
         boot="same", now_mono_ns=READY_MONO_NS + 5_000_123_456, now_wall_ms=READY_WALL_MS + 65_000,
         expect=(5000, "mono", 0), deploy_only=True,
         note="재시작 뒤에도 boot_id 가 같으면 단조로 계산한다. 단조 시계가 재시작에 이어지는지는 배포 검증이 본다"),
    dict(id="reboot", situation="인스턴스 재부팅 뒤", calc="벽시계 대체",
         result="NTP 스텝만큼 틀릴 수 있다. 이 경우 `protocol.md` 10.2 랑데부의 오차 상한은 보장되지 않는다. "
                "14장 계약 4 가 그 예외를 이 문서에 위임한다. 준비 완료와 폴링 응답 사이 몇 초 안에 재부팅과 "
                "시각 조정이 겹쳐야 하므로 드물다. 카운터로 관측한다",
         boot="other", now_mono_ns=3_000_000_000, now_wall_ms=READY_WALL_MS + 7_000,
         expect=(7000, "wall", 1), deploy_only=True,
         note="재부팅 뒤 단조 값은 기준이 달라 작아졌다. 벽시계 차를 쓰고 elapsed_wall_fallback 을 올린다"),
    dict(id="wall-negative", situation="벽시계 대체에서 음수", calc="`max(0, ...)`",
         result="0. 클라이언트는 `punch_delay_ms` 를 다 기다린다",
         boot="other", now_mono_ns=3_000_000_000, now_wall_ms=READY_WALL_MS - 2_500,
         expect=(0, "wall", 1), deploy_only=False, note="벽시계가 준비 완료 시각보다 뒤로 갔다"),
    dict(id="no-boot-id-file", situation="`boot_id` 파일이 없다 (Linux 가 아니다)",
         calc="프로세스 시작 시 뽑은 난수를 `boot_id` 로 쓴다",
         result="프로세스 재시작마다 벽시계 대체로 떨어진다. 로컬 시험에서 그렇다. 배포 대상은 Linux 다",
         boot="missing-file", now_mono_ns=3_000_000_000, now_wall_ms=READY_WALL_MS + 4_000,
         expect=(4000, "wall", 1), deploy_only=False,
         note="한 프로세스 안에서는 같은 값이 나와 단조로 계산한다. 다른 프로세스의 값과는 다르다"),
]


# 문서 표에 없는 행. 7.4 의사 코드의 max(0, d) 는 두 분기 모두에 걸린다.
CLOCK_EXTRA_CASES = [
    dict(id="same-boot-negative", situation="같은 부팅, 단조 차이가 음수", calc="`max(0, ...)`", result="0",
         boot="same", now_mono_ns=READY_MONO_NS - 2_000_000_000, now_wall_ms=READY_WALL_MS + 1_000,
         expect=(0, "mono", 0), deploy_only=False,
         note="저장값이 지금보다 뒤인 단조 값이어도 음수를 내지 않는다. 벽시계로 떨어지지 않고 카운터도 그대로다"),
]

def _boot_ids(case, tmp_path):
    """(준비 완료 때의 boot_id, 지금의 boot_id)."""
    if case["boot"] == "same":
        return BOOT_A, BOOT_A
    if case["boot"] == "other":
        return BOOT_A, BOOT_B
    missing = str(tmp_path / "no_such_boot_id")
    now = clock.current_boot_id(missing)
    # 같은 프로세스 안에서는 매번 같은 값이다. 그래야 재시작 없는 로컬 실행은 단조로 계산한다.
    assert clock.current_boot_id(missing) == now
    counters = {}
    assert clock.elapsed_since_ready_ms(_pair(now), now, READY_MONO_NS + 1_000_000, READY_WALL_MS, counters) == (1, "mono")
    assert counters == {}
    # 재시작 전 프로세스: 다른 파이썬 프로세스가 같은 controlplane 을 import 해 뽑은 값이다.
    return _boot_id_in_other_process(missing), now


def _boot_id_in_other_process(path):
    src = str(Path(clock.__file__).resolve().parents[1])
    code = f"import sys; sys.path.insert(0, {src!r}); from controlplane import clock; print(clock.current_boot_id({path!r}))"
    out = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, timeout=30, check=True)
    return out.stdout.strip()


def _pair(boot_id):
    return {"ready_boot_id": boot_id, "ready_at_mono_ns": READY_MONO_NS, "ready_at_wall_ms": READY_WALL_MS}


@pytest.mark.parametrize("case", CLOCK_CASES + CLOCK_EXTRA_CASES, ids=lambda c: c["id"])
def test_clock(case, tmp_path):
    ready_boot, now_boot = _boot_ids(case, tmp_path)
    counters = {clock.FALLBACK_COUNTER: 3}
    got = clock.elapsed_since_ready_ms(_pair(ready_boot), now_boot, case["now_mono_ns"], case["now_wall_ms"], counters)
    ms, source, bump = case["expect"]
    assert got == (ms, source)
    assert counters == {clock.FALLBACK_COUNTER: 3 + bump}


def test_clock_table_is_whole():
    assert len(CLOCK_CASES) == 5
    assert [c["id"] for c in CLOCK_CASES if c["deploy_only"]] == ["same-boot-restart", "reboot"]


def test_boot_id_read_from_file(tmp_path):
    """7.4 저장값 표. ready_boot_id 의 출처는 boot_id 파일 내용이다."""
    p = tmp_path / "boot_id"
    p.write_text(BOOT_A + "\n", encoding="ascii")
    assert clock.current_boot_id(str(p)) == BOOT_A


def test_counter_starts_from_absent():
    counters = {}
    clock.elapsed_since_ready_ms(_pair(BOOT_A), BOOT_B, 0, READY_WALL_MS + 1, counters)
    assert counters == {clock.FALLBACK_COUNTER: 1}
