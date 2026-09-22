# nat-probe

NAT 매핑 거동과 UDP 홀펀칭 실측 도구. 표준 라이브러리만 쓴다. Python 3.8+.

대상은 Windows 10/11 x64지만 Linux 와 macOS 에서도 돈다.

## 무엇을 재고 무엇을 재지 않는가

| 하위 명령 | 재는 것 | 인원 |
|-----------|---------|------|
| `probe` | STUN Binding 으로 **매핑 거동**만 | 혼자 |
| `punch` | `probe` 를 먼저 하고 **같은 소켓으로** 실제 홀펀칭 | 양쪽 망에서 동시에 |

**`probe` 만으로 판단하지 않는다.** Binding 응답은 매핑 거동만 알려주고 **필터링 거동은
알려주지 않는다.** 홀펀칭 성립은 둘 다에 달려 있다. 엔드포인트 독립인데 필터링 때문에 안
되는 경우를 그대로 놓친다.

## 실행

```
python natprobe.py probe [--label NAME] [--port N] [--stun HOST:PORT ...] [--out DIR]
python natprobe.py punch [--label NAME] [--port N] [--stun HOST:PORT ...] [--out DIR]
                         [--peer IP:PORT] [--duration SEC]
                         [--unsolicited] [--unsolicited-duration SEC]
python natprobe.py check-peer <IP> [--ipv4]
python natprobe.py check-peer --stdin [--ipv4]
```

| 옵션 | 기본값 | 뜻 |
|------|--------|-----|
| `--label` | 호스트명 | 측정 지점 이름. 결과 파일 이름에 들어간다 |
| `--port` | `0` | 로컬 UDP 포트. 재현하려면 고정한다 |
| `--stun` | 내장 목록 | 질의할 STUN 서버. 목록은 `natprobe.py` 상수에 있다 |
| `--peer` | 없음 | 상대 공인 엔드포인트. 아래 경고를 먼저 읽는다 |
| `--duration` | `30` | 펀치 시도 시간(초) |
| `--unsolicited` | 꺼짐 | 펀치 성립 후 요청하지 않은 인바운드가 통과하는지 시험. **양쪽이 함께 준다** |
| `--unsolicited-duration` | `10` | 위 시험 시간(초) |
| `--out` | `results/` | 결과 JSON 디렉터리 |

`--stun` 과 `--peer` 는 `punch` 에서 **같은 소켓을 공유한다.** 소켓을 따로 열면 출발지 포트가
달라져 매핑도 달라지고, 목적지 의존성과 구분할 수 없다 (`spec.md` NFR-9와 같은 이유).

**`punch` 는 양쪽이 시각을 맞춰 시작해야 한다.** `--peer` 를 **생략하면** 도구가 상대
엔드포인트를 묻고 그다음 Enter 대기에서 멈춘다. 양쪽이 "셋, 둘, 하나" 를 세고 함께 누르면
된다. `--peer` 를 **주면 질문과 Enter 대기를 둘 다 건너뛰고 바로 시작한다.** 그러면 시작
시각을 다른 방법으로 맞춰야 하고, 어긋나면 먼저 시작한 쪽 패킷이 상대 NAT 에 구멍이 뚫리기
전에 도착해 버려진다. **원인이 NAT 이 아니라 타이밍인 `failure` 가 나온다.**

**공용 기기에서는 `--peer` 를 쓰지 않는다.** 상대 공인 엔드포인트가 셸 기록과 프로세스
목록에 남는다. 대화식 입력을 쓴다.

`check-peer` 는 주소가 도달 가능한 유니캐스트인지만 본다. 소켓을 열지 않고 **종료 코드로만
답한다**(통과 0, 거부 2). **주소는 성공·실패 어느 쪽에도 출력하지 않는다.** 터미널 기록에
남기지 않기 위해서다. `--stdin` 은 명령줄에 주소를 남기지 않을 때 쓴다.

## 측정 전에 확인할 것

**Windows 방화벽 프롬프트를 허용한다.** 첫 실행 때 뜬다. 거부하거나 창을 닫으면 인바운드가
전부 막혀서 **실제 NAT 거동과 무관하게** `punch.result` 가 `failure` 로 나온다. 프롬프트가
뜨지 않았는데 `failure` 면 이전에 거부해 둔 규칙이 남아 있는지 본다. 이 도구는 관리자 권한을
요구하지 않는다.

**두 피어가 서로 다른 가정망에 있어야 한다.**

| 구성 | 유효한가 |
|------|----------|
| 집 A ↔ 집 B, 공인 IP 가 다름 | **유효.** ISP 가 같아도 된다 |
| 같은 공유기 아래 PC 2대 | **무효.** NAT 을 통과하지 않는다 |
| 같은 건물의 다른 세대 | 공인 IP 가 다른지 확인한다. 같으면 같은 NAT 뒤다 |

양쪽 `stun.servers[].mapped.ip` 가 다르면 별개 회선이다. ISP 가 같으면 결과를 다른 ISP 로
일반화하지 말고 그 한계를 기록에 적는다.

