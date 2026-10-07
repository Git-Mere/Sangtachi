"""control_plane.md 7.5 로그 줄의 모양과 허용 목록."""

import pytest

from controlplane import log


def test_line_shape():
    line = log.format_line("INFO", "pair.ready", {"punch_delay_ms": 1000, "peer_id": 7, "host_peer_id": 3})
    assert line == "INFO pair.ready host_peer_id=3 peer_id=7 punch_delay_ms=1000"


def test_empty_value_is_dash():
    assert log.format_line("INFO", "http.request", {"op": "get_peers", "status": 200, "error": "", "src": "1.2.3.4",
                                                     "ms": 3}).endswith("error=- src=1.2.3.4 ms=3")


@pytest.mark.parametrize("event,fields", [
    ("room.created", {"peer_id": 1, "virtual_ip": "10.100.0.1", "room_id": "ABCDEF"}),   # 7.5: room_id 를 싣지 않는다
    ("http.request", {"op": "x", "status": 200, "error": "-", "src": "a", "ms": 1, "peer_token": "t"}),
    ("room.created", {"peer_id": 1}),                                                      # 필수 필드가 빠졌다
    ("room.joined", {"peer_id": 1}),                                                       # 7.5 에 없는 이벤트
])
def test_fields_are_an_allow_list(event, fields):
    with pytest.raises(ValueError):
        log.format_line("INFO", event, fields)


@pytest.mark.parametrize("value", ["a b", "a=b", "a\nb", "가", True, 1.5, None],
                         ids=["space", "equals", "newline", "non-ascii", "bool", "float", "none"])
def test_bad_values_are_rejected(value):
    # 값에 공백이나 '=' 가 있으면 줄을 다시 읽을 수 없다. 줄바꿈은 가짜 줄을 만든다.
    with pytest.raises(ValueError):
        log.format_line("WARN", "peer.reclaim_skipped", {"peer_id": 1, "reason": value})


def test_unknown_level():
    with pytest.raises(ValueError):
        log.format_line("DEBUG", "counter", {"name": "x", "value": 1})


def test_sink():
    got = []
    log.set_sink(got.append)
    try:
        log.emit("INFO", "counter", name="unavailable", value=0)
    finally:
        log.set_sink(None)
    assert got == ["INFO counter name=unavailable value=0"]
