# tm-plot

[`docs/kor/experiments.md`](../../docs/kor/experiments.md) 6장의 그래프를 그린다. 출처는 [`docs/kor/roadmap.md`](../../docs/kor/roadmap.md) Phase 9 의 실험 결과 시각화 항목이다.

| 그림 | 자료 |
|------|------|
| `rtt-per-minute.png` | CloudWatch 1분 해상도 `Rtt` 지표의 분마다 중앙값과 사분위수 |
| `player-setup-time.png` | 레코드에서 [`tools/tm-analysis`](../tm-analysis/README.md) 가 고른 플레이어 수립 시간 |
| `success-rate.png` | 같은 도구가 센 플레이어 성공/시도와 Wilson 95% 구간 |

가로축은 시각이 아니라 묶음 안 경과 시간이다([ADR 0001](../../docs/kor/decisions/0001-직접-연결-대비책-미도입.md) 따라야 할 조치 2번). 한 환경의 묶음이 여럿이면 이어 붙인다.

## 실행

Git Bash 에서 돈다.

```bash
uv run --no-project --with matplotlib python tools/tm-plot/plot.py \
  --records tools/tm-analysis/local/records.json \
  --cw tools/tm-analysis/local/cw-2026-11-19-1min.json tools/tm-analysis/local/cw-2026-11-23-1min.json \
  --label "same-lan-1119=Same LAN" --label "home-home-1123=Home vs home" \
  --label "campus-home-1119=Campus vs home" --label "home-hotspot-1119b=Home vs hotspot" \
  --out docs/assets/phase9
```

이것이 `experiments.md` 6장의 그림을 낸 명령이다. 환경 이름은 로컬 묶음 파일의 것이다.

- `matplotlib` 은 `uv run --with` 의 임시 환경에서만 쓴다. 제어 서버의 의존성 목록에 넣지 않는다
- `--records` 는 `tm-analysis` 의 `--save` 가 낸 파일이다. `--batches` 의 기본값도 `tm-analysis` 와 같다
- `--cw` 는 지표 이름마다 `get-metric-statistics` 의 데이터 점 목록을 담은 JSON 이다. `Rtt` 는 `p25`, `p50`, `p75` 확장 통계가 있어야 한다. 1분 해상도는 15일만 남으므로 실측 뒤 그 안에 받는다(`roadmap.md` Phase 9 작업)
- 묶음 시작 전 1분 안의 `Rtt` 표본도 그 묶음에 들어간다. 그 사이에 묶음 밖 시도를 돌리지 않는다. 1분 구간 하나가 두 묶음에 걸치면 멈춘다
- `--label` 은 환경 하나마다 따로 적는다. 실제 환경 묶음만 받고 적은 순서대로 그린다
