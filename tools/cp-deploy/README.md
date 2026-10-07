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

**인스턴스에 남는 것.** 위 사용자, 폴더, 설정 파일, 유닛이다. 릴리스 폴더는 지우지 않는다. 앞
릴리스로 되돌리려면 `current` 링크를 그 폴더로 바꾸고 서비스를 다시 띄운다. 이 되돌리기는 아직
돌려 보지 않았다.

**자격 증명을 다루지 않는다.** 서버는 인스턴스의 IAM 역할을 쓴다.

처음 돌렸을 때 두 번 실패했다. 고친 뒤의 코드가 지금 것이다.

- 원격 스크립트를 텍스트 모드로 넘기면 Windows 가 줄 끝을 CRLF 로 바꿔 원격 bash 가 깨진다.
  바이트로 넘긴다
- `mktemp -d` 가 0700 으로 만든 릴리스 폴더에 서비스 사용자가 들어가지 못했다. 권한을 맞춘다

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
