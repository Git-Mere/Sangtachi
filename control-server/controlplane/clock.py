"""7.4 시계. 출처는 control_plane.md 7.4 시계다. 케이스 표는 tests/test_clock.py 에 있다.

elapsed_since_ready_ms 는 순수 함수다. 현재 시각 세 값과 카운터를 부르는 쪽이 넘긴다.
"""

from __future__ import annotations

import secrets
import time
from collections.abc import Mapping, MutableMapping

BOOT_ID_PATH = "/proc/sys/kernel/random/boot_id"

# boot_id 파일이 없을 때(Linux 가 아닐 때) 쓰는 값. 프로세스마다 한 번 뽑는다 (7.4 케이스 표 마지막 행).
_PROCESS_BOOT_ID = "process-" + secrets.token_hex(16)

FALLBACK_COUNTER = "elapsed_wall_fallback"


def current_boot_id(path: str = BOOT_ID_PATH) -> str:
    """boot_id 파일 내용. 읽지 못하거나 비었으면 프로세스 시작 시 뽑은 난수다."""
    try:
        with open(path, encoding="ascii") as f:
            text = f.read().strip()
    except (OSError, UnicodeDecodeError):
        text = ""
    return text if text else _PROCESS_BOOT_ID


def now_mono_ns() -> int:
    return time.monotonic_ns()


def now_wall_ms() -> int:
    return time.time_ns() // 1_000_000


def elapsed_since_ready_ms(
    pair: Mapping[str, object],
    boot_id: str,
    mono_ns: int,
    wall_ms: int,
    counters: MutableMapping[str, int],
) -> tuple[int, str]:
    """7.4 의사 코드. (경과 ms, "mono" 또는 "wall") 을 돌려준다.

    pair 는 PAIR# 항목의 ready_boot_id, ready_at_mono_ns, ready_at_wall_ms 를 갖는다.
    벽시계 대체이면 counters["elapsed_wall_fallback"] 을 1 올린다.
    """
    if pair["ready_boot_id"] == boot_id:
        d = (mono_ns - pair["ready_at_mono_ns"]) // 1_000_000
        source = "mono"
    else:
        d = wall_ms - pair["ready_at_wall_ms"]
        source = "wall"
        counters[FALLBACK_COUNTER] = counters.get(FALLBACK_COUNTER, 0) + 1
    return max(0, d), source
