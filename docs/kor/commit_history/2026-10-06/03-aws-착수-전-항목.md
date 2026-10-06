# AWS 계정 쪽 착수 전 항목 다섯을 닫았다

## 왜 했나

`roadmap.md` Phase 3 착수 전 항목 가운데 AWS 계정이 필요한 다섯이 남아 있었다. Elastic IP·DNS,
자격 증명, 프리 티어, 배포 설정 값, 시계 전제 둘이다. 저장소 소유자가 콘솔과 인스턴스에서 직접
만들고 확인한 값을 알려 줬고, 에이전트가 판정했다.

## 확인한 것

| 항목 | 결과 | 근거 |
|------|------|------|
| 시계 전제 1 | 통과. `clock_gettime(CLOCK_MONOTONIC)`, `adjustable=False` | 인스턴스의 `time.get_clock_info('monotonic')` |
| 시계 전제 2 | 통과. 재부팅 전후 `boot_id` 가 달랐다 | 두 값을 비교했다 |
| 자격 증명 | 통과. IAM 역할, 자격 증명 파일 없음 | `aws sts get-caller-identity`, `ls ~/.aws/credentials` |
| 프리 티어 | 통과. DynamoDB 25 WCU / 25 RCU / 25 GB 가 Always Free | 계정의 Free Tier 화면. ADR 0004 "확인하지 못한 것" 의 그 항목이 닫혔다 |
| 테이블 | 키 `pk`·`sk`, TTL `ttl`, 인덱스 없음 | 콘솔 |
| Elastic IP | 붙였다 | 콘솔. `describe-addresses` 판정 명령은 돌리지 않았다 |
| 보안 그룹 | TCP 8000 전체 허용 | 콘솔 |

## 판정 중에 고친 것

소유자가 처음 알린 값에 문서와 어긋나는 것이 다섯 있었고, 소유자가 콘솔에서 고쳤다.

| 어긋난 것 | 처리 |
|-----------|------|
| 테이블 키가 `PK`·`SK` | 테이블을 지우고 `pk`·`sk` 로 다시 만들었다. 키 스키마는 만든 뒤 바꿀 수 없고, 문서 쪽을 바꾸는 것보다 작다 |
| TTL 꺼짐 | `ttl` 로 켰다 |
| 보안 그룹에 TCP 8000 없음 | 열었다 |
| IAM 정책에 `dynamodb:ConditionCheckItem` 없음 | 더했다. 트랜잭션 안의 ConditionCheck 는 이 권한이 따로 필요하다(AWS DynamoDB 개발자 가이드의 트랜잭션 IAM 절에서 원문을 받아 확인했다) |
| 용량 RCU 5, WCU 5 | 25·25 로 올렸다. 아래 계산 |

**용량은 `control_plane.md` 10장의 미결 "갱신 쓰기가 프리 티어 안에 드는가" 를 계산해 정했다.**
정원 미만 방 하나의 갱신 쓰기가 최악 2.4 WCU/s 이고 폴링하는 플레이어 하나가 최악 8 RCU/s 라
5 는 모자랐다. Always Free 한도가 리전과 결제 계정 단위로 25·25 인 것은 AWS DynamoDB 요금 페이지에서,
burst capacity 가 300초인 것은 개발자 가이드에서 원문을 받아 확인했다. 계산 자체는 재지 않았고
`roadmap.md` Phase 3 검증에 CloudWatch 확인을 걸었다.

**도메인은 만들지 않는다.** 소유자 결정이다. 클라이언트에 IPv4 리터럴을 준다. `windows-prereq.md`
10절이 이미 허용하던 운영이다.

## 무엇을 바꿨나

| 파일 | 내용 |
|------|------|
| `control_plane.md` 7.4 | "두 전제는 확인하지 않았다" 를 확인 결과로 |
| `control_plane.md` 7.6 | 배포 환경 표, IAM 동작 여섯, 용량과 부하 계산 |
| `control_plane.md` 10장 | 닫힌 미결 셋(테이블·용량·리전, Elastic IP·DNS 실제 값, 프리 티어 안의 갱신 쓰기)을 뺐다 |
| `roadmap.md` Phase 3 | 착수 전 다섯을 지웠다. 배포 작업 항목이 7.6 을 가리킨다. 검증에 "배포" 묶음(권한 오류 없음, CloudWatch 용량) |
| `windows-prereq.md` 6절·10절 | 검증 상태. 10절의 "DNS 와 둘 다" 를 DNS 를 쓰는 운영의 조건으로 |
| `plan.md` | 다음에 할 일 1번을 지웠다 |
| `.gitignore` | `deploy/*.local.md`. 그리고 작업 트리에만 있던 키 파일 이름 한 줄 |

