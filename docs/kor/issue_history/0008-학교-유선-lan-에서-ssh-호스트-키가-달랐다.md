# 0008. 학교 유선 LAN 에서 인스턴스로 가는 SSH 의 호스트 키가 달랐다

- 상태: 우회
- 발견: 2026-11-18
- 관련: [`../plan.md`](../plan.md) 시험 기기 절, [`../commit_history/2026-11-18/04-phase-9-오늘-세-묶음-분석.md`](../commit_history/2026-11-18/04-phase-9-오늘-세-묶음-분석.md)

## 증상

학교에서 이 PC 로 제어 평면 인스턴스에 SSH 로 들어가려 했다. 오늘 묶음의 CloudWatch 1분 해상도 값을 받기 위해서였다. Git Bash 의 `ssh` 와 Windows OpenSSH 가 둘 다 `REMOTE HOST IDENTIFICATION HAS CHANGED` 로 거부했다. 서버가 보여 준 ED25519 키가 `known_hosts` 에 적힌 키와 달랐다.

## 조사

| 의심한 것 | 본 것 | 판단 |
|-----------|-------|------|
| 인스턴스의 호스트 키가 바뀌었다 | 집 Wi-Fi 의 `mere-galaxy` 에서 `ssh-keyscan` 으로 본 ED25519 키가 `known_hosts` 의 키와 같았다 | 아니다 |
| SSH 클라이언트마다 `known_hosts` 가 다르다 | 두 클라이언트가 같은 `known_hosts` 의 같은 항목을 들어 거부했다 | 아니다 |
| 이 PC 에서 나가는 경로에 따라 받는 키가 다르다 | 학교 유선 LAN 주소로 바인드한 `ssh` 는 다른 키를 받았다. `mere-galaxy` 를 거친 `ProxyJump` 는 `StrictHostKeyChecking=yes` 로 저장된 키를 통과했다 | 맞다 |
| 휴대폰 핫스팟 경로 | 그때 이 PC 의 Wi-Fi 가 끊겨 있어 보지 못했다 | 모른다 |

## 원인

**확인한 것.** 받은 호스트 키가 경로에 따라 다르다. 이 PC 의 학교 유선 LAN 경로로 22번 포트에 닿으면 `known_hosts` 와 다른 키가 오고, 집 망에서는 `known_hosts` 와 같은 키가 온다.

**추정한 것.** 학교 유선 LAN 경로에서는 인스턴스가 아닌 쪽이 답하는 것으로 본다. 학교 망의 장비가 SSH 를 가로채 검사하는 것으로 본다. 장비나 정책은 확인하지 않았다. UDP 터널과 제어 평면의 TCP 8000 이 같은 처리를 받는지는 보지 않았다. 오늘 학교 유선 LAN 의 시행은 모두 연결됐다.

## 처리

학교 유선 LAN 에서는 인스턴스에 바로 SSH 하지 않는다. `mere-galaxy` 를 `ProxyJump` 로 거친다. 그러면 인스턴스로 가는 TCP 연결은 집 망에서 나가고, 호스트 키 확인과 키 인증은 이 PC 와 인스턴스 사이에서 끝까지 한다. `known_hosts` 는 고치지 않았다.

## 결과

`ProxyJump` 로 저장된 키가 통과했고 CloudWatch 값을 받았다. 학교 유선 LAN 에서 인스턴스로의 직접 SSH 는 여전히 다른 키를 받는다.
