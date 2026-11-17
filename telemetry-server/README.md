# telemetry-server

Sangtachi 텔레메트리 서비스. 설계의 출처는 [`docs/kor/architecture.md`](../docs/kor/architecture.md) 3.4 텔레메트리 서비스와
6.3 텔레메트리 서비스 인터페이스다. 이 README 는 시험과 로컬 실행만 적는다.

| 파일 | 무엇 |
|------|------|
| `telemetry/server.py` | 입력 상한의 값, 출발지 예산(요청마다 하나), 전체 초당 상한, 로그 이벤트 표 |
| `telemetry/ingest.py` | 레코드 검사(6.3), 방 대조, PutItem |
| `telemetry/metrics.py` | CloudWatch 지표의 모음과 60초마다의 전송 |
| `telemetry/__main__.py` | 설정과 기동 |
| `tests/test_*.py` | 케이스 표와 그 시험 |
| `tests/mutants/<표>.py`, `tests/mutate.py` | 변이 목록과 실행기. 실행기는 `control-server/tests/mutate.py` 를 그대로 쓴다 |

**제어 서버의 코드를 import 한다.** HTTP 부분집합 파싱, 응답, 시간 제한, `MAX_INFLIGHT`, 출발지 표, 로그 줄, 설정 읽기 규칙은
`control-server/controlplane` 의 것이다. 그래서 `control-server` 가 import 경로에 있어야 한다. 시험은 `tests/conftest.py` 가 넣고,
배포는 릴리스 폴더에 두 패키지를 나란히 둔다([`tools/cp-deploy`](../tools/cp-deploy/README.md)).

## 시험

`telemetry-server/` 에서 돌린다. 의존성은 제어 서버와 같아서 그 venv 를 써도 된다.

```text
../control-server/.venv/Scripts/python -m pytest -q
../control-server/.venv/Scripts/python -m pytest -q --store
../control-server/.venv/Scripts/python tests/mutate.py --store
```

- `--store` 와 DynamoDB local 은 [`control-server/README.md`](../control-server/README.md) 의 것과 같다. 같은 컨테이너
  (`sangtachi-ddb`, `127.0.0.1:8001`)를 쓰고, 시험마다 `tm-test-<uuid>` 와 `tm-cp-test-<uuid>` 테이블을 만들고 지운다
- CloudWatch 는 가짜 클라이언트로만 본다. 실제 AWS 에 닿는 시험은 없다
- `mutate.py` 의 마지막 줄이 `VERDICT: pass` 여야 한다

## 로컬에서 띄우기

```text
export AWS_ACCESS_KEY_ID=fakeLocalOnly AWS_SECRET_ACCESS_KEY=fakeLocalOnly
export AWS_REGION=us-west-2 AWS_EC2_METADATA_DISABLED=true
export SANGTACHI_TM_TABLE=<텔레메트리 테이블> SANGTACHI_TM_CP_TABLE=<제어 평면 테이블>
export SANGTACHI_TM_ENDPOINT=http://127.0.0.1:8001 SANGTACHI_TM_PORT=18001
PYTHONPATH=../control-server ../control-server/.venv/Scripts/python -m telemetry
```

- `SANGTACHI_TM_ENDPOINT` 는 DynamoDB 에만 걸린다. CloudWatch 전송은 리전 기본 엔드포인트로 가므로 가짜 키로는 실패하고
  `metrics_put_error` 가 오른다
