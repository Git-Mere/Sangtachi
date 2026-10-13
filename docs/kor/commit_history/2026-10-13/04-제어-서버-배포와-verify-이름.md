# 제어 서버 배포와 `verify.py` 의 이름

## 왜 했나

1차 D 의 표시 이름, 후보 위생 대역, 헤더 이름 공백이 main 에 있었지만 배포하지 않았다. 4차 K 까지 끝난 뒤 저장소 소유자가
배포를 지시했다. 배포 뒤 확인 도구 `tools/cp-deploy/verify.py` 가 이름 없는 `create_room` 을 보내 `bad_request` 로 떨어졌다.
서버가 1차 D 부터 이름을 요구하는데(`control_plane.md` 2.7, 4.2, 4.3) 도구를 그때 고치지 않았다.

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| `create_room` 과 `join_room` 이 표시 이름을 싣는다. 실행마다 다른 16진수 꼬리표에 순번을 붙여 방 안에서 겹치지 않는다 | `tools/cp-deploy/verify.py` |
| 배포 행을 지웠다 | `plan.md` |

## 배포

| 무엇 | 결과 |
|------|------|
| SSH 호스트 키 | 학교 Wi-Fi 에서 불일치로 거부됐던 것이다. 이 기기의 집 망에서 `ssh-keyscan` 의 ED25519 지문이 `known_hosts` 의 것과 같았다. `known_hosts` 는 고치지 않았다 |
| `tools/cp-deploy/deploy.py` | 커밋 `fe47fdc` 의 `controlplane/` 를 올렸다. `server.started` 를 확인했고 없는 방의 `get_peers` 가 `room_not_found` 였다. `VERDICT: pass` |
| `verify.py --players 4` (고친 뒤) | 방 하나, 참가 넷, 가상 IP 겹침 없음, 확인 넷, 회수 넷. `VERDICT: pass`. `--concurrent --client-retry` 도 `VERDICT: pass` |
| 실제 클라이언트 둘 | 이 기기에서 `host` 와 `player` 를 배포 서버와 기본 STUN 목록으로 띄웠다. 둘 다 `CONNECTED` 에 닿았고, 학습한 주소는 로컬 후보(`10.0.0.49`)였다. 플레이어가 두 이름이 든 `ROSTER` 를 받았다. 두 기록 파일에 `result=OK` 와 `result=CLOSED` 가 같은 시도 번호로 남았고 종료 줄의 RTT 는 약 0.6ms 였다 |

**배포 전에 만든 방이 남아 있었는지는 확인하지 않았다.** `control_plane.md` 6.3 의 형 규칙대로 이름이 없는 `PEER#` 가 든 방은
요청이 `internal` 이 된다. `roadmap.md` Phase 4 의 표시 이름 항목대로 배포는 그 전의 방을 모두 끝내는 것으로 본다.

## 검증

| 무엇 | 결과 |
|------|------|
| `python -m pytest -q -p no:cacheprovider tools/cp-deploy` | 25 passed (control-server 의 venv) |
| `docgate.py` | `VERDICT: pass` |

## 크로스 모델 리뷰

Codex 에 `verify.py` 의 정확성과 기록의 일관성으로 돌렸다.

| 지적 | 처리 |
|------|------|
| warn. 무작위 32비트 이름은 방 안 유일성을 보장하지 않는다. 겹치면 `name_taken` 으로 확인이 떨어진다 | 반영. 실행마다 꼬리표 하나에 순번을 붙인다. 재시도는 같은 본문이라 이름도 같다 |
