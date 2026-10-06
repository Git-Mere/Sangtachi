"""케이스 표의 변이 시험 실행기.

control_plane.md 9장 검증이 정한 것을 기계로 돈다. 규칙 한 줄을 지운 구현이 그 표의 어느
행에서 FAIL 하는지 본다. 한 행도 떨어뜨리지 못하는 변이가 있으면 그 규칙은 아직 아무것도
지키지 않는다.

쓰는 법 (control-server 에서):

    .venv/Scripts/python tests/mutate.py              # 전부
    .venv/Scripts/python tests/mutate.py room_id      # tests/mutants/room_id.py 만
    .venv/Scripts/python tests/mutate.py --store store # 저장소 시험. pytest 에 --store 를 넘긴다

옵션은 둘이다. 모르는 옵션은 ERROR 다.

    --store   원본 시험과 모든 변이의 pytest 에 그대로 넘긴다
    -j N      변이를 N 개씩 동시에 돌린다. 기본값은 DEFAULT_JOBS 다. -j 1 은 차례로 돈다

변이마다 pytest 는 그 변이의 kills 행만 돌린다. 판정이 kills 행만 보기 때문이다. 그래서 행이
단독으로 돌 때와 파일째 돌 때 결과가 다르면 판정이 틀어진다. 이를 막으려고 변이마다 같은 행을
원본으로도 단독으로 돌려, 원본이 그 행들을 통과하지 못하면 그 변이는 ERROR 다.

변이 목록은 tests/mutants/<표>.py 의 MUTANTS 다. 원소는 dict 이고 키는 MUTANT_KEYS 다.

    id     변이 이름. ASCII
    path   control-server 기준 원본 경로. controlplane/ 아래여야 한다
    old    원본에서 바꿀 문자열. 원본 안에 정확히 한 번 있어야 한다
    new    바꿀 문자열
    kills  FAIL 해야 하는 시험 노드 id 목록. 하나 이상
    why    지운 규칙이 무엇인가

판정은 넷이다. 기본값은 "판정 불가" 이고 확실할 때만 KILLED 다. KILLED 는 kills 의 행이
시험 본문에서 FAIL 한 것만 센다. 준비나 정리 단계의 ERROR 는 ERROR 다.

    KILLED    kills 가 전부 FAIL 했다
    PARTIAL   일부만 FAIL 했다. kills 가 틀렸거나 표가 그 행을 지키지 못한다
    SURVIVED  시험이 전부 통과했다
    ERROR     위 셋으로 읽을 수 없다. 수집 실패, 시간 초과, 원본에서 old 를 찾지 못함 등

변이를 넣기 전에 원본으로 같은 시험을 한 번 돌린다. 원본이 떨어지면 변이 판정이 뜻이 없으므로
멈춘다. kills 의 행이 원본에서 PASSED 로 나오지 않아도 멈춘다. 건너뛴 행(--store 없이 돌린 저장소
시험)이나 없는 노드 id 는 변이가 무엇이든 떨어질 수 없어 SURVIVED 로 잘못 읽히기 때문이다.
종료 코드는 전부 KILLED 일 때만 0 이다.
"""

from __future__ import annotations

import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

CP_ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "controlplane"
MUTANT_DIR = CP_ROOT / "tests" / "mutants"
MUTANT_KEYS = frozenset({"id", "path", "old", "new", "kills", "why"})
TIMEOUT_S = 120
_ID = re.compile(r"^[A-Za-z0-9_.-]+$")
_REPORT = re.compile(r"^(FAILED|ERROR) (\S+?)(?: - .*)?$")
_PASSED = re.compile(r"^PASSED (\S+)$")
PYTEST_FLAGS = frozenset({"--store"})
DEFAULT_JOBS = 8


@dataclass(frozen=True)
class Mutant:
    id: str
    path: str
    old: str
    new: str
    kills: tuple[str, ...]
    why: str


@dataclass(frozen=True)
class Outcome:
    verdict: str
    detail: str


def to_mutant(raw: object, source: str) -> Mutant:
    """목록 원소 하나를 검사해 Mutant 로 만든다. 모르는 모양이면 ValueError."""
    if not isinstance(raw, dict):
        raise ValueError(f"{source}: 원소가 dict 가 아니다")
    keys = set(raw)
    if keys != MUTANT_KEYS:
        raise ValueError(f"{source}: 키가 다르다. 없음 {sorted(MUTANT_KEYS - keys)}, 남음 {sorted(keys - MUTANT_KEYS)}")
    for k in ("id", "path", "old", "new", "why"):
        if not isinstance(raw[k], str):
            raise ValueError(f"{source}: {k} 가 문자열이 아니다")
    if not _ID.match(raw["id"]):
        raise ValueError(f"{source}: id {raw['id']!r} 는 ASCII 이름이어야 한다")
    if not raw["path"].startswith(PACKAGE + "/") or ".." in raw["path"]:
        raise ValueError(f"{source}: path {raw['path']!r} 는 {PACKAGE}/ 아래여야 한다")
    if raw["old"] == "" or raw["old"] == raw["new"]:
        raise ValueError(f"{source}: old 가 비었거나 new 와 같다")
    kills = raw["kills"]
    if not isinstance(kills, (list, tuple)) or not kills or not all(isinstance(x, str) and "::" in x for x in kills):
        raise ValueError(f"{source}: kills 는 시험 노드 id 의 비지 않은 목록이어야 한다")
    if not raw["why"].strip():
        raise ValueError(f"{source}: why 가 비었다")
    return Mutant(raw["id"], raw["path"], raw["old"], raw["new"], tuple(kills), raw["why"])


