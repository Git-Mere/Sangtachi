"""control_plane.md 7.5 로그 줄의 변이."""

P = "controlplane/log.py"
T = "tests/test_log.py::"

MUTANTS = [
    {"id": "log-fields-not-checked", "path": P,
     "old": "    if set(fields) != set(required):\n", "new": "    if False:\n",
     "kills": [T + "test_fields_are_an_allow_list[room.created-fields0]",
               T + "test_fields_are_an_allow_list[http.request-fields1]"],
     "why": "7.5 room_id 와 peer_token 을 싣지 않는다. 표에 있는 필드만 받는다"},
    {"id": "log-event-not-checked", "path": P,
     "old": "    required = EVENTS.get(event)\n    if required is None:", "new": "    required = EVENTS.get(event, tuple(fields))\n    if required is None:",
     "kills": [T + "test_fields_are_an_allow_list[room.joined-fields3]"],
     "why": "7.5 표에 없는 이벤트"},
    {"id": "log-value-not-checked", "path": P,
     "old": "        if not _VALUE.fullmatch(text):", "new": "        if False:",
     "kills": [T + "test_bad_values_are_rejected[space]", T + "test_bad_values_are_rejected[equals]",
               T + "test_bad_values_are_rejected[newline]"],
     "why": "값에 공백, '=', 줄바꿈이 들어가면 줄을 다시 읽을 수 없다"},
    {"id": "log-bool-as-int", "path": P,
     "old": "isinstance(value, bool) or ", "new": "",
     "kills": [T + "test_bad_values_are_rejected[bool]"],
     "why": "bool 은 정수가 아니다"},
    {"id": "log-empty-not-dash", "path": P,
     "old": 'text = str(value) if value != "" else "-"', "new": "text = str(value)",
     "kills": [T + "test_empty_value_is_dash"],
     "why": "7.5 성공이면 error 는 '-'"},
]