**실제 값은 커밋하지 않았다.** 계정 번호, Elastic IP, 인스턴스 ID, 역할 이름, 리전, 테이블 이름,
`boot_id` 값은 `deploy/aws.local.md` 에 있고 ignore 된다. 스테이징된 diff 를 그 값들로 훑어
하나도 없는 것을 확인했다. 키 파일 이름은 원래 ignore 대상이고 키 내용이 아니다.

## 검증

| 무엇 | 결과 |
|------|------|
| `python tools/docgate/docgate.py` | `link` 0 건. 나머지는 영어 미러를 아직 만들지 않은 `mirror`·`parity` 다 |
| 스테이징된 diff 의 민감 값 | 위 값 아홉으로 훑어 0 건 |
| `git status --ignored deploy` | `deploy/` 전체가 ignore |

## 크로스 모델 리뷰

### 1라운드. 일관성 + 기록·유출 묶음 (warn 2, nit 1)

두 렌즈 모두 다뤘다.

| # | 등급 | 렌즈 | 지적 | 처리 |
|---|------|------|------|------|
| 1 | warn | record | 강한 일관성 `Query` 는 바이트 합을 4KB 단위로 올림하므로 항목당 1KB 가정으로 회당 1 RCU 를 보장할 수 없다 | 반영. 최악값으로 다시 계산했다. 폴링 하나 8 RCU/s(15KB, 4 RCU), `host_report` 읽기 1.2 RCU/s. 넷이 동시에 폴링하면 최악 32 RCU/s 로 25 를 잠깐 넘는다고 적었다 |
| 2 | warn | consistency | CloudWatch 의 소비 용량 원시 값은 기간 합이라 초당 25 와 바로 비교할 수 없다 | 반영. 1분 `Sum` / 60 과 `ThrottledRequests` 의 `Sum` 으로 |
| 3 | nit | record | 어긋난 값을 넷이라 했는데 표에는 다섯이다 | 반영 |


### 2라운드. 수리 확인 (warn 1)

| # | 등급 | 렌즈 | 지적 | 처리 |
|---|------|------|------|------|
| 1 | warn | repair | 폴링 최악값은 12KB 가 아니라 정원이 찬 방의 15KB 기준이라 4 RCU, 8 RCU/s 다 | 반영. 넷 동시 폴링은 최악 32 RCU/s |

### 3라운드. 수리 확인 (warn 3)

| # | 등급 | 렌즈 | 지적 | 처리 |
|---|------|------|------|------|
| 1 | warn | repair | `host_report` 읽기의 최악값이 정원이 막 찬 방(15KB)을 빼먹었다 | 반영. 호출당 8 RCU, 최악 1.6 RCU/s |
| 2 | warn | repair | 32 RCU/s 가 폴링만의 값인데 방 전체 읽기처럼 적었다 | 반영. 폴링만의 값이라 적고 `host_report` 읽기를 따로 더했다 |
| 3 | warn | repair | burst capacity 가 받는다고 단정했다 | 반영. AWS 가이드 원문("consume burst capacity for background maintenance ... without prior notice")을 받아 확인하고, 보장되지 않으며 스로틀이 나면 `internal` 이라고 적었다 |

### 4라운드. 수리 확인 (warn 1)

| # | 등급 | 렌즈 | 지적 | 처리 |
|---|------|------|------|------|
| 1 | warn | repair | 1.6 RCU/s 는 주기 호출만의 값이고 4.6 의 세션 종료 즉시 호출이 빠졌다 | 반영. 주기 호출만의 값이라 적고 즉시 호출이 호출당 8 RCU 씩 더해진다고 적었다 |

### 5라운드. 수리 확인

`LGTM - no blockers`.
