# Phase 4 1차 C: 로컬 후보와 위생

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| `peer/candidates`: 10.1 로컬 후보 판정(활성 = `OperStatus` Up 이고 `DadState` Preferred, 제외 목록), 자기 서브넷 집합, 기본 경로, 등록 목록(반사, 기본 경로 인터페이스, 나머지, 중복 제거, 8개), 수집 실패 처리와 경고 | `client/include/sangtachi/peer/candidates.hpp`, `client/src/peer/candidates.cpp` |
| `platform/interfaces`: `GetAdaptersAddresses`, `GetIpForwardTable2` 와 `GetIpInterfaceEntry`. `iphlpapi` 링크 | `client/include/sangtachi/platform/interfaces.hpp`, `client/src/platform/win32/interfaces.cpp`, `client/CMakeLists.txt` |
| 받은 후보 위생에 `0.0.0.0/8`, `240.0.0.0/4`, 자기 서브넷 행. 위생과 해석 함수가 자기 서브넷 집합을 필수 인자로 받는다 | `client/src/control/ops.cpp`, `ops.hpp`, `runner.*`, `main.cpp` |
| 7장 출발지 위생의 자기 서브넷 directed broadcast 행. `classify_datagram` 이 집합을 받고 `EventLoop` 가 집합을 소유한다 | `client/src/loop.cpp`, `loop.hpp` |
| HTTP 응답에서 이름 안에 공백이나 탭이 있는 헤더 줄 거부 (`control_plane.md` 3.5) | `client/src/control/http.cpp` |
| 시험 | `tests/peer/candidates_test.cpp`, `tests/platform/interfaces_test.cpp`, `tests/control/{ops,runner,http}_test.cpp`, `tests/loop_test.cpp` |
| 모듈 표, 10장 트리, 11장 IP Helper 행 | `architecture.md` |
| 받은 후보 위생 행을 서버 몫만 남기고, 로컬 후보 수집 연결 행을 더함 | `plan.md` |

## 중요한 결정

- **로컬 후보를 실제로 모아 등록하는 연결은 4차 J 로 미뤘다.** `Lobby::on_stun_done` 은 아직 반사 후보만 등록한다.
  **그때까지 제품 경로의 자기 서브넷 집합은 비어 있어 7장 directed broadcast 행과 받은 후보 위생의 자기 서브넷 행이 아무것도 막지
  않는다.** 남은 일을 `plan.md` 에 적었다. 자기 Wintun 식별자는 Phase 6 까지 늘 비어 있다
- **문서가 정하지 않은 것을 코드가 정했다.** 문서 규칙과 어긋나지 않는다
  - 경로 조회 API 는 `GetIpForwardTable2` 와 `GetIpInterfaceEntry` 다. 10.1 은 규칙(`0.0.0.0/0`, 두 메트릭의 합, 동점은 열거 순서)만 정한다
  - 한 어댑터의 GUID 변환이 실패하면 열거 전체를 실패로, 한 기본 경로 줄의 인터페이스 조회가 실패하면 경로 조회 실패로 다룬다.
    둘 다 10.1 이 정한 결과(경고, 다시 시도하지 않음)로 떨어진다. GUID 를 모른 채 남의 어댑터로 짐작하면 자기 Wintun 주소가 후보로
    나갈 수 있다
  - 경고 이벤트 이름 `candidates.enumerate_failed`, `candidates.route_failed`. `architecture.md` 9장이 이벤트를 늘리는 것을 허용한다
- **`control_peers_fields` 의 재위생은 빈 집합으로 돈다.** 로그 줄 함수라 집합이 없고, 위생은 거르기만 하므로 해석 결과에는 영향이 없다

## 검증

| 무엇 | 결과 |
|------|------|
| 전체 시험 (`scripts/test.ps1`, main 에 합친 뒤) | 단위 383건 통과, cli/e2e/lobby 검사 실패 0, `all tests passed` |
| 변이 (하위 에이전트) | 28개 모두 시험이 잡았다. `roadmap.md` Phase 4 가 정한 다섯(M1~M5)과 7장 (a) 층의 규칙별 변이(M6~M11)가 들어 있다. 둘(M14, M18)은 처음 형태가 미사용 인자 경고로 빌드에서 막혀 경고를 피하는 형태로 다시 돌렸다 |
| `docgate.py`, `platformgate.py` | 둘 다 `VERDICT: pass` |

## 크로스 모델 리뷰

하위 에이전트가 worktree 에서 Codex 리뷰를 두 번 돌렸다.

| 지적 | 처리 |
|------|------|
| warn. `IfType` 24 행이 루프백 대역 규칙에 가려 `IfType` 24 규칙을 지워도 통과한다 | 반영. 대역 밖 주소의 `IfType` 24 케이스를 더했다. 변이 M27 로 확인했다 |
| warn. 등록 목록의 중복 시험이 IP 만 비교하는 구현을 못 잡는다 | 반영. 같은 IP 다른 포트 케이스를 더했다. 변이 M28 로 확인했다 |

2차는 `LGTM - no blockers` 였다.

main 에 합친 뒤 staged diff 에 Codex 리뷰를 한 번 더 돌렸다.

| 지적 | 처리 |
|------|------|
| warn. 자기 서브넷이 10.1.2.3/1 처럼 넓으면 브로드캐스트가 127.255.255.255 가 되어, 7장이 통과로 정한 루프백이 `BadSource` 가 된다 | 반영. 루프백을 자기 서브넷 행보다 먼저 통과시킨다. 케이스를 먼저 써서 실패를 확인했다 |
| warn. `drain_udp` 가 루프의 집합 대신 빈 집합을 넘겨도 시험이 통과한다 | 미룸. 보통 소켓으로는 directed broadcast 출발지를 만들 수 없어 수신 경로 주입이 필요하다. 그 진입점은 2차 E 가 만든다. 7장 (b) 층 시험에 자기 서브넷 행을 넣어 이 변이를 잡도록 `plan.md` 에 적었다 |

2차 리뷰(그 수정 확인)는 `LGTM - no blockers` 였다.
