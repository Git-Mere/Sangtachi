# 0004. 제어 평면 상태 저장소를 SQLite 대신 Amazon DynamoDB로 한다

- 상태: 채택
- 날짜: 2026-09-22
- 관련: [`../architecture.md`](../architecture.md) 3.3, [`../spec.md`](../spec.md) C-3·NFR-5, [`../roadmap.md`](../roadmap.md) Phase 3, [ADR 0003](0003-텔레메트리-서비스-분리.md)

## 맥락

원래 설계는 EC2 인스턴스 안의 SQLite 파일에 방과 피어 상태를 두었다. `spec.md` C-3이
"AWS EC2 단일 인스턴스와 SQLite"였고, `architecture.md` 11장의 외부 의존성 표는 Python
표준 라이브러리에 `sqlite3`를 올려 두었다. 서드파티 의존성이 하나도 없다는 것이 이 선택의
장점이었다.

저장소를 관리형 서비스로 바꾸기로 했다. 이유는 둘이다.

1. **상태가 인스턴스에 묶이지 않는다.** SQLite 파일은 그 EC2 인스턴스의 디스크에 있다.
   인스턴스를 바꾸거나 늘리면 상태를 같이 옮겨야 한다. NFR-3은 서버 재시작 후 방 상태
   유지를 요구하는데, SQLite로는 "같은 인스턴스가 같은 디스크로 다시 뜬다"가 전제가 된다.
2. **[ADR 0003](0003-텔레메트리-서비스-분리.md)으로 서비스가 둘이 됐다.** SQLite를 그대로
   쓰려면 둘 중 하나를 골라야 한다. 한 파일을 공유하면 파일 잠금이 두 서비스의 공유 지점이
   되어 분리해 놓고 저장소에서 다시 묶인다. 파일을 따로 두면 그 문제는 없지만 맥락 1번이
   양쪽에 그대로 남고 백업 대상이 둘이 된다. **강제되는 것은 아니다.** 어느 쪽도 맥락 1번을
   풀지 못한다는 것이 요점이다.

## 결정

**Amazon DynamoDB를 쓴다.** 아래 다섯 개를 이 결정에 포함한다. 근거는 전부 AWS 공식 문서
문장이고 출처는 각 항목에 적었다.

**1. 읽은 값으로 판정하는 읽기는 테이블에 `ConsistentRead=true`를 건다.**

DynamoDB의 기본 읽기는 최종 일관성이다. "Eventually consistent is the default read consistent
model for all read operations."
([HowItWorks.ReadConsistency](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/HowItWorks.ReadConsistency.html))
`get_peers`가 이 값을 기본값으로 읽으면 **상대가 방금 등록한 후보가 응답에서 빠질 수 있다.**
그러면 클라이언트는 상대 엔드포인트 없이 펀칭을 시작하고 `HOLE_PUNCH_TIMEOUT`으로 끝난다.
원인이 NAT가 아니라 읽기 일관성인데 실패 코드는 NAT처럼 보인다.

**GSI로 그 판정을 하지 않는다.** "Strongly consistent reads are only supported on tables and
local secondary indexes. Strongly consistent reads from a global secondary index or a DynamoDB
stream are not supported." (같은 문서)

대가는 비용 2배다. "Eventually consistent reads are half the cost of strongly consistent reads."
(같은 문서)

**2. 가상 IP 배정은 조건부 쓰기 선점으로 한다. 원자적 카운터를 쓰지 않는다.**

IP 하나를 아이템 하나의 키로 두고 `attribute_not_exists()`로 선점한다. "DynamoDB evaluates a
condition expression on a write operation against the item identified by the request's key, so
attribute_not_exists(Id) is true only when no item with that Id value already exists."
([Expressions.ConditionExpressions](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/Expressions.ConditionExpressions.html))

원자적 카운터로 순번을 뽑는 방식은 버렸다. 문서가 멱등하지 않다고 적는다. "With an atomic
counter, the updates are not idempotent." 그리고 "An atomic counter would not be appropriate
where overcounting or undercounting can't be tolerated ... it is safer to use a conditional
update instead of an atomic counter."
([WorkingWithItems](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/WorkingWithItems.html))
`10.100.0.0/24`는 배정 가능한 주소가 254개뿐이라 재시도가 번호를 건너뛰면 주소가 샌다.

**조건이 실패해도 쓰기 용량을 쓴다.** "If a ConditionExpression evaluates to false during a
conditional write, DynamoDB still consumes write capacity from the table." (같은 문서)
그래서 빈 주소를 무작정 순회하며 시도하지 않는다. 탐색 방법은 테이블 설계와 같이 정한다.

**3. 방 만료 판정을 TTL에 맡기지 않는다.**

AWS 문서는 삭제 시점을 만료 후 "며칠 안"으로 **설명할 뿐이고 상한을 정하지 않는다.**
"DynamoDB automatically deletes expired items within a few days of their expiration time."
그리고 만료 시각이 지나도 삭제 전까지는 보인다.
"If they are not filtered, they'll continue to show in read and write operations until they are
deleted by the background process."
([TTL](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/TTL.html),
[ttl-expired-items](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/ttl-expired-items.html))

**TTL은 저장 공간 회수 수단으로만 쓴다.** "이 방이 살아 있는가"는 애플리케이션이 만료 시각을
직접 보고 판정한다.

**4. 용량 모드는 provisioned로 시작한다.**

