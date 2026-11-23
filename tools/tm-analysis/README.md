# tm-analysis

텔레메트리 테이블에서 실측 묶음의 레코드를 읽어 [`experiments.md`](../../docs/kor/experiments.md) 에 실을 표를 낸다. `boto3` 와
표준 라이브러리만 쓴다. 무엇을 내야 하는지는 [`docs/kor/roadmap.md`](../../docs/kor/roadmap.md) Phase 9 의 분석 스크립트 항목과
통계 설계 표가 갖는다. 레코드의 저장과 필드는 [`architecture.md`](../../docs/kor/architecture.md) 3.4 저장과 CloudWatch 지표, 6.3
레코드 필드가 갖는다. **무엇을 어느 레코드에서 세는지는 이 README 의 "무엇을 세는가" 가 갖는다.**

## 실측 묶음 파일

텔레메트리는 환경을 싣지 않는다. 실측한 사람이 묶음마다 UTC 시간 범위와 환경을 이 파일에 적고, 도구가 시간으로 가른다.
분 단위 시각이 들어 있어 공개하지 않는다([ADR 0001](../../docs/kor/decisions/0001-직접-연결-대비책-미도입.md) 따라야 할 조치
2번). 기본 위치는 `tools/tm-analysis/local/batches.txt` 이고 `local/` 은 `.gitignore` 가 막는다.

```text
# 시작(UTC)             끝(UTC)                환경       종류
2026-11-20T10:00:00Z 2026-11-20T11:30:00Z home-home  real
2026-11-20T12:00:00Z 2026-11-20T12:10:00Z ctl-stun   controlled
```

- 한 줄에 묶음 하나다. 필드 넷을 공백으로 가른다. `#` 뒤는 주석이다
- 시각은 `YYYY-MM-DDTHH:MM:SSZ` 꼴의 UTC 다. 구간은 시작을 넣고 끝을 뺀다
- 환경은 공백 없는 가명이다. 같은 환경을 여러 묶음에 적으면 표에서 하나로 합친다
- 종류는 `real`(실제 환경) 또는 `controlled`(통제된 실패 `spec.md` A-2, 에뮬레이션한 NAT [ADR 0040](../../docs/kor/decisions/0040-목적지-의존-매핑-에뮬레이션.md))다. 두 종류는 따로 집계한다
- 묶음끼리 겹치거나 한 환경이 두 종류로 적히면 거부한다
- **끝은 그 묶음의 마지막 세션이 끝난 뒤로 적는다.** 시도는 seq 가 가장 작은 레코드를 받은 시각으로 묶음에 들어간다.
  세션의 `interval` 이나 `end` 가 묶음 끝 뒤에 오면 경고를 내고, 마지막 묶음의 끝 뒤에 온 것은 읽지 않는다

## 실행

```text
python tools/tm-analysis/tmanalysis.py --table <텔레메트리 테이블> --region <리전> [--save tools/tm-analysis/local/records.json]
python tools/tm-analysis/tmanalysis.py --load tools/tm-analysis/local/records.json
```

이 PC 와 인스턴스 역할은 텔레메트리 테이블을 읽지 못한다. 테이블을 읽는 `--save` 는 소유자가 AWS CloudShell 에서 돈다. `tmanalysis.py` 를 `~/tm/` 에, 묶음 파일을 `~/tm/local/batches.txt` 에 올리고, `python3 tmanalysis.py --table <텔레메트리 테이블> --region <리전> --save local/records.json > local/report.md` 로 돌린 뒤 두 파일을 받아 이 레포의 `tools/tm-analysis/local/` 에 둔다. 그 뒤는 `--load` 로 이 PC 에서 다시 낸다.

| 옵션 | 기본값 | 뜻 |
|------|--------|-----|
| `--batches` | `local/batches.txt` | 위 묶음 파일 |
| `--table`, `--region` | 없음 | 읽을 테이블과 리전. 자격 증명은 boto3 의 기본 경로다. 읽기만 한다 |
| `--max-rcu` | `5` | 초당 소비 읽기 용량의 상한. 테이블의 RCU 5(`architecture.md` 3.4 저장)다 |
| `--save` | 없음 | 읽은 레코드를 JSON 으로 저장한다. `local/` 아래에 둔다 |
| `--load` | 없음 | 저장한 레코드로 표를 다시 낸다. 테이블을 읽지 않는다 |

