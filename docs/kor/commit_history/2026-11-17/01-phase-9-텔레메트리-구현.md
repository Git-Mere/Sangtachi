# Phase 9 텔레메트리 구현

## 왜 했나

`roadmap.md` Phase 9 작업 가운데 코드로 하는 것을 했다. 텔레메트리 서비스, 클라이언트의 레코드와 업로드, 배포 도구의 텔레메트리
대상, 분석 스크립트다. 작업자 셋이 서로 겹치지 않는 영역을 나눠 맡았고, 작업자들이 물은 빈칸의 답을 살아 있는 문서에 옮겼다.

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| 텔레메트리 서비스. `report` 하나, 레코드 검사, 방 대조, 저장, CloudWatch 지표, 입력 상한 | 새 `telemetry-server/telemetry/` |
| 제어 서버 코드를 텔레메트리가 쓰도록 매개변수를 더함. 기본값이 지금 동작이다 | `control-server/controlplane/server.py`, `log.py`, `__main__.py`, 변이 표 |
| 클라이언트의 `--telemetry`, 레코드와 세션 값, `[telemetry]` 스레드와 업로드, `telemetry_upload_failed` | `client/`, `tests/`, `scripts/cli-check.ps1`, CMake |
| 배포 도구의 `--target tm`, 연기 시험, `verify.py --tm-load` | `tools/cp-deploy` |
| 분석 스크립트 | 새 `tools/tm-analysis`. 실측 묶음 파일 자리 `tools/tm-analysis/local/` 을 `.gitignore` 에 더함 |
| `PENDING_COUNTERS` 에서 `telemetry_upload_failed` 를 뺌 | `tools/docgate/docgate.py` |
| 패키지 구조, `controlplane` 재사용, 전체 상한의 자리와 셈법, 로그와 카운터, `boto3` 시간 제한, CloudWatch 시각과 해상도, 종료 때 묶음 버림, journald 드롭인, 대시보드 성공률과 분석의 차이 | `architecture.md` 3.4 |
| `instance_id` 는 텔레메트리를 켠 경우만 만듦 | `architecture.md` 3.5·6.3 |
| NAT 관측값 셋의 정의, `room_id` 형식 검사 | `architecture.md` 6.3 |
| `interval` 의 다음 구간, 끝난 세션, 지터의 실수 보관과 반올림, 고정 로그 키 `telemetry.off`·`telemetry.upload_failed` | `architecture.md` 9장 |
| 레포 구조 | `architecture.md` 10장 |
| 구간 타이머의 자리, 단독 소유 예외 둘, 업로드 connect 시간 제한 | `concurrency.md` 4·6·7장 |
| 분석의 셈법(제외한 OK, `ABORTED`, `PEER_FAILED`, Wilson 95%), 부하 방법을 정하는 시점, 부하 레코드의 모양 | `roadmap.md` Phase 9 |
| 텔레메트리 표의 한계 | `experiments.md` 5장 |
| 수집 항목과 싣지 않는 것 | 루트 `README.md`, 배포물 `NOTICE.txt`(`scripts/package-gui.ps1`) |
| 다음 할 일 | `plan.md` |

## 결정

작업자 질문에 답하며 정한 것이다. 모두 위 자리에 옮겼다.

- 분석: 시도와 성공은 플레이어 `establish` 만. `OK` 는 `room_known` 이 `true` 일 때만 성공이고 아니면 분모와 분자에서 빼고 "제외한
  OK" 로 적는다. `ABORTED` 는 분모에서 뺀다. `PEER_FAILED` 를 포함한 그 밖의 결과는 실패다. 구간은 Wilson 95%
- NAT 관측값의 공인 IP 는 STUN 단계가 처음 받은 성공 응답의 매핑 주소다. `same_lan` 은 상대의 반사 후보가 없으면 `false` 다.
  STUN 단계가 성공하지 않으면 STUN 필드는 `null` 이다
- `room_id` 는 `null` 이거나 `control_plane.md` 2.1 정규형이어야 한다. 아니면 `bad_request`
- 전체 상한은 파싱과 출발지 검사를 지난 요청만 센다. 토큰 버킷 용량 5, 초당 5
- 텔레메트리 서비스의 카운터 여섯과 로그 이벤트 셋. `unavailable` 은 동시 연결 상한과 전체 상한을 합쳐 센다
- `boto3` 시간 제한은 제어 서버의 DynamoDB 호출과 같다. CloudWatch 클라이언트도 같다. 종료 때 보내지 않은 지표 묶음은 버린다
- journald 상한은 `/etc/systemd/journald.conf.d/` 드롭인이다
- `telemetry_upload_failed` 는 `[telemetry]` 가 올리는 원자 변수다. 업로드 connect 시간 제한은 1.5초다
- 텔레메트리 서비스는 공용 모듈을 따로 빼지 않고 `controlplane` 을 import 한다. 바꾸는 것이 경로 하나, 출발지 예산 값과 셈법, 로그의
  `src` 와 이벤트 표, 카운터 이름뿐이라서다

## 검증

작업자 보고서의 결과다. 실제 AWS 에 닿는 시험은 돌리지 않았다.