문서의 일반 권장은 on-demand다. "On-demand mode is the default and recommended throughput
option."
([on-demand-capacity-mode](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/on-demand-capacity-mode.html))
그러나 프리 티어 용량은 provisioned에만 붙는다. 가격 페이지가 월 25 WCU / 25 RCU / 25 GB를
적으면서 "It uses provisioned capacity and the DynamoDB Standard table class."라고 명시한다.
([pricing](https://aws.amazon.com/dynamodb/pricing/))
학기 프로젝트라 **무료 한도 안에서 돌리는 것**을 용량 계획 회피보다 위에 둔다. **비용이 0이
된다고 말하지 않는다.** 프리 티어의 적용 범위와 만료 조건은 아래 "확인하지 못한 것" 2번이고,
Phase 3 착수 전에 계정에서 직접 확인한다.

**모드 전환에 제한이 있다.** "You can switch tables from provisioned capacity mode to on-demand
mode up to four times in a 24-hour rolling window."
([bp-switching-capacity-modes](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/bp-switching-capacity-modes.html))

**5. EC2에서 액세스 키를 쓰지 않는다. IAM 역할을 붙인다.**

"If you're running on an EC2 instance, use AWS IAM roles." 그리고 "if you've launched an EC2
instance with an IAM role configured, there's no explicit configuration you need to set in Boto3
to use these credentials."
([boto3 credentials](https://docs.aws.amazon.com/boto3/latest/guide/credentials.html))

## 대안

**대안 1. SQLite 파일 하나를 두 서비스가 공유한다.**
버렸다. 서드파티 의존성이 0이라는 장점이 컸다. 그러나 파일 잠금이 두 서비스의 공유 지점이
되어, 분리해 놓고 저장소에서 다시 묶는 셈이 된다. **파일을 따로 두는 선택지는 아래 대안 2로
따로 뒀다.** 맥락 2번에서 적었듯 SQLite 유지는 이 둘 중 하나를 고르는 일이지, 공유가
강제되는 것이 아니다.

**대안 2. 서비스마다 SQLite 파일을 따로 둔다.**
버렸다. 잠금 공유 문제는 풀리지만 상태가 여전히 인스턴스 디스크에 묶인다. 위 맥락 1번이
그대로 남는다.

**대안 3. RDS 같은 관계형 관리형 DB를 쓴다.**
버렸다. 저장할 것은 방과 피어라는 단순한 키-값 구조이고 조인이 필요 없다. 관계형 엔진의
기능이 필요 없는데 인스턴스 비용과 운영 부담이 붙는다.

**대안 4. DynamoDB를 쓰되 기본 읽기(최종 일관성)로 둔다.**
버렸다. 비용이 절반이지만 위 결정 1번의 실패 양상이 **NAT 실패로 위장한다.** Phase 9가
NAT 거동별 성공률을 재는 프로젝트에서, 저장소 일관성 때문에 생긴 실패가 NAT 실패로 집계되면
결과 전체가 오염된다. 싸게 틀리는 것보다 비싸게 맞는 쪽을 고른다.

## 결과

**얻는 것.**

- 상태가 인스턴스 디스크에 묶이지 않는다. 인스턴스를 바꿔도 방 상태가 남는다 (NFR-3)
- **파일 잠금이라는 공유 지점이 없어진다** ([ADR 0003](0003-텔레메트리-서비스-분리.md) 분리 1).
  **"저장소를 공유하지 않는다" 는 뜻이 아니다.** 두 서비스는 여전히 같은 DynamoDB를 쓴다.
  사라지는 것은 한 프로세스가 잠금을 쥐면 다른 쪽이 기다리는 관계뿐이다. **테이블을 나눌지,
  IAM 권한을 서비스마다 좁힐지, 장애 도메인을 분리할지는 정하지 않았다.** Phase 9의 텔레메트리
  스펙 확정 때 같이 정한다
- 액세스 키를 소스나 문서에 둘 이유가 없어진다

**치르는 것.**

- **서드파티 의존성이 생겼다.** `boto3`는 표준 라이브러리가 아니다(`sys.stdlib_module_names`에
  없고 `pip install boto3`가 공식 절차다.
  [boto3 quickstart](https://docs.aws.amazon.com/boto3/latest/guide/quickstart.html)).
  `spec.md` NFR-5는 비기본 서드파티 의존성의 **사전 승인**을 요구한다. **승인받았다.**
  Wintun에 이어 두 번째 승인 의존성이다
- **읽기 비용이 2배다.** 결정 1번의 대가다
- **로컬 시험이 일관성 결함을 잡지 못한다.** DynamoDB local은 공식 수단이지만 "Read operations
  are eventually consistent. However, due to the speed of DynamoDB local running on your
  computer, most reads appear to be strongly consistent."라고 문서가 적는다
  ([DynamoDBLocal.UsageNotes](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/DynamoDBLocal.UsageNotes.html)).
  **`ConsistentRead`를 빠뜨린 결함은 로컬에서 통과한다.** 실제 테이블에서 확인해야 한다.
  같은 문서가 `TransactionConflictException`도 로컬에서는 발생하지 않는다고 적는다
- **프리 티어를 넘기면 과금된다.** provisioned 25/25를 넘는 요청은 스로틀되거나 비용이 붙는다

**확인하지 못한 것.**

다음 셋은 공식 문서에서 확인하지 못했다. **확인한 것처럼 적지 않는다.**

1. TTL 삭제 지연의 수치 상한이나 SLA. 문서 표현은 "within a few days"뿐이다
2. DynamoDB 프리 티어 25 WCU / 25 RCU / 25 GB가 영구라고 **직접 말하는** 문장. 가격 페이지는
   월 단위 혜택으로 적고 만료를 명시하지 않을 뿐이다. 2025년 프리 티어 개편이 계정 개설
   시점에 따라 어떻게 다른지도 확인하지 못했다
3. DynamoDB local의 TTL 동작

1번과 3번은 결정 3번(TTL을 만료 판정에 쓰지 않는다)으로 이미 피해 간다. 2번은 비용 위험이므로
Phase 3 착수 전에 계정에서 직접 확인한다.
