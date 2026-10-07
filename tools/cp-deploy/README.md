# cp-deploy

제어 서버를 EC2 한 대에 배포하고, 배포한 서버에 연산을 돌려 본다. 설계의 출처는
[`docs/kor/control_plane.md`](../../docs/kor/control_plane.md) 7.6 설정과 배포다.

공개 레포라 주소, 키 경로, 테이블 이름은 인자로만 받는다. 실제 값은 `deploy/aws.local.md`(ignore 됨)에
있다.

## deploy.py

```text
python tools/cp-deploy/deploy.py --host <Elastic IP> --key <키 파일> --table <테이블> --region <리전>
```

레포 루트에서 Git Bash 로 돌렸다. 마지막 줄이 `VERDICT: pass` 여야 한다.

| 단계 | 무엇 |
|------|------|
| 입력 검사 | `--host` 는 공인 유니캐스트 IPv4 리터럴만. 테이블, 리전, 사용자 이름은 형식 검사 |
| 커밋 확인 | `control-server/controlplane` 과 `requirements.txt` 에 커밋하지 않은 변경이 있으면 멈춘다. 올리는 것은 `git archive HEAD` 다 |
| 원격 | `python3-venv` 가 없으면 apt 로 설치. 시스템 사용자 `sangtachi-cp`. `/opt/sangtachi-cp/releases/<커밋>` 에 풀고 venv 에 `requirements.txt` 설치. `/opt/sangtachi-cp/current` 를 그 릴리스로 |
| 설정 | `/etc/sangtachi-cp.env` 에 `SANGTACHI_CP_TABLE`, `AWS_REGION`. 모드 0640 |
| 서비스 | `sangtachi-cp.service`. `Restart=always`, 서비스 사용자로 실행, `ProtectSystem=strict` 등 |
| 확인 | journal 에 `server.started` 가 30초 안에 나오는지. 이 기기에서 없는 방의 `get_peers` 를 보내 `room_not_found` 를 받는지 |

**인스턴스에 남는 것.** 위 사용자, 폴더, 설정 파일, 유닛이다. 릴리스 폴더는 지우지 않는다.

**원격 스크립트가 실패하면 되돌린다.** 원격 스크립트가 설정을 바꾸기 시작한 뒤 `server.started` 를
확인하기 전에 실패하거나 끊기면 앞 설정, 앞 유닛, 앞 릴리스로 되돌리고 서비스를 다시 띄운다. 그 뒤
이 기기에서 하는 연기 시험이 실패하면 되돌리지 않는다. 새 릴리스가 남고 `VERDICT` 가 나오지 않는다. 일부러 기동에 실패하는 릴리스를 올려 이 경로를 돌렸고 앞
릴리스로 돌아와 서버가 떴다. 처음 배포가 실패했을 때의 경로(서비스를 멈추고 지움)는 돌려 보지 않았다.

- 배포는 인스턴스 단위 잠금(`/run/lock/sangtachi-cp-deploy.lock`)으로 한 번에 하나다
- 되돌리기가 `/etc/sangtachi-cp.env.prev` 나 유닛의 `.prev` 를 제자리에 놓지 못하면 그 파일을 남기고,
  그것이 남아 있는 동안 다음 배포를 거부한다. 손으로 확인하고 지운 뒤 다시 한다. `current` 를 되돌리거나
  다시 띄우는 단계의 실패는 파일을 남기지 않고 메시지만 낸다

**자격 증명을 다루지 않는다.** 서버는 인스턴스의 IAM 역할을 쓴다.

돌리면서 고친 것이 셋이다. 지금 코드가 그 뒤의 것이다.

- 원격 스크립트를 텍스트 모드로 넘기면 Windows 가 줄 끝을 CRLF 로 바꿔 원격 bash 가 깨진다.
  바이트로 넘긴다
- `mktemp -d` 가 0700 으로 만든 릴리스 폴더에 서비스 사용자가 들어가지 못했다. 권한을 맞춘다
- 실패한 릴리스가 `Restart=always` 로 거듭 재시작하면 systemd 의 시작 횟수 제한에 걸려, 되돌린 뒤의
  restart 가 "Start request repeated too quickly" 로 거부됐다. 그동안 서버가 약 2분 내려가 있었다.
  restart 앞에 `systemctl reset-failed` 를 둔다

## verify.py

```text
python tools/cp-deploy/verify.py --host <Elastic IP> [--players 4] [--concurrent] [--client-retry] [--rounds N] [--soak SECONDS]
```

방 하나를 만들고 플레이어를 넣고, 후보를 등록하고, 호스트가 확인하고, 플레이어를 회수한다.
연산마다 이 기기에서 잰 왕복 시간을 낸다. 실제 테이블에 항목이 생기고 방이 만료되면 TTL 이 지운다.
방 코드와 토큰은 내지 않는다.

- `--concurrent` 는 플레이어가 동시에 참가한다. 실제 테이블에서 트랜잭션 충돌이 난다
  (`control_plane.md` 7.6)
- `--client-retry` 는 `control_plane.md` 8.3 의 클라이언트 재시도를 흉내 낸다
- `--soak SECONDS` 는 용량 확인용 부하다. 방 하나에 플레이어 넷이 0.5초마다 `get_peers` 를 부르고
  호스트가 5초마다 `host_report` 를 부르는 상태를 그 시간 동안 유지한다. 끝에 그 구간을 UTC 로 낸다.
  CloudWatch 의 테이블 지표를 그 구간으로 본다(`roadmap.md` Phase 3 검증의 배포 묶음)
- 왕복 시간은 이 기기에서 리전까지의 네트워크를 포함한다. 서버 쪽 시간은 journal 의
  `http.request` 줄의 `ms` 다

## 시험

```text
python -m pytest -q -p no:cacheprovider tools/cp-deploy
```

입력 검사의 케이스 표다. 원격 동작은 시험하지 않는다.
