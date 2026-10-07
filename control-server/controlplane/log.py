"""서버 로그 한 줄. 출처는 control_plane.md 7.5 로그와 카운터다.

모양은 클라이언트와 같은 `<수준> <이벤트키> <이름>=<값> ...` 이고 표준 오류에 한 줄 한 이벤트다.

이벤트와 필드는 허용 목록이다. 7.5 표에 없는 이벤트나 필드를 넘기면 ValueError 다. room_id 와
peer_token 과 본문이 로그에 실리지 않는다는 7.5 의 규칙을, 그 이름을 막는 대신 표에 있는 이름만
받는 것으로 지킨다.
"""

from __future__ import annotations

import re
import sys
from collections.abc import Callable

LEVELS = ("INFO", "WARN", "ERROR")

# 7.5 표의 이벤트키 -> 필수 필드. 순서가 줄에 찍히는 순서다.
EVENTS: dict[str, tuple[str, ...]] = {
    "http.request": ("op", "status", "error", "src", "ms"),
    "room.created": ("peer_id", "virtual_ip"),
    "pair.ready": ("host_peer_id", "peer_id", "punch_delay_ms"),
    "peer.released": ("peer_id", "virtual_ip", "by"),
    "peer.reclaim_skipped": ("peer_id", "reason"),
    "room.renewed": ("host_peer_id", "expires_in_s"),
    "vip.claimed": ("virtual_ip", "peer_id"),
    "counter": ("name", "value"),
    "server.started": ("bind", "port"),
}

# 값 하나. 공백, '=', 제어 문자가 없는 ASCII 출력 문자. 빈 값은 '-' 로 쓴다.
_VALUE = re.compile(r"[!-<>-~]+")

Sink = Callable[[str], None]


def _default_sink(line: str) -> None:
    sys.stderr.write(line + "\n")
    sys.stderr.flush()


_sink: Sink = _default_sink


def set_sink(sink: Sink | None) -> None:
    """줄을 받을 곳을 바꾼다. None 이면 표준 오류로 돌아간다. 시험이 쓴다."""
    global _sink
    _sink = sink or _default_sink


def format_line(level: str, event: str, fields: dict[str, object]) -> str:
    if level not in LEVELS:
        raise ValueError(f"모르는 수준 {level!r}")
    required = EVENTS.get(event)
    if required is None:
        raise ValueError(f"7.5 에 없는 이벤트 {event!r}")
    if set(fields) != set(required):
        raise ValueError(f"{event} 의 필드는 {required} 다. 받은 것 {sorted(fields)}")
    parts = [level, event]
    for name in required:
        value = fields[name]
        if isinstance(value, bool) or not isinstance(value, (int, str)):
            raise ValueError(f"{event}.{name} 은 정수나 문자열이어야 한다")
        text = str(value) if value != "" else "-"
        if not _VALUE.fullmatch(text):
            raise ValueError(f"{event}.{name} 의 값에 쓸 수 없는 문자가 있다")
        parts.append(f"{name}={text}")
    return " ".join(parts)


def emit(level: str, event: str, **fields: object) -> None:
    _sink(format_line(level, event, fields))
