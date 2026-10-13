# Plan

**이 파일은 다음 세션의 인수인계다.** 무엇이 남았고 무엇을 먼저 하는지만 적는다. 끝난 일의
경과는 [`commit_history/`](commit_history/README.md)가 갖는다.

## 남은 일이 어디에 있는가

| 축 | 출처 | 지금 |
|----|------|------|
| 구현 | [`roadmap.md`](roadmap.md) | Phase 2 까지 끝났다. Phase 3 은 산출물 확인 하나만 남았다(두 번째 기기, "대기 중인 것"). **Phase 4 를 재측정 전에 시작했다.** 아래 "다음에 할 일" 이 조각 순서다 |
| 대기 | 이 파일 "대기 중인 것" | 두 번째 기기나 측정이 있어야 풀린다 |
| 구현 때 같이 볼 자리 | 이 파일 같은 이름의 절 | `roadmap.md` 에 없는 것만 둔다 |
| 문서 부채 | 이 파일 "문서 부채" 절 | 1건 |

## 다음에 할 일

| 순서 | 무엇 | 왜 |
|:--:|------|-----|
| 1 | 친구 기기로 한꺼번에: `tools/nat-probe` 재측정(호스트 네 쌍), 방화벽 인바운드 실측, Phase 3 산출물, 필요하면 시연 상대 망의 매핑 수명 | 두 번째 기기가 필요한 일을 한 번에 한다. 재측정 결과로 방 생성 전 호스트 NAT 경고 여부를 정한다 (`roadmap.md` Phase 4 작업 머리) |
| 2 | 제어 서버 배포. 1차 D 의 표시 이름, 위생 대역, 헤더 이름 공백이 main 에 있고 아직 배포하지 않았다. **배포 전에 만든 방은 `PEER#` 에 `name` 이 없어 그 방의 요청이 `internal` 이 된다** (`control_plane.md` 6.3) | 승인은 받았다. 저장소 소유자가 다시 말할 때 올린다. 학교 Wi-Fi 에서 시도했을 때 SSH 가 호스트 키 불일치로 거부했다. `known_hosts` 는 고치지 않았다. 다른 망에서 다시 돌리고, 그래도 다르면 EC2 콘솔의 시스템 로그 지문과 대조한다 |
| 3 | Phase 4 3차 나머지: H 라우팅 표와 `DATA` 송신·`send` 명령·에코 응답기, I `ROSTER` 송신과 시점. G 는 끝났다. 둘의 착수 전 결정은 [ADR 0023](decisions/0023-3차-h-i-착수-전-결정.md) 이 정했다. **I 는 멤버 목록을 입력으로 받는 데까지다.** 그 입력을 `host_report` 응답에 잇는 일은 4차 J 다 | 2차가 이음새를 남겼다(아래 "구현 때 같이 볼 자리") |
| 4 | Phase 4 4차: J 랑데부·로비·기록 파일·클라이언트 이름 통합, K 같은 기기 검증 스크립트와 캡처 체크리스트 | 나머지 뒤 |

**재측정 전에 Phase 4 를 시작한 것은 저장소 소유자의 결정이다.** `roadmap.md` 는 재측정을 착수 전 항목으로 둔다. 측정 결과에 기대는 자리는 호스트 NAT 경고 하나이고 위 조각 어디에도 들어 있지 않다. 측정에서 직접 연결 판정이 뒤집히면 [ADR 0001](decisions/0001-직접-연결-대비책-미도입.md) 을 다시 연다

**전체 시험이 DynamoDB local 을 요구한다.** `scripts/lobby-check.ps1` 이 제어 서버 하네스를 띄우기 때문이다.
띄우는 명령은 `control-server/README.md` 에 있다.

**AWS 쪽 실제 값은 `deploy/aws.local.md` 에 있다.** 계정, 리전, Elastic IP, 인스턴스, 역할, 테이블
이름이다. 공개 레포라 `.gitignore` 가 막고 이 기기에만 있다. SSH 키도 레포 루트에 ignore 된 채
있다. 확인한 사실과 판정은 `control_plane.md` 7.4 와 7.6 이 갖는다.

## 대기 중인 것

| 무엇 | 막힌 이유 | 풀리는 조건 |
|------|-----------|-------------|
| 방화벽 인바운드 실측 | 상대 피어가 필요하다 | 두 번째 기기 |
| 로비 체류가 NAT 매핑에 무엇을 하는가 | 측정한 적이 없다. [`concurrency.md`](concurrency.md) 7장 로비가 "단정하지 않는다" 로 적었다 | Phase 4 의 `tools/nat-probe` 재측정과 같이 본다 |
| 시연 상대 망의 매핑 수명 | 두 집을 쟀다. 이 기기 집 Wi-Fi 하한 58초, 친구 집 Wi-Fi 300초 이상. 최솟값 58초로 [`protocol.md`](protocol.md) 11장 규칙(`간격 × 3 < 최솟값`)을 지킨다. 시연 상대가 이 둘이 아니면 그 망은 아직 모른다 | 시연 상대의 망에서 `tools/nat-lifetime` 을 돌린다. 그때마다 보안 그룹을 그 공인 IP `/32` 로 잠시 연다 |
| nat-probe 후속 | Windows에서 이어간다 | [`../../tools/nat-probe/README.md`](../../tools/nat-probe/README.md) |
| [`windows-prereq.md`](windows-prereq.md) 실물 검증 | Wintun 어댑터가 아직 없다 | Phase 6(Wintun), 8(시연) |
| 서로 다른 네트워크의 두 클라이언트 | `roadmap.md` Phase 3 산출물이다. 배포된 서버로 한 NAT 뒤의 두 프로세스는 돌렸다 | 두 번째 기기 |
| NFR-5 를 두 종류로 나눈 것의 교수 확인 | 확인 대상은 Catch2 승인이 아니라 요구를 제품 의존성과 시험 전용 의존성으로 나눈 것이다 | 제출 전 확인. 결과가 다르면 [ADR 0005](decisions/0005-시험-프레임워크-catch2.md)를 뒤집고 새 ADR 을 쓴다 |

