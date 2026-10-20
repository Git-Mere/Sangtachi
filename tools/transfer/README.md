# transfer

가상 IP 사이의 파일 전송 확인이다. [`roadmap.md`](../../docs/kor/roadmap.md) Phase 7 검증의 NFR-2 판정 항목이 쓴다.

## Test-Transfer.ps1

한쪽이 TCP 25565 에서 기다리고 다른 쪽이 파일을 보낸다. 받는 쪽이 받은 바이트의 SHA-256 을 돌려주고, 보낸 쪽이
자기 해시와 비교해 판정한다. 25565 인 이유는 클라이언트가 만드는 방화벽 규칙이 그 포트만 열기 때문이다
([`windows-prereq.md`](../../docs/kor/windows-prereq.md) 2절). 같은 기기의 Minecraft 서버가 25565 를 쓰고 있으면
먼저 끈다.

```powershell
# 받는 쪽
powershell -NoProfile -ExecutionPolicy Bypass -File tools\transfer\Test-Transfer.ps1 -Listen
# 보내는 쪽. 64 MiB 무작위 파일을 만들어 보낸다. 있는 파일은 -Path 로 준다
powershell -NoProfile -ExecutionPolicy Bypass -File tools\transfer\Test-Transfer.ps1 -Connect 10.100.0.1 -GenerateMiB 64
```

| 쪽 | 마지막 줄 | 종료 코드 |
|----|-----------|-----------|
| 보내는 쪽 | `VERDICT: pass` / `fail` / `unknown` | 0 / 1 / 2 |
| 받는 쪽 | `received ... sha256=<해시>` | 0 |

`unknown` 은 연결이나 전송이 실패한 것이다. 통과가 아니다. Windows PowerShell 5.1 에서 돈다.

같은 기기의 루프백(`-BindAddress 127.0.0.1`, `-Connect 127.0.0.1`)으로 통과와, 받는 쪽이 틀린 해시를 돌려줄 때
`fail` 이 나는 것을 확인했다.
