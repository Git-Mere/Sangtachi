"""Phase 9 실측 묶음의 그래프를 그린다.

분당 RTT 는 CloudWatch 1분 해상도 지표(`Rtt`), 플레이어 수립 시간과 성공률은 tm-analysis 가 읽는 레코드에서 낸다.
쓰는 법과 입력은 README.md 다.
"""
from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tm-analysis"))
import tmanalysis as tm  # noqa: E402


def cw_points(paths: list[str], metric: str) -> list[tuple[int, dict]]:
    out = []
    for p in paths:
        for d in json.loads(Path(p).read_text(encoding="utf-8")).get(metric, []):
            ms = int(datetime.fromisoformat(d["Timestamp"].replace("Z", "+00:00")).timestamp() * 1000)
            out.append((ms, d["ExtendedStatistics"]))
    return sorted(out, key=lambda x: x[0])


def rtt_per_minute(batches, cw, labels, out: Path) -> None:
    """묶음 안 경과 시간(분)을 가로축으로 둔다. 시각을 내지 않는다(ADR 0001).

    데이터 점의 시각은 그 1분 구간의 시작이다. 묶음과 겹치는 구간을 모두 넣는다. 한 구간이 두 묶음에
    걸치면 표본을 가를 수 없으므로 멈춘다.
    """
    pts = cw_points(cw, "Rtt")

    def overlaps(ms, b):
        return b.start_ms - 60000 < ms < b.end_ms

    for ms, _ in pts:
        hit = [b.env for b in batches if overlaps(ms, b)]
        if len(hit) > 1:
            raise SystemExit(f"ERROR 1분 구간 하나가 두 묶음에 걸친다: {hit}")
    fig, ax = plt.subplots(figsize=(8, 4.5))
    for env, label in labels.items():
        xs, mid, lo, hi, offset = [], [], [], [], 0.0
        for b in sorted((b for b in batches if b.env == env), key=lambda b: b.start_ms):
            for ms, e in pts:
                if overlaps(ms, b):
                    xs.append(offset + (max(ms, b.start_ms) - b.start_ms) / 60000)
                    mid.append(e["p50"]); lo.append(e["p25"]); hi.append(e["p75"])
            offset += (b.end_ms - b.start_ms) / 60000
        if not xs:
            print(f"WARN {env} 에 Rtt 데이터 점이 없다", file=sys.stderr)
            continue
        (line,) = ax.plot(xs, mid, marker="o", ms=3, label=label)
        ax.fill_between(xs, lo, hi, alpha=0.15, color=line.get_color())
    if ax.lines:
        ax.set_yscale("log")
    ax.set_xlabel("Elapsed time in batch (min)")
    ax.set_ylabel("Tunnel RTT per minute (ms)\nmedian, band = 25th-75th percentile")
    ax.grid(alpha=0.3)
    if ax.lines:
        ax.legend()
    fig.tight_layout()
    fig.savefig(out, dpi=110)


def setup_time(summaries, labels, out: Path) -> None:
    fig, ax = plt.subplots(figsize=(8, 4))
    data = [[v / 1000 for v in summaries[env].player_setup] for env in labels]
    ax.boxplot(data, whis=(0, 100))
    for i, vals in enumerate(data, start=1):
        ax.scatter([i] * len(vals), vals, s=12, alpha=0.6, zorder=3)
    ax.set_xticks(range(1, len(labels) + 1), [f"{l}\n(n={len(d)})" for l, d in zip(labels.values(), data)])
    ax.set_ylabel("Player setup time (s)")
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(out, dpi=110)


def success_rate(summaries, labels, out: Path) -> None:
    fig, ax = plt.subplots(figsize=(8, 4))
    for i, (env, label) in enumerate(labels.items()):
        s = summaries[env]
        if s.attempts == 0:
            ax.annotate("0/0", (i, 0), textcoords="offset points", xytext=(8, 4))
            continue
        rate = s.successes / s.attempts
        lo, hi = tm.wilson(s.successes, s.attempts)
        ax.errorbar(i, rate * 100, yerr=[[max(rate - lo, 0) * 100], [max(hi - rate, 0) * 100]], fmt="o", capsize=5)
        ax.annotate(f"{s.successes}/{s.attempts}", (i, rate * 100), textcoords="offset points", xytext=(8, -4))
    ax.set_xticks(range(len(labels)), list(labels.values()))
    ax.set_ylabel("Player success rate (%)\nerror bar = Wilson 95%")
    ax.set_ylim(0, 105)
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(out, dpi=110)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--batches", default=str(tm.DEFAULT_BATCHES))
    ap.add_argument("--records", required=True, help="tm-analysis --save 로 저장한 레코드 JSON")
    ap.add_argument("--cw", nargs="+", required=True, help="CloudWatch 1분 해상도 값 JSON")
    ap.add_argument("--label", action="append", required=True, metavar="ENV=LABEL", help="그릴 실제 환경과 그림의 이름. 순서대로 그린다")
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    batches = tm.parse_batches(Path(a.batches).read_text(encoding="utf-8"))
    labels = dict(x.split("=", 1) for x in a.label)
    real = {b.env for b in batches if b.kind == "real"}
    if not set(labels) <= real:
        print(f"ERROR 실제 환경 묶음이 아니다: {sorted(set(labels) - real)}", file=sys.stderr)
        return 2
    got = tm.assign(json.loads(Path(a.records).read_text(encoding="utf-8")), batches)
    summaries = {env: tm.summarize(got.attempts.get(env, [])) for env in labels}
    summaries_all = dict(summaries)
    summaries_all["__all__"] = tm.summarize([x for env in labels for x in got.attempts.get(env, [])])
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    rtt_per_minute(batches, a.cw, labels, out / "rtt-per-minute.png")
    setup_time(summaries, labels, out / "player-setup-time.png")
    success_rate(summaries_all, {**labels, "__all__": "Combined"}, out / "success-rate.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
