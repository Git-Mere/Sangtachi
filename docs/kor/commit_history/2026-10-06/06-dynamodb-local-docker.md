# DynamoDB local 을 Docker 로 한 번 띄웠다

## 왜 했나

`plan.md` 다음에 할 일 1번이다. ADR 0014 가 DynamoDB local 을 Docker 로 띄우기로 정하면서, 이미지
태그와 실행 명령은 실제로 한 번 띄운 뒤 넣는다고 적었다. 저장소 소유자가 Docker Desktop 을 띄웠다.

## 한 것

| 무엇 | 결과 |
|------|------|
| `docker info` | 서버 29.8.2, linux |
| 이미지 | `amazon/dynamodb-local:3.3.1`. Docker Hub 에서 `latest` 와 다이제스트가 같은 가장 최근 태그다 |
| 실행 | `control-server/README.md` 의 두 명령. 멈추고 다시 띄우는 것까지 돌렸다 |

명령은 `control-server/README.md` 에 두었다. ADR 0014 는 "도구에 넣는다" 고 적었는데, 그 도구의
README 를 그 자리로 보았다. 명령 두 줄이라 스크립트로 감쌀 이유가 없었다.

## 실측으로 확인한 것

저장소 계층이 기대는 동작을 `boto3` 로 한 번씩 돌렸다. 일회용 스크립트이고 레포에 두지 않았다.

| 무엇 | 결과 | 왜 봤나 |
|------|------|---------|
| 자격 증명 없이 요청 | `NoCredentialsError`. 요청이 나가지 않는다 | `control_plane.md` 7.6 이 "로컬은 자격 증명이 필요 없다" 고 적었다. **틀렸다.** 고쳤다 |
| 숫자 속성을 읽은 형 | `Decimal` | 6.3 의 형 변환 규칙 |
| 트랜잭션 취소 사유 | `['None', 'ConditionalCheckFailed', 'None']`. 넣은 항목과 같은 위치에 하나씩 | 6.3 취소 사유 표가 위치로 항목을 식별한다 |
| `size(candidates) = :zero` 조건 | 빈 목록에서 참 | 6.3 서버 회수 조건 |
| `attribute_not_exists(candidates) OR size(candidates) = :zero` | 속성이 없을 때도 참 | 같은 조건 |
| TTL 켜기 | `update_time_to_live` 가 받는다 | 6.2 |

DynamoDB local 의 TTL 이 실제로 항목을 지우는지는 보지 않았다(ADR 0004 "확인하지 못한 것" 3번).

## 무엇을 바꿨나

| 파일 | 내용 |
|------|------|
| `control_plane.md` 7.6 | 자격 증명 문단. 서버가 받는 변수는 여전히 없고, 로컬 시험 하네스가 `boto3` 표준 변수에 실제 키가 아닌 값을 넣으며, 엔드포인트가 루프백일 때만 저장소 시험을 돈다 |
| `control-server/README.md` | "아직 없는 것" 을 "DynamoDB local" 절로. 명령, 태그, 다이제스트, 포트 선택 이유 |
| `plan.md` | 1번을 지우고 번호를 당겼다. 새 1번에 저장소 시험 하네스를 더했다 |

루프백 확인은 아직 코드가 없다. 저장소 시험을 만들 때 같이 넣는다. `plan.md` 1번이 그것을 적었다.

## 크로스 모델 리뷰

Codex 1회. correctness + record 묶음. 두 렌즈 모두 `none`.