| 무엇 | 결과 |
|------|------|
| `telemetry-server` `pytest -q --store` | 657 passed |
| `telemetry-server` `tests/mutate.py --store` | 변이 57개 모두 잡힘 |
| `control-server` `pytest -q`, `pytest -q --store` | 747 passed 290 skipped, 1037 passed |
| `control-server` `tests/mutate.py http log rate_limit server` | 변이 179개 모두 잡힘 |
| `tools/cp-deploy` `pytest` | 38 passed |
| `tools/tm-analysis` `pytest` | 51 passed. 변이 21개 모두 잡힘 |
| 로컬 끝-끝(DynamoDB local 위의 텔레메트리 서비스) | 정상 200, 같은 레코드 400 `bad_request`, `{"v":2}` 400. 로그에 출발지 IP 없음 |
| `scripts/test.ps1 -Full` (리뷰 수정 뒤) | 통과. `ctest` 689, `cli-check` 49/49, `e2e-check`, `lobby-check`, `session-check` 0 failed 0 not run |
| 클라이언트 변이 | 리뷰 전 13개, 리뷰 수정의 2개 모두 잡힘 |
| 클라이언트와 텔레메트리 서비스의 로컬 끝-끝 | 두 쪽 모두 `establish`(seq 0, `OK`, `room_known` `true`), `interval`(seq 1, 60000ms 와 60016ms, RTT 표본 11개), `end`(seq 2, `CLOSED`) 가 테이블에 있다. `telemetry_upload_failed` 0 |
| `docgate.py`, `--claims` | `VERDICT: pass` |


확인하지 못한 것: Linux 빌드(`mere-galaxy` 에 닿지 않음), GUI 빌드와 `package-gui.ps1` 실행(구문 검사만), 실제 인스턴스의 `deploy.py --target tm` 과 `verify.py --target tm`.

## 크로스 모델 리뷰

작업자마다 차이를 따로 리뷰했다.

| 대상 | 지적 | 처리 |
|------|------|------|
| 분석 | `--max-rcu` 가 NaN 과 무한대를 받는다 | 고침. `math.isfinite` 검사와 시험 |
| 분석 | `--load` 가 레코드 구조를 검사하지 않는다 | 기각. 입력은 이 도구의 `--save` 출력이고 레코드는 서버가 검사해 저장한 것이다 |
| 텔레메트리 서비스 | `room_id` 형식 검사가 불투명 값 계약(분리 3)과 어긋난다 | 기각. 정상 클라이언트는 정규형만 보내고, 서버는 값을 바꾸지 않고 저장하므로 불투명 계약과 어긋나지 않는다 |
| 텔레메트리 서비스 | `SystemMaxUse` 는 영속 저널만 묶는다 | 고침. `RuntimeMaxUse=200M` 을 더함 |
| 클라이언트 | 소멸자가 시간 상한 없이 join 한다 | 고침. 본체가 0 이 아닌 코드로 돌아오는 경로도 `[telemetry]` 를 멈추고 2초 상한으로 join 하고, 넘기면 그 코드로 `_Exit` 한다. `cli-check` 사례 하나. `_Exit` 분기를 직접 밟는 시험은 없다 |
| 클라이언트 | 등록 목록의 중복 제거 때문에 공인 IP 호스트에서 `local_differs_public` 이 `true` 가 된다 | 고침. 후보 수집이 중복 제거 전의 로컬 주소를 따로 돌려주고 그것과 비교한다. 공인 IP 인터페이스 시험과 변이 2개 |
| 클라이언트 | `interval` 을 처리 시각에서 다시 걸어 밀린다 | 기각. `interval_ms` 가 실제 길이를 싣고 밀림은 밀리초 단위다 |
| 클라이언트 | 틀린 `--telemetry` 값이 로그에 나간다 | 기각. `--server` 와 같은 처리다. 6.3 이 올리지 않는 것은 업로드 레코드이고 로컬 로그가 아니다 |

최종 리뷰는 세 렌즈로 나눴다. 클라이언트 수정, 문서와 코드의 일치, 스테이징한 변경 전체다.

| 렌즈 | 지적 | 처리 |
|------|------|------|
| 전체 | 서버가 `setup_ms` 가 `null` 인 `OK` 를 받으므로 분석 스크립트의 사분위가 `TypeError` 로 죽는다 | 고침. 분석이 `null` 수립 시간을 분포에서만 빼고 성공으로는 센다. 시험 하나 |
| 클라이언트 수정 | `_Exit` 전에 표준 출력과 표준 오류를 비우다 막힐 수 있다 | 기각. 이번 변경 전부터 있던 동작이고 `concurrency.md` 7장 종료가 정한 것이다 |
| 클라이언트 수정 | 본체에서 예외가 빠져나가면 상한 없는 소멸자 join 을 탄다 | 기각. 그 자리까지 오는 예외는 메모리 부족 같은 것뿐이다 |
| 클라이언트 수정 | 이른 실패 반환에서 `[control]` 채널의 소멸자가 상한 없이 join 한다 | 기각. 이번 변경 전부터 있던 `[control]` 의 동작이고 이번 범위가 아니다 |
| 문서 | `--tm-load` 의 거절된 요청이 누락 수에 든다는 문장이 코드와 다르다 | 고침. 순번은 받아들여진 요청에서만 는다 |
| 문서 | `establish` 를 잃은 시도의 누락 수가 늘 1이라는 문장 | 고침. 적어도 1이다 |
| 문서 | 업로드 한 번이 4.5초까지라는 문장 | 고침. 나뉜 송신마다 걸려 더 길어질 수 있다 |
| 문서 | 배포물 안내에 NAT 관측값과 묶음 키가 빠졌다 | 고침 |
| 문서 | README 의 NAT 관측값이 늘 참/거짓이라는 문장 | 고침. 관측 전이면 `null` |
| 문서 (수정 확인) | 배포물 안내가 여전히 참/거짓만 적는다 | 고침 |
| 문서 (수정 확인) | `SetupTime` 이 `null` 을 넣지 않는 조건과 그로 인한 대시보드와 분석의 차이가 빠졌다 | 고침. `architecture.md` 3.4. 다음 확인에서 그 차이가 `room_known` 이 `true` 인 플레이어 `establish` 에만 해당한다는 한정을 더했다 |