def load_mutants(names: list[str] | None = None) -> list[Mutant]:
    files = sorted(p for p in MUTANT_DIR.glob("*.py") if not p.name.startswith("_"))
    if names:
        wanted = set(names)
        missing = wanted - {p.stem for p in files}
        if missing:
            raise ValueError(f"변이 파일이 없다: {sorted(missing)}")
        files = [p for p in files if p.stem in wanted]
    if not files:
        raise ValueError(f"{MUTANT_DIR} 에 변이 파일이 없다")
    out: list[Mutant] = []
    seen: set[str] = set()
    for path in files:
        spec = importlib.util.spec_from_file_location(f"_mutants_{path.stem}", path)
        assert spec and spec.loader
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        raw = getattr(mod, "MUTANTS", None)
        if not isinstance(raw, list) or not raw:
            raise ValueError(f"{path.name}: MUTANTS 가 비지 않은 list 가 아니다")
        for i, item in enumerate(raw):
            m = to_mutant(item, f"{path.name}[{i}]")
            if m.id in seen:
                raise ValueError(f"{path.name}: id {m.id} 가 겹친다")
            seen.add(m.id)
            out.append(m)
    return out


def apply(text: str, m: Mutant) -> str:
    """old 가 정확히 한 번 있을 때만 바꾼다. 아니면 ValueError (낡은 변이)."""
    n = text.count(m.old)
    if n != 1:
        raise ValueError(f"{m.id}: {m.path} 에서 old 가 {n} 번 나온다. 한 번이어야 한다")
    return text.replace(m.old, m.new, 1)


def report_nodes(output: str) -> tuple[set[str], set[str]]:
    """pytest -rfE 요약 줄에서 (FAILED 노드, ERROR 노드) 를 모은다.

    ERROR 는 시험 본문이 아니라 준비나 정리 단계에서 터진 것이다. 변이가 그 행을 실제로 돌렸다는
    증거가 아니므로 KILLED 로 세지 않는다.
    """
    failed: set[str] = set()
    errored: set[str] = set()
    for line in output.splitlines():
        hit = _REPORT.match(line.strip())
        if hit:
            (failed if hit.group(1) == "FAILED" else errored).add(hit.group(2))
    return failed, errored


def passed_nodes(output: str) -> set[str]:
    """pytest -rA 요약 줄에서 PASSED 노드를 모은다."""
    out: set[str] = set()
    for line in output.splitlines():
        hit = _PASSED.match(line.strip())
        if hit:
            out.add(hit.group(1))
    return out


def unrun_kills(output: str, mutants: list[Mutant]) -> list[str]:
    """원본 시험에서 PASSED 로 나오지 않은 kills 행. 비어 있어야 변이 판정이 뜻이 있다."""
    passed = passed_nodes(output)
    return sorted({k for m in mutants for k in m.kills if k not in passed})


def parse_args(argv: list[str]) -> tuple[list[str], list[str], int]:
    """(변이 파일 이름, pytest 에 넘길 옵션, 동시 실행 수). 모르는 옵션은 ValueError."""
    names: list[str] = []
    flags: list[str] = []
    jobs = DEFAULT_JOBS
    args = list(argv)
    while args:
        arg = args.pop(0)
        if arg == "-j" or arg.startswith("-j"):
            value = arg[2:] if arg != "-j" else (args.pop(0) if args else "")
            if not re.fullmatch(r"[0-9]+", value) or not 1 <= int(value) <= 64:
                raise ValueError(f"-j 는 1~64 의 정수다: {value!r}")
            jobs = int(value)
        elif arg.startswith("-"):
            if arg not in PYTEST_FLAGS:
                raise ValueError(f"모르는 옵션 {arg!r}")
            if arg not in flags:
                flags.append(arg)
        else:
            names.append(arg)
    return names, flags, jobs