**`socket.behind_nat` 를 반드시 기록에 옮긴다. 한쪽이라도 `false` 면 그 측정은 NAT 두 개를
시험한 것이 아니다.** 실제로 한 측정에서 한쪽의 로컬 주소가 곧 공인 주소였다. 사후에
알아챘기에 망정이지 몰랐다면 NAT ↔ NAT 으로 잘못 보고했을 것이다.

**경로 오염을 먼저 없앤다.** VPN, 회사망, 기숙사망, 캠퍼스망이 켜져 있으면 결과가 오염된다.
끄고 가상 어댑터가 남았는지도 본다. 끌 수 없으면 그 사실을 기록에 남기고 **그 결과를 가정망
측정값으로 쓰지 않는다.** 모바일 핫스팟은 best-effort 토폴로지라 필수 판정에 쓰지 않는다.
측정 중 다른 대용량 트래픽을 돌리지 않는다. RTT 와 손실률이 왜곡된다.

## 결과 읽는 법

`stun.mapping`

| 값 | 뜻 |
|----|-----|
| `endpoint-independent` | 응답한 모든 서버가 같은 공인 엔드포인트를 봤다. `spec.md` 의 지원 조건 |
| `destination-dependent` | 서버마다 다른 엔드포인트. 대칭형 거동. 미지원 조건 |
| `unknown` | **판정 불가.** 측정 실패이지 NAT 거동이 아니다 |

`mapping_confident` 가 `false` 인 결과는 판정에 쓰지 않는다. **서로 다른 IP 를 가진 서버
2곳 이상**에서 응답을 받아야 목적지 의존성을 볼 수 있다. 서버를 늘려 다시 잰다.

`punch.result`

| 값 | 뜻 |
|----|-----|
| `success` | 상대 패킷을 받았고 내 패킷에 대한 응답도 받았다. 양방향 성립 |
| `one-way` | 한쪽만 성립. 필터링 또는 방화벽 의심 |
| `failure` | 아무것도 못 받았다. 방화벽, 시작 시각, 엔드포인트 오기부터 배제한다 |

`source_matches_expected` 가 `false` 면 받은 패킷의 출발지가 기대와 다르다. 실제 관측값이
`inbound_sources` 에 있으니 그 값을 기록에 남긴다.

**한 번의 성공은 판정 근거가 아니다.** 재현 조건은 3회 연속 성공이다.

## 기록

| 무엇 | 어디 |
|------|------|
| 결과 JSON | `results/`. **`.gitignore` 로 막혀 있다** |
| 측정 기록 | `records/`. **`.gitignore` 로 막혀 있다.** 양식은 `RECORD-TEMPLATE.md` |
| 판정 근거 | `docs/kor/audit-history/design-audit.md` 후속 5번, [ADR 0001](../../docs/kor/decisions/0001-직접-연결-대비책-미도입.md) |

**공인 IP 가 들어가는 원본은 커밋하지 않는다.** 두 디렉터리 모두 `.gitignore` 에 있고 `.gitkeep` 만 추적된다. 공개 레포이고 두 가정의 공인 IP 와 분 단위 시각이 들어 있다. 로컬에 보관하고 보고서 제출 때만 첨부한다. **커밋해야 한다면 가린 사본만 올린다.**

## 나쁜 결과도 실측 성공이다

`destination-dependent` 가 나오거나 펀치가 실패해도 측정은 성공한 것이다. 숨기지 않고 기록한다.

다시 재야 하는 것은 **판정 근거가 되지 못하는 결과**다. `unknown`, 방화벽 프롬프트를 허용하지
않은 시행, 같은 공유기 아래에서 돌린 시행, 두 쪽의 시작 시각이 어긋난 시행이다. 이것들은 NAT
거동이 아니라 측정이 틀린 것이다.

## 그 밖

`unsolicited-firewall-test.ps1` 은 요청하지 않은 인바운드가 막히는 원인이 NAT 인지 호스트
방화벽인지 좁히는 보조 스크립트다. 선택 사항이고 **관리자 권한과 상대의 동시 실행이 필요하다.**

> ⚠️ **비정상 종료하면 인바운드 허용 규칙이 영구히 남는다.** `New-NetFirewallRule` 은 기본으로
> `PersistentStore` 에 만든다. 강제 종료하거나 창을 닫으면 정리가 돌지 않아 **포트가 열린 채로
> 남는다.** 그런 일이 있었으면 직접 확인한다.
>
> ```powershell
> Get-NetFirewallRule -Group 'natprobe-unsolicited-test' | Remove-NetFirewallRule
> Get-NetFirewallRule -PolicyStore ActiveStore -Group 'natprobe-unsolicited-test'
> ```
>
> 두 번째 줄이 아무것도 출력하지 않아야 정리된 것이다. 신뢰할 수 있는 망에서만 돌린다.

자세한 CLI 동작과 JSON 스키마는 `natprobe.py` 의 주석과 `--help` 가 출처다. 여기에 옮겨
적지 않는다.