**읽기 용량.** 키가 `pk`/`seq` 라 시간 범위는 Scan 과 `received_at_ms` 필터다. 필터는 읽은 뒤에 걸리므로 돌릴 때마다
테이블 전체를 읽는다. 한 페이지를 `--max-rcu` 의 8배 항목으로 자르고(레코드 한 건은 1KB 안쪽, 최종 일관 읽기는 4KB 에
0.5 RCU), 페이지마다 응답의 소비 용량을 보고 초당 `--max-rcu` 를 넘지 않게 쉰다. 같은 자료로 표를 다시 낼 때는 `--save` 와
`--load` 를 쓴다.

출력은 Markdown 이고 표준 출력으로 낸다. 날짜까지만 내고 시각, `instance_id`, `room_id` 를 내지 않는다.

## 무엇을 세는가

| 값 | 쓰는 레코드 |
|----|-------------|
| 시도, 성공 | 플레이어의 `establish`. `ABORTED` 는 분모에서 빼고 개수만 적는다. `OK` 는 `room_known` 이 `true` 일 때만 성공이고, 아니면 분모에서도 빼고 개수만 적는다. 그 밖의 결과(`PEER_FAILED` 포함)는 `room_known` 과 무관하게 실패로 센다 |
| Wilson 구간 | 95%, z = 1.959963984540054 |
| 실패 코드별 개수 | 위 실패 |
| 매핑 거동별, 동일 LAN 별, 빌드 종류별 | 위 시도를 플레이어 레코드의 `stun_same_endpoint`, `local_differs_public`, `same_lan`, `build_kind` 값(`null` 포함)으로 가른다 |
| 호스트와 짝지은 매핑 거동별 | 위 시도를 플레이어의 `stun_same_endpoint` 값과 같은 `room_id` 의 호스트 `establish` 값의 짝으로 가른다. `local_differs_public` 도 같다. 호스트의 STUN 은 방을 만들 때 한 번이라 방으로 짝짓는다(`architecture.md` 6.3). 방이 없거나 그 방의 호스트 `establish` 가 없으면 "짝 없음", 그 방의 호스트 값이 엇갈리면 "불일치"다 |
| 플레이어 수립 시간 | 위 성공의 `setup_ms` |
| 호스트 수립 시간 | 호스트의 `establish`, `OK`, `room_known` 이 `true`. 기점이 달라 플레이어 값과 비교하지 않는다(`architecture.md` 9장) |
| RTT, 지터 | 양쪽의 `interval` 과 `end`, `room_known` 이 `true`. RTT 는 표본마다, 지터는 `null` 이 아닌 레코드마다 |
| 손실 | 양쪽의 `end`, `room_known` 이 `true`. `total_expected - total_accepted` 의 합을 `total_expected` 의 합으로 나눈다 |
| 세션 지속 시간, 종료 결과 | 플레이어의 `end`, `room_known` 이 `true` |
| 누락 | 시도마다 빈 seq 의 수. `room_known` 과 무관하다. 시도의 마지막 레코드나 시도 전체를 잃은 것은 들지 않는다 |

대표값은 중앙값과 사분위 범위다. 정렬한 표본을 선형 보간한다(`experiments.md` 1장 표기). 평균과 표준편차는 내지 않는다.
모든 값에 표본 수 `n` 이나 분모를 붙인다.

실제 환경이 둘 이상이면 끝에 실제 환경을 합친 표를 하나 더 낸다. 통제된 실패는 합치지 않는다.

## 시험

```text
python -m pytest -q -p no:cacheprovider tools/tm-analysis
```

DynamoDB 를 부르지 않는 계산부다. 거르기, Wilson, 사분위, 누락 수, 묶음 파일과 배정, Scan 의 페이지 조절(가짜 테이블)을
본다. Wilson 의 기대값은 점수 검정 이차식의 근을 따로 구한 것이고, 사분위는 numpy 기본 방식과 같은 값이다.
