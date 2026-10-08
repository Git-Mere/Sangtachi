# nat-probe 에 다중 펀치(host, players)를 더했다

## 왜 했나

`plan.md` 다음에 할 일의 첫째인 Phase 4 착수 전 `tools/nat-probe` 재측정이다. `roadmap.md` 는 그때 한 호스트가
동시에 네 쌍과 펀치할 때의 거동을 같이 재라고 했는데, 기존 `punch` 는 1:1 이다. 저장소 소유자가 "친구 기기
하나에서 소켓 넷으로 플레이어 넷을 흉내 낸다" 와 "네 쌍 모두 `success` 인 회차가 3회 연속이면 통과" 를 골랐다.

## 무엇을 바꿨나

| 파일 | 무엇 |
|------|------|
| `tools/nat-probe/natprobe.py` | `run_links`(쌍 여러 개를 한 루프에서), `link_result`, `MappingKeeper`, `recheck_endpoint`, 하위 명령 `host`, `players`. 결과 JSON 에 `links`, `player_sockets`, `prepunch` |
| `tools/nat-probe/test_natprobe.py` | 쌍 판정 표, 쌍 수 상한, 루프백으로 호스트와 플레이어, 응답 없는 상대, 목록 밖 출발지, 쌍이 하나일 때, 위조 PONG 셋 |
| `tools/nat-probe/README.md` | `host`, `players` 의 절차, 준비 중 매핑 갱신과 펀치 직전 확인, 가르는 규칙, 흉내의 한계 |
| `tools/nat-probe/RECORD-TEMPLATE.md` | 4.5 다중 펀치 표 |
| `roadmap.md` Phase 4 | 재측정 항목에 도구와 통과 기준. 펀치 직전 확인을 통과한 회차만 센다 |

## 중요한 결정

- **`run_punch` 는 그대로 둔다.** 2자 실측 22건이 그 함수로 돌았고 시험이 덮는다. 새 루프는 따로 둔다. 두 루프의
  판정 규칙(`success`, `one-way`, `failure`)은 같다
- **PONG 은 쌍마다 다른 세션으로 가르고 출발지를 보지 않는다.** `run_punch` 와 같은 이유다. 상대 NAT 이 다른
  매핑을 쓰면 다른 포트에서 오는 것이 정상이다
- **PING 은 소켓의 쌍이 여럿일 때만 출발지로 가른다.** 맞는 쌍이 없으면 `unattributed_inbound` 에만 센다.
  쌍이 하나면 `run_punch` 처럼 거르지 않는다
- **엔드포인트는 실행 중에 묻는다.** 명령줄에 공인 주소를 남기지 않는다
- 흉내의 한계(원격 IP 가 하나)는 README 와 측정 기록에 적는다

## 검증

| 무엇 | 결과 |
|------|------|
| `python test_natprobe.py` | 194/194 |
| 변이 | 처음 아홉 가운데 둘이 살아남아(PONG 의 소켓 검사, 타임스탬프 검사) 위조 PONG 시험 둘을 더했다. 리뷰 수정 뒤 열넷(중복 PONG, PONG 출발지 거르기, 펀치 직전 비교, 갱신 멈춤, 첫 서버 고르기 포함)을 다시 돌려 모두 잡았다 |
| 연기 시험 | 이 기기에서 `host --peers 1`, `players --count 2` 를 응답 없는 루프백 상대로 1초 돌렸다. STUN, 엔드포인트 출력, 입력, 판정, 저장까지 돈다 |
| 문서 게이트 | `docgate.py` `VERDICT: pass` |

## 크로스 모델 리뷰

Codex. staged diff 에 렌즈 둘(코드, 측정 설계와 문서)을 따로 돌렸다.

| 렌즈 | 지적 | 처리 |
|------|------|------|
| 코드 | warn. 중복 PONG 막기를 지워도 시험이 통과한다 | 반영. 같은 PONG 을 두 번 보내는 시험 |
| 코드 | warn. PONG 을 출발지로 거르게 바꿔도 시험이 통과한다 | 반영. 다른 포트에서 온 PONG 이 `success` 이고 `source_matches_expected` 가 거짓인 시험 |
| 설계 | warn. 엔드포인트를 손으로 주고받는 동안 매핑이 만료되면 실패가 펀치가 아니라 낡은 엔드포인트 때문일 수 있다 | 반영. 준비 중 10초마다 STUN Binding Request, 펀치 직전 재확인. 다르면 그 회차는 판정에 쓰지 않는다 |
| 설계 | warn. `unattributed_inbound` 가 비어 있지 않다고 상대 NAT 의 다른 매핑이라 단정할 수 없다 | 반영. "출처를 모르는 PING" 으로만 적고 상대 쪽 결과와 맞춰 본다 |
| 설계 | warn. 기록 양식이 쌍 하나만 담는다 | 반영. `RECORD-TEMPLATE.md` 4.5 |

2차 리뷰(수정 확인)에서 warn 하나가 나왔다. `players` 의 갱신이 모든 소켓을 첫 소켓의 서버로 보냈다. 매핑이 목적지마다
다르면 알려 준 매핑이 살지 않는다. 반영했다. 소켓마다 자기 서버로 보낸다. 시험과 변이 하나를 더했다.

3차 리뷰에서 warn 하나가 나왔다. `players` 는 소켓마다 STUN 을 차례로 돌아, 뒤 소켓의 STUN 이 길어지면(서버 셋이 응답하지 않으면
최대 45초) 앞 소켓이 갱신 없이 기다린다. 반영했다. STUN 을 마친 소켓부터 바로 갱신에 더한다. 시험을 더했다.

4차 리뷰에서 warn 하나가 나왔다. 펀치 직전 확인이 함께 누르는 Enter 뒤라, 소켓이 많고 STUN 이 느리면 두 쪽의 펀치 시작이
어긋난다. 반영했다. 확인을 Enter 앞에서 하고 갱신은 Enter 까지 돈다.

5차 리뷰(그 수정 확인)는 `LGTM - no blockers` 였다.