def judge(exit_code: int | None, output: str, kills: tuple[str, ...]) -> Outcome:
    """pytest 한 번의 결과를 판정한다. exit_code None 은 시간 초과다."""
    if exit_code is None:
        return Outcome("ERROR", f"{TIMEOUT_S}초 안에 끝나지 않았다")
    if exit_code == 0:
        return Outcome("SURVIVED", "시험이 전부 통과했다")
    if exit_code != 1:
        # 2 중단, 3 내부 오류, 4 사용법, 5 수집된 시험 없음. 어느 것도 행의 FAIL 이 아니다.
        return Outcome("ERROR", f"pytest 종료 코드 {exit_code}")
    failed, errored = report_nodes(output)
    hit_err = [k for k in kills if k in errored]
    if hit_err:
        return Outcome("ERROR", f"준비나 정리 단계에서 터진 행: {hit_err}")
    if not failed and not errored:
        return Outcome("ERROR", "종료 코드 1 인데 FAIL 줄을 읽지 못했다")
    hit = [k for k in kills if k in failed]
    if len(hit) == len(kills):
        return Outcome("KILLED", f"{len(hit)}/{len(kills)}")
    if hit:
        miss = [k for k in kills if k not in failed]
        return Outcome("PARTIAL", f"떨어지지 않은 행: {miss}")
    return Outcome("SURVIVED", f"다른 행만 떨어졌다: {sorted(failed)[:5]}")


def run_pytest(src: Path, targets: list[str], flags: list[str] | tuple[str, ...] = (),
               report: str = "-rfE") -> tuple[int | None, str]:
    env = dict(os.environ, CP_SRC=str(src), PYTHONDONTWRITEBYTECODE="1")
    cmd = [sys.executable, "-m", "pytest", "-q", report, "--no-header", "-p", "no:cacheprovider", *flags,
           *targets]
    try:
        proc = subprocess.run(cmd, cwd=CP_ROOT, env=env, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired as exc:
        return None, (exc.stdout or "") if isinstance(exc.stdout, str) else ""
    return proc.returncode, proc.stdout + proc.stderr


def copy_package(dest: Path) -> None:
    shutil.copytree(CP_ROOT / PACKAGE, dest / PACKAGE, ignore=shutil.ignore_patterns("__pycache__"))


def run_one(m: Mutant, original: Path, flags: list[str] | tuple[str, ...] = ()) -> Outcome:
    """kills 행만 원본과 변이로 한 번씩 돌린다. original 은 원본 패키지를 복사해 둔 폴더다."""
    nodes = sorted(set(m.kills))
    code, out = run_pytest(original, nodes, flags)
    if code != 0:
        return Outcome("ERROR", f"원본이 이 행들을 단독으로 통과하지 못한다 (종료 코드 {code})")
    with tempfile.TemporaryDirectory(prefix="cp-mutant-") as tmp:
        tmp_path = Path(tmp)
        copy_package(tmp_path)
        target = tmp_path / m.path
        if not target.is_file():
            return Outcome("ERROR", f"{m.path} 가 없다")
        try:
            target.write_text(apply(target.read_text(encoding="utf-8"), m), encoding="utf-8")
        except ValueError as exc:
            return Outcome("ERROR", str(exc))
        code, out = run_pytest(tmp_path, nodes, flags)
    return judge(code, out, m.kills)


def baseline(mutants: list[Mutant], flags: list[str] | tuple[str, ...] = ()) -> tuple[bool, str]:
    files = sorted({k.split("::", 1)[0] for m in mutants for k in m.kills})
    with tempfile.TemporaryDirectory(prefix="cp-baseline-") as tmp:
        copy_package(Path(tmp))
        code, out = run_pytest(Path(tmp), files, flags, report="-rA")
    return code == 0, out


def main(argv: list[str]) -> int:
    try:
        names, flags, jobs = parse_args(argv)
        mutants = load_mutants(names or None)
    except ValueError as exc:
        print(f"ERROR 변이 목록: {exc}")
        return 2
    ok, out = baseline(mutants, flags)
    if not ok:
        print(out)
        print("ERROR 원본이 시험을 통과하지 못한다. 변이 판정을 하지 않는다")
        return 2
    unrun = unrun_kills(out, mutants)
    if unrun:
        print(out)
        print(f"ERROR 원본에서 PASSED 로 나오지 않은 kills 행이 있다 (건너뜀이나 없는 노드): {unrun}")
        return 2
    with tempfile.TemporaryDirectory(prefix="cp-original-") as orig:
        copy_package(Path(orig))
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            results = list(pool.map(lambda mu: run_one(mu, Path(orig), flags), mutants))
    counts: dict[str, int] = {}
    for m, res in zip(mutants, results):
        counts[res.verdict] = counts.get(res.verdict, 0) + 1
        print(f"{res.verdict:8} {m.id}  {res.detail}")
    total = len(mutants)
    killed = counts.get("KILLED", 0)
    print(f"VERDICT: {'pass' if killed == total else 'fail'}  killed {killed}/{total}  {counts}")
    return 0 if killed == total else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
