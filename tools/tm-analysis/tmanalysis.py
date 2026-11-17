"""텔레메트리 테이블에서 실측 묶음의 레코드를 읽어 experiments.md 에 실을 표를 낸다.

    python tools/tm-analysis/tmanalysis.py --table <테이블> --region <리전> [--save FILE]
    python tools/tm-analysis/tmanalysis.py --load FILE

출처는 docs/kor/roadmap.md Phase 9 의 분석 스크립트 항목과 통계 설계 표, architecture.md 3.4 저장과 CloudWatch 지표,
6.3 레코드 필드다. 묶음 파일 형식은 README.md 가 갖는다. 표는 Markdown 으로 표준 출력에 낸다. 시각은 날짜까지만 낸다.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import time
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from datetime import datetime, timezone
from decimal import Decimal
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_BATCHES = HERE / "local" / "batches.txt"

# Wilson 95% (roadmap.md Phase 9 분석 항목).
Z95 = 1.959963984540054
KINDS = ("real", "controlled")
BY_FIELDS = ("stun_same_endpoint", "local_differs_public", "same_lan", "build_kind")
# 플레이어 값과 같은 방 호스트 값의 짝으로 가르는 필드. 홀펀칭은 양쪽 NAT 에 달렸다.
PAIRED_FIELDS = ("stun_same_endpoint", "local_differs_public")
NO_PAIR = "짝 없음"
CONFLICT = "불일치"
# 레코드 한 건은 1KB 안쪽이다(architecture.md 3.4 입력 상한). 최종 일관 읽기는 4KB 에 0.5 RCU 라 한 페이지를
# 이 수로 자르면 페이지 하나가 초당 상한 하나를 넘지 않는다.
ITEMS_PER_RCU = 8


class AnalysisError(Exception):
    pass


# ---- 묶음 파일 ----

@dataclass
class Batch:
    start_ms: int
    end_ms: int
    env: str
    kind: str

    @property
    def date(self) -> str:
        return datetime.fromtimestamp(self.start_ms / 1000, timezone.utc).strftime("%Y-%m-%d")


def parse_utc(text: str) -> int:
    if not text.endswith("Z"):
        raise AnalysisError(f"UTC 시각은 Z 로 끝나야 한다: {text}")
    try:
        dt = datetime.strptime(text, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
    except ValueError as exc:
        raise AnalysisError(f"시각 형식: {text}") from exc
    return int(dt.timestamp()) * 1000


def parse_batches(text: str) -> list[Batch]:
    batches = []
    for no, line in enumerate(text.splitlines(), 1):
        parts = line.split("#", 1)[0].split()
        if not parts:
            continue
        if len(parts) != 4:
            raise AnalysisError(f"{no}행: 필드는 넷이다 (시작 끝 환경 종류)")
        start, end, env, kind = parts
        if kind not in KINDS:
            raise AnalysisError(f"{no}행: 종류는 {'|'.join(KINDS)}")
        b = Batch(parse_utc(start), parse_utc(end), env, kind)
        if b.end_ms <= b.start_ms:
            raise AnalysisError(f"{no}행: 끝이 시작보다 뒤여야 한다")
        batches.append(b)
    if not batches:
        raise AnalysisError("묶음이 없다")
    ordered = sorted(batches, key=lambda b: b.start_ms)
    for a, b in zip(ordered, ordered[1:]):
        if b.start_ms < a.end_ms:
            raise AnalysisError(f"묶음이 겹친다: {a.env}, {b.env}")
    kinds: dict[str, str] = {}
    for b in batches:
        if kinds.setdefault(b.env, b.kind) != b.kind:
            raise AnalysisError(f"환경 {b.env} 의 종류가 묶음마다 다르다")
    return batches


# ---- 묶음 배정 ----

@dataclass
class Assigned:
    attempts: dict[str, list[list[dict]]] = field(default_factory=lambda: defaultdict(list))
    unassigned: int = 0                                   # 어느 묶음에도 들지 않은 시도
    straddling: dict[str, int] = field(default_factory=dict)  # 레코드 일부가 배정 묶음 밖인 시도


def _batch_of(batches: list[Batch], t: int) -> Batch | None:
    return next((b for b in batches if b.start_ms <= t < b.end_ms), None)


def assign(records: list[dict], batches: list[Batch]) -> Assigned:
    """시도(pk)를 받은 seq 가 가장 작은 레코드의 received_at_ms 로 묶음에 배정한다."""
    by_pk: dict[str, list[dict]] = defaultdict(list)
    for r in records:
        by_pk[r["pk"]].append(r)
    out = Assigned()
    for recs in by_pk.values():
        recs.sort(key=lambda r: r["seq"])
        b = _batch_of(batches, recs[0]["received_at_ms"])
        if b is None:
            out.unassigned += 1
            continue
        out.attempts[b.env].append(recs)
        if any(not b.start_ms <= r["received_at_ms"] < b.end_ms for r in recs):
            out.straddling[b.env] = out.straddling.get(b.env, 0) + 1
    return out


# ---- 통계 ----

def wilson(k: int, n: int, z: float = Z95) -> tuple[float, float] | None:
    if n == 0:
        return None
    p = k / n
    den = 1 + z * z / n
    mid = (p + z * z / (2 * n)) / den
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / den
    return max(0.0, mid - half), min(1.0, mid + half)


def _quantile(s: list[float], q: float) -> float:
    h = (len(s) - 1) * q
    lo = math.floor(h)
    return s[lo] + (h - lo) * (s[min(lo + 1, len(s) - 1)] - s[lo])


def quartiles(values: list[float]) -> tuple[float, float, float] | None:
    """정렬 후 선형 보간(experiments.md 1장 표기)."""
    if not values:
        return None
    s = sorted(values)
    return _quantile(s, 0.25), _quantile(s, 0.5), _quantile(s, 0.75)


def missing_seq(recs: list[dict]) -> int:
    """빈 seq 수. 뒤에 온 레코드가 있는 누락만 보인다(architecture.md 6.3)."""
    seqs = {r["seq"] for r in recs}
    return max(seqs) + 1 - len(seqs)


def classify(r: dict) -> str:
    """플레이어 establish 하나. 분모와 room_known 거르기는 roadmap.md Phase 9 분석 항목이 정한다.
    ABORTED 는 CloudWatch 의 Attempts 처럼 뺀다. OK 는 room_known 이 true 일 때만 성공이고 아니면 분모에서도 뺀다.
    수립 전 실패는 room_known 과 무관하게 다 센다."""
    if r["result"] == "ABORTED":
        return "aborted"
    if r["result"] == "OK":
        return "success" if r.get("room_known") is True else "excluded_ok"
    return "failure"


@dataclass
class Summary:
    attempts: int = 0
    successes: int = 0
    aborted: int = 0
    excluded_ok: int = 0
    failures: Counter = field(default_factory=Counter)
    by: dict[str, dict] = field(default_factory=dict)  # 필드 -> 값 -> (시도, 성공)
    paired: dict[str, dict] = field(default_factory=dict)  # 필드 -> (플레이어 값, 호스트 값) -> (시도, 성공)
    player_setup: list[int] = field(default_factory=list)
    host_setup: list[int] = field(default_factory=list)
    rtt_ms: list[float] = field(default_factory=list)
    jitter_ms: list[float] = field(default_factory=list)
    lost: int = 0
    expected: int = 0
    duration_s: list[float] = field(default_factory=list)
    end_results: Counter = field(default_factory=Counter)
    missing: int = 0


def host_values(attempts: list[list[dict]]) -> dict[str, dict[str, set]]:
    """방마다 호스트 establish 의 PAIRED_FIELDS 값. 호스트의 STUN 은 방을 만들 때 한 번이라 한 방의 값은 하나다
    (architecture.md 6.3). 그래서 상대 peer_id 를 맞추지 않고 방으로 짝짓는다."""
    rooms: dict[str, dict[str, set]] = defaultdict(lambda: defaultdict(set))
    for recs in attempts:
        for r in recs:
            if r["kind"] == "establish" and r["role"] == "host" and r.get("room_id") is not None:
                for f in PAIRED_FIELDS:
                    rooms[r["room_id"]][f].add(r.get(f))
    return rooms


def host_value(rooms: dict[str, dict[str, set]], room_id: str | None, f: str):
    vals = rooms[room_id][f] if room_id in rooms else set()
    if not vals:
        return NO_PAIR
    return CONFLICT if len(vals) > 1 else next(iter(vals))


def summarize(attempts: list[list[dict]]) -> Summary:
    s = Summary()
    by: dict[str, dict] = {f: defaultdict(lambda: [0, 0]) for f in BY_FIELDS}
    paired: dict[str, dict] = {f: defaultdict(lambda: [0, 0]) for f in PAIRED_FIELDS}
    rooms = host_values(attempts)
    for recs in attempts:
        s.missing += missing_seq(recs)
        for r in recs:
            known = r.get("room_known") is True
            if r["kind"] == "establish" and r["role"] == "player":
                c = classify(r)
                if c == "aborted":
                    s.aborted += 1
                    continue
                if c == "excluded_ok":
                    s.excluded_ok += 1
                    continue
                s.attempts += 1
                ok = c == "success"
                if ok:
                    s.successes += 1
                    # 서버는 OK 의 setup_ms 가 null 인지 검사하지 않는다. CloudWatch SetupTime 처럼 값만 뺀다(architecture.md 3.4).
                    if r.get("setup_ms") is not None:
                        s.player_setup.append(r["setup_ms"])
                else:
                    s.failures[r["result"]] += 1
                for f in BY_FIELDS:
                    cell = by[f][r.get(f)]
                    cell[0] += 1
                    cell[1] += ok
                for f in PAIRED_FIELDS:
                    cell = paired[f][(r.get(f), host_value(rooms, r.get("room_id"), f))]
                    cell[0] += 1
                    cell[1] += ok
            elif r["kind"] == "establish":
                if r["result"] == "OK" and known and r.get("setup_ms") is not None:
                    s.host_setup.append(r["setup_ms"])
            elif known:  # interval, end. 세션 값은 room_known 이 true 인 것만(architecture.md 3.4 CloudWatch 지표)
                s.rtt_ms += [v / 1000 for v in r["rtt_us"]]
                if r.get("jitter_us") is not None:
                    s.jitter_ms.append(r["jitter_us"] / 1000)
                if r["kind"] == "end":
                    s.lost += r["total_expected"] - r["total_accepted"]
                    s.expected += r["total_expected"]
                    if r["role"] == "player":
                        s.duration_s.append(r["duration_ms"] / 1000)
                        s.end_results[r["result"]] += 1
    s.by = {f: {k: tuple(v) for k, v in by[f].items()} for f in BY_FIELDS}
    s.paired = {f: {k: tuple(v) for k, v in paired[f].items()} for f in PAIRED_FIELDS}
    return s


# ---- 출력 ----

def _num(x: float) -> str:
    return f"{x:.1f}"


def _dist(values: list[float]) -> str:
    q = quartiles(values)
    if q is None:
        return "n=0"
    return f"n={len(values)}, 중앙값 {_num(q[1])}, 사분위 범위 {_num(q[0])}~{_num(q[2])}"


def _rate(k: int, n: int) -> str:
    w = wilson(k, n)
    if w is None:
        return "0/0"
    return f"{k}/{n} ({100 * k / n:.1f}%), Wilson 95% {100 * w[0]:.1f}~{100 * w[1]:.1f}%"


def _value(v) -> str:
    return "null" if v is None else str(v).lower() if isinstance(v, bool) else str(v)


def render(title: str, dates: list[str], s: Summary, straddling: int) -> str:
    lines = [f"### {title}", ""]
    if dates:
        lines += [f"날짜: {', '.join(dates)}", ""]
    lines += [
        "| 항목 | 값 |", "|------|-----|",
        f"| 성공/시도 (플레이어) | {_rate(s.successes, s.attempts)} |",
        f"| 분모에서 뺀 것 | `ABORTED` {s.aborted}, `room_known` 이 true 가 아닌 `OK` {s.excluded_ok} |",
        f"| 플레이어 수립 시간 (ms) | {_dist(s.player_setup)} |",
        f"| 호스트 수립 시간 (ms) | {_dist(s.host_setup)} |",
        f"| RTT (ms) | {_dist(s.rtt_ms)} |",
        f"| 지터 (ms) | {_dist(s.jitter_ms)} |",
        f"| 손실 | {s.lost}/{s.expected}" + (f" ({100 * s.lost / s.expected:.2f}%)" if s.expected else "") + " |",
        f"| 세션 지속 시간 (s, 플레이어) | {_dist(s.duration_s)} |",
        f"| 종료 결과 (플레이어) | {', '.join(f'`{k}` {v}' for k, v in sorted(s.end_results.items())) or '-'} |",
        f"| 빈 seq 로 보인 누락 | {s.missing} |",
        "",
        "| 실패 코드 | 개수 |", "|------|-----|",
    ]
    lines += [f"| `{k}` | {v} |" for k, v in sorted(s.failures.items())] or ["| - | 0 |"]
    for f in BY_FIELDS:
        lines += ["", f"| `{f}` | 성공/시도 |", "|------|-----|"]
        for v, (n, k) in sorted(s.by[f].items(), key=lambda kv: _value(kv[0])):
            lines.append(f"| {_value(v)} | {_rate(k, n)} |")
    for f in PAIRED_FIELDS:
        lines += ["", f"| 플레이어 `{f}` | 같은 방 호스트 `{f}` | 성공/시도 |", "|------|------|-----|"]
        for (pv, hv), (n, k) in sorted(s.paired[f].items(), key=lambda kv: (_value(kv[0][0]), _value(kv[0][1]))):
            lines.append(f"| {_value(pv)} | {_value(hv)} | {_rate(k, n)} |")
    if straddling:
        lines += ["", f"경고: {title} 의 시도 {straddling}개는 레코드 일부가 묶음 시간 범위 밖에 왔다. 묶음 끝을 세션 종료 뒤로 적는다."]
    return "\n".join(lines) + "\n"


def report(records: list[dict], batches: list[Batch]) -> str:
    got = assign(records, batches)
    dates: dict[str, list[str]] = defaultdict(list)
    kind_of = {}
    for b in sorted(batches, key=lambda b: b.start_ms):
        if b.date not in dates[b.env]:
            dates[b.env].append(b.date)
        kind_of[b.env] = b.kind
    out = [f"레코드 {len(records)}개. 어느 묶음에도 들지 않은 시도 {got.unassigned}개.\n"]
    for kind, head in (("real", "## 실제 환경"), ("controlled", "## 통제된 실패 (실패 처리 경로의 증거. 실패율이 아니다)")):
        envs = [e for e in dates if kind_of[e] == kind]
        if not envs:
            continue
        out.append(head + "\n")
        for env in envs:
            out.append(render(env, dates[env], summarize(got.attempts.get(env, [])), got.straddling.get(env, 0)))
        if kind == "real" and len(envs) > 1:
            pooled = [a for e in envs for a in got.attempts.get(e, [])]
            out.append(render("실제 환경 합산", [], summarize(pooled), 0))
    return "\n".join(out)


# ---- DynamoDB ----

def plain(v):
    """boto3 resource 가 돌려주는 Decimal 을 int 나 float 로."""
    if isinstance(v, Decimal):
        return int(v) if v == v.to_integral_value() else float(v)
    if isinstance(v, list):
        return [plain(x) for x in v]
    if isinstance(v, dict):
        return {k: plain(x) for k, x in v.items()}
    return v


def scan(table, start_ms: int, end_ms: int, max_rcu: float) -> list[dict]:
    """received_at_ms 가 [start_ms, end_ms) 인 레코드. 키가 pk/seq 라 Scan 과 필터다. 필터는 읽은 뒤에 걸리므로 읽기 용량은
    테이블 전체만큼 든다. 페이지마다 소비 용량을 보고 초당 max_rcu 를 넘지 않게 쉰다."""
    from boto3.dynamodb.conditions import Attr

    kwargs = dict(FilterExpression=Attr("received_at_ms").gte(start_ms) & Attr("received_at_ms").lt(end_ms),
                  Limit=max(1, int(max_rcu * ITEMS_PER_RCU)), ReturnConsumedCapacity="TOTAL")
    items: list[dict] = []
    while True:
        t0 = time.monotonic()
        page = table.scan(**kwargs)
        items += [plain(i) for i in page.get("Items", [])]
        used = page["ConsumedCapacity"]["CapacityUnits"]
        if "LastEvaluatedKey" not in page:
            return items
        kwargs["ExclusiveStartKey"] = page["LastEvaluatedKey"]
        time.sleep(max(0.0, used / max_rcu - (time.monotonic() - t0)))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--batches", default=str(DEFAULT_BATCHES))
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--table")
    src.add_argument("--load", help="--save 로 저장한 레코드 JSON")
    ap.add_argument("--region")
    ap.add_argument("--max-rcu", type=float, default=5.0)
    ap.add_argument("--save", help="읽은 레코드를 JSON 으로 저장")
    a = ap.parse_args(argv)
    try:
        batches = parse_batches(Path(a.batches).read_text(encoding="utf-8"))
        if a.load:
            records = json.loads(Path(a.load).read_text(encoding="utf-8"))
        else:
            if not a.region:
                raise AnalysisError("--table 에는 --region 이 필요하다")
            if not (math.isfinite(a.max_rcu) and a.max_rcu > 0):
                raise AnalysisError("--max-rcu 는 0 보다 큰 유한한 수다")
            import boto3

            table = boto3.resource("dynamodb", region_name=a.region).Table(a.table)
            records = scan(table, min(b.start_ms for b in batches), max(b.end_ms for b in batches), a.max_rcu)
        if a.save:
            Path(a.save).parent.mkdir(parents=True, exist_ok=True)
            Path(a.save).write_text(json.dumps(records), encoding="utf-8")
    except (AnalysisError, OSError, json.JSONDecodeError) as exc:
        print(f"ERROR {exc}", file=sys.stderr)
        return 2
    sys.stdout.write(report(records, batches))
    return 0


if __name__ == "__main__":
    sys.exit(main())
