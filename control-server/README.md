# control-server

Sangtachi 제어 서버. 설계의 출처는 [`docs/kor/control_plane.md`](../docs/kor/control_plane.md) 다.
이 README 는 시험을 돌리는 법만 적는다.

## 지금 있는 것

`controlplane/` 에는 케이스 표가 판정하는 순수한 부분만 있다. 수락 루프, `ops.dispatch`,
`store.py` 는 아직 없다. 그래서 저장소가 있어야 도는 표의 행은 `needs="store"` 로 표시되어 있고
건너뛴다.

| 파일 | 무엇 |
|------|------|
| `controlplane/` | 모듈. 책임은 `control_plane.md` 7.1 모듈 |
| `tests/test_*.py` | 케이스 표와 그 시험. 표는 파일 머리의 목록이다 |
| `tests/mutants/<표>.py` | 그 표의 변이 목록 |
| `tests/mutate.py` | 변이 실행기. 판정 규칙은 그 파일 첫머리에 있다 |
| `requirements.txt` | 제품 의존성. 배포하는 프로세스가 쓰는 것만 |
| `requirements-dev.txt` | 시험 전용 의존성 ([ADR 0014](../docs/kor/decisions/0014-제어-서버-시험-pytest와-docker.md)) |

## 시험

Windows 의 개발 기기에서 돌린 명령이다. `control-server/` 에서 돌린다.

```text
py -3.14 -m venv .venv
.venv/Scripts/python -m pip install -r requirements-dev.txt
.venv/Scripts/python -m pytest -q
.venv/Scripts/python tests/mutate.py
```

- `pytest` 는 건너뛴 행의 수를 함께 낸다. 그 수가 `needs="store"` 행의 수와 다르면 무언가 다른
  이유로 건너뛴 것이다
- `mutate.py` 의 마지막 줄이 `VERDICT: pass` 여야 한다. 표 하나만 돌리려면 이름을 준다
  (`tests/mutate.py room_id`). 이름은 `tests/mutants/` 의 파일 이름이다
- 변이마다 pytest 를 새 프로세스로 띄운다. 변이 하나에 상한 120초가 걸려 있다

## 시험이 잘못된 코드를 읽지 않게 하는 장치

`mutate.py` 는 `controlplane/` 을 임시 폴더에 복사해 고친 뒤 `CP_SRC` 로 그 폴더를 넘긴다.
`tests/conftest.py` 는 실제로 import 된 `controlplane` 이 `CP_SRC` 아래인지 확인하고, 아니면 시험
전체를 멈춘다. 원본이 대신 읽히면 모든 변이가 살아남은 것처럼 보이기 때문이다.

변이를 넣기 전에 원본으로 같은 시험을 한 번 돌린다. 원본이 떨어지면 판정하지 않고 멈춘다.

## DynamoDB local

Docker 컨테이너로 띄운다([ADR 0014](../docs/kor/decisions/0014-제어-서버-시험-pytest와-docker.md)). 아래는
Docker Desktop 29.8 에서 실제로 돌린 명령이다.

```text
docker run -d --rm --name sangtachi-ddb -p 127.0.0.1:8001:8000 amazon/dynamodb-local:3.3.1 -jar DynamoDBLocal.jar -inMemory -sharedDb
docker stop sangtachi-ddb
```

- 이미지 태그는 `3.3.1` 로 고정한다. 받은 이미지의 다이제스트는
  `sha256:ff89bd48ff32cd8d9be5fee8873b65b8854dc408f1afe881be6eb00247bc0dab` 였다
- 호스트 쪽 포트는 `127.0.0.1:8001` 이다. 8000 은 제어 서버의 포트(`CONTROL_PORT`)라 피했고,
  루프백에만 묶어 같은 망의 다른 기기가 닿지 않게 한다
- `-inMemory` 라 컨테이너를 멈추면 테이블이 사라진다. `--rm` 이라 컨테이너도 사라진다
- `-sharedDb` 는 자격 증명과 리전이 달라도 같은 데이터베이스를 쓰게 한다
- `boto3` 는 자격 증명이 없으면 요청을 보내지 않는다. 실제 키가 아닌 값을 넣는 이유는
  `control_plane.md` 7.6 설정과 배포에 있다

저장소 시험을 돌리는 방법은 그 시험이 생길 때 여기에 넣는다.
