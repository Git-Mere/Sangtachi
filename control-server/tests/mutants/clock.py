"""7.4 시계 케이스 표의 변이. 실행기는 tests/mutate.py 다."""

P = "controlplane/clock.py"
T = "tests/test_clock.py::test_clock"


def k(*ids):
    return [f"{T}[{i}]" for i in ids]


MUTANTS = [
    dict(id="boot-compare-inverted", path=P, old='if pair["ready_boot_id"] == boot_id:',
         new='if pair["ready_boot_id"] != boot_id:',
         kills=k("same-boot", "same-boot-restart", "reboot", "wall-negative", "no-boot-id-file"),
         why="같은 부팅 판정을 뒤집는다"),
    dict(id="always-wall", path=P, old='if pair["ready_boot_id"] == boot_id:', new="if False:",
         kills=k("same-boot", "same-boot-restart", "no-boot-id-file"),
         why="단조 계산을 지우고 늘 벽시계를 쓴다. 벽시계가 1분 앞으로 간 행에서 값이 뛴다"),
    dict(id="always-mono", path=P, old='if pair["ready_boot_id"] == boot_id:', new="if True:",
         kills=k("reboot", "wall-negative", "no-boot-id-file"),
         why="벽시계 대체를 지운다. 재부팅 뒤 기준이 다른 단조 값을 뺀다"),
    dict(id="mono-unit", path=P, old='"ready_at_mono_ns"]) // 1_000_000', new='"ready_at_mono_ns"]) // 1_000',
         kills=k("same-boot", "same-boot-restart"), why="ns 를 ms 로 바꾸는 단위를 틀린다"),
    dict(id="no-clamp", path=P, old="return max(0, d), source", new="return d, source",
         kills=k("wall-negative", "same-boot-negative"), why="max(0, ...) 를 지운다"),
    dict(id="no-counter", path=P,
         old="counters[FALLBACK_COUNTER] = counters.get(FALLBACK_COUNTER, 0) + 1",
         new="pass", kills=k("reboot", "wall-negative", "no-boot-id-file"),
         why="elapsed_wall_fallback 카운터를 올리지 않는다"),
    dict(id="counter-on-mono", path=P, old='        source = "mono"\n',
         new='        source = "mono"\n        counters[FALLBACK_COUNTER] = counters.get(FALLBACK_COUNTER, 0) + 1\n',
         kills=k("same-boot", "same-boot-restart"), why="단조 경로에서도 대체 카운터를 올린다"),
    dict(id="boot-id-redrawn", path=P, old="return text if text else _PROCESS_BOOT_ID",
         new='return text if text else "process-" + secrets.token_hex(16)',
         kills=k("no-boot-id-file"),
         why="boot_id 대체 난수를 호출마다 새로 뽑는다. 한 프로세스 안에서도 늘 벽시계로 떨어진다"),
    dict(id="boot-id-constant", path=P, old='_PROCESS_BOOT_ID = "process-" + secrets.token_hex(16)',
         new='_PROCESS_BOOT_ID = "process-fixed"',
         kills=k("no-boot-id-file"),
         why="대체 boot_id 를 난수가 아닌 고정값으로 둔다. 프로세스 재시작 뒤에도 같은 부팅으로 읽혀 기준이 다른 단조 값을 뺀다"),
    dict(id="boot-id-ignores-file", path=P, old="return text if text else _PROCESS_BOOT_ID",
         new="return _PROCESS_BOOT_ID", kills=["tests/test_clock.py::test_boot_id_read_from_file"],
         why="boot_id 파일을 읽지 않는다"),
    dict(id="counter-requires-key", path=P,
         old="counters[FALLBACK_COUNTER] = counters.get(FALLBACK_COUNTER, 0) + 1",
         new="counters[FALLBACK_COUNTER] += 1",
         kills=["tests/test_clock.py::test_counter_starts_from_absent"],
         why="카운터 키가 미리 없으면 KeyError 로 응답이 500 이 된다"),
    dict(id="clamp-wall-only", path=P, old="return max(0, d), source",
         new='return (max(0, d) if source == "wall" else d), source',
         kills=k("same-boot-negative"), why="max(0, ...) 를 벽시계 분기에만 건다"),
]