Phase 1 의 두 머신 실측은 따로 기다리지 않는다. `roadmap.md` Phase 1 검증은 한 기기의 두 프로세스도
받고, 그것은 `scripts/e2e-check.ps1` 로 돌렸다. 실제 두 기기 사이의 송수신은 Phase 4 검증의 "같은 LAN 의
두 PC 에서도 `CONNECTED` 에 도달한다" 가 대신 본다.

## 구현 때 같이 볼 자리

문서 계약은 정해졌고 코드가 아직 그 자리에 없다. `roadmap.md` 에 이미 걸린 작업은 여기에 다시
적지 않는다.

| 자리 | 무엇 | 언제 |
|------|------|------|
| `tests/mutate.py` 의 시간 초과 | 상한에 걸리면 pytest 프로세스만 죽인다. 그 시험이 띄운 서버 프로세스가 남을 수 있다. 손자까지 거두려면 Windows Job Object 가 필요하다 | 변이 실행기를 다시 고칠 때 |
| 로컬 후보 수집의 실제 연결 | 1차 C 가 판정과 OS 조회를 만들었지만 `Lobby::on_stun_done` 은 아직 반사 후보만 등록한다. **그때까지 제품 경로의 자기 서브넷 집합이 비어 있어 7장 directed broadcast 행과 받은 후보 위생의 자기 서브넷 행이 아무것도 막지 않는다.** 남은 일은 수집 호출, 등록 본문, `EventLoop::set_own_subnets`, 수집 경고, `lobby.hpp` 경계 표와 `lobby_test` 갱신이다. 자기 Wintun 식별자는 Phase 6 까지 늘 비어 있다 | 4차 J |
| 4.5 `pkt_pos` 의 언더플로 | 첫 수용 패킷의 `position` 이 0 이라 그보다 앞선 번호의 재정렬 수용에서 `position - back` 이 64비트로 감싸 돈다. `tunnel/replay` 는 4.5 공식을 그대로 따르고 시험이 그 거동을 고정했다. 손실 집계가 `baseline` 이하를 배제한다는 4.5 의 주장과 어긋난다. `protocol.md` 4.5 를 먼저 고친다 | Phase 5 손실 집계 착수 전 |
| `is_valid_name` 의 자리 | `control_plane.md` 2.7 검사가 `tunnel/packet.hpp` 에 있어 기동 인자와 제어 평면 코드가 터널 패킷 모듈을 포함한다. 중립 헤더로 옮기고 `packet.hpp` 가 그것을 쓰는 편이 의존 방향이 맞다 | 다음에 그 헤더를 고칠 때 |
| 2차가 남긴 3차 이음새 | H: `Tunnel::set_terminal_handler`(라우팅 칸 비우기), `TunnelDeps::data_sink`(M-5 비교와 에코 응답기) | 3차 H |
| 4차 J 가 이어 붙일 자리 | `Tunnel::create_session`(이름 포함), `Session::start_punch`(10.2 펀치 지연 만료 때), 로비 `leave` 의 `Tunnel::leave_room`, 기록 파일 줄의 값(`failure()`, `peer_close_reason()`, `last_rtt_us()`), 3차 I 의 명부 입력(`host_report` 응답의 `peers`, 5.7). `main.cpp` 를 2차도 고쳤다 | 4차 J |
| `EventLoop` 의 대기 | 루프 수준 동작 보존을 지키는 시험이 없다. `busy_ ? 0 :` 를 지워도 시험 전량이 통과한다. 대기 자체를 주입할 수 있어야 풀리고, 지금은 `platform::wait_any` 가 그 자리다 ([ADR 0010](decisions/0010-플랫폼-이식-이음새.md) 이음새 2). 변이 시험과 크로스 모델 리뷰가 각각 따로 짚었다 | Phase 6. 대기 집합이 바뀌는 시점이다 |

## 문서 부채

| 항목 | 내용 |
|------|------|
| `experiments.md` 없음 | Phase 9 산출물. 게이트가 예외로 두는 유일한 끊긴 링크다 |

## 세션을 시작할 때

읽는 순서, 문서 역할, 재발 방지 규칙은 저장소 루트 `CLAUDE.md`에 있다. **여기에 옮겨 적지
않는다.**
