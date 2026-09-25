# 2026-09-20 natprobe Windows 실행 결함 수정

## 변경

[`2026-09-18/01-nat-probe.md`](../2026-09-18/01-nat-probe.md)에서 만든 `tools/nat-probe/natprobe.py`가
**Windows에서 시작하지 못하는 결함 2건**을 고쳤다. 실제 Windows 실행에서 드러났다.

- `SIO_UDP_CONNRESET`을 `ws2_32.dll`의 `WSAIoctl`로 직접 호출
- 시작할 때 콘솔 인코딩을 UTF-8로 맞추는 `setup_console()` 신설
- Python 3.8 미만이면 ASCII 안내를 내고 종료
- JSON에 `socket.udp_connreset_disabled`, `socket.udp_connreset_detail` 추가
- `README.md` 5.3 "Windows에서 막힐 때" 신설. 증상별 대처표
- 자체 검증 38건 -> 41건

## 문제

**1. `ValueError: invalid ioctl command 2550136844`**

`2550136844`은 `0x9800000C`, 즉 `SIO_UDP_CONNRESET`이다. **Python의 `socket.ioctl()`은 이
명령을 지원하지 않는다.** CPython은 `SIO_RCVALL`, `SIO_KEEPALIVE_VALS`,
`SIO_LOOPBACK_FAST_PATH` 세 개만 허용하고 나머지는 `ValueError`로 막는다.
`socket.SIO_UDP_CONNRESET` 상수 자체가 없어서 `getattr` 폴백이 raw 값을 넘겼고, 그것이
거부됐다.

`protocol.md` 15장이 요구하는 항목이라 건너뛸 수 없다. 끄지 않으면 ICMP port unreachable을
받은 뒤 `recvfrom`이 `WSAECONNRESET`로 깨진다. **펀치 초반에는 상대가 아직 포트를 열기
전이라 이 ICMP가 정상적으로 발생한다.** 가장 중요한 구간에서 수신이 흔들린다.

**2. `UnicodeEncodeError: 'charmap' codec`**

이 도구의 출력은 한국어다. Windows 콘솔 코드페이지가 cp1252(영문 기본)면 첫 `print`에서
죽는다. 출력을 파일로 넘길 때도 같다. 한글 Windows(cp949)에서는 드러나지 않는다.

## 결정

**`ctypes`로 `WSAIoctl`을 직접 부른다.** Python이 막아 둔 경로라 우회가 필요하다. 인자 형은
명시적으로 지정한다. `SOCKET`은 64비트 Windows에서 8바이트이므로 `c_void_p`로 넘긴다.

**실패해도 측정을 멈추지 않는다.** 대신 성공 여부를 JSON에 남긴다. 수신 경로는 이미
`OSError`를 잡고 넘어가므로 도구가 죽지는 않는다. 다만 결과 해석에 영향이 있을 수 있어
`socket.udp_connreset_disabled`로 기록하고 화면에도 찍는다. **기록되지 않는 실패가 가장 나쁘다.**

**인코딩은 `errors="replace"`를 마지막 방어선으로 둔다.** 글자가 깨질지언정 30초짜리 측정이
중간에 죽지 않게 한다. JSON은 항상 UTF-8로 정확히 저장된다.

## 검증

- `test_natprobe.py` 41/41 통과. `SIO_UDP_CONNRESET` 상수값, 소켓 생성 후 상태 기록,
  bind와 논블로킹 확인 3건 추가
- `PYTHONIOENCODING=cp1252`로 수정 전 판을 돌려 `UnicodeEncodeError` 재현, 수정 후 정상 완료
- Linux에서 `probe` 정상 동작, `udp_connreset_disabled`가 `null`로 기록됨
- **Windows 실행 확인은 다음 측정에서 한다.** 이 수정은 Windows에서 재현 확인되지 않았다

## 크로스 모델 리뷰

- 리뷰어: Codex (GPT). `WSAIoctl` 경로는 Linux에서 실행 검증이 불가능해 정적 검토에 의존한다
- 결과: warn 2, **전부 반영. 기각 없음**

| 지적 | 판단 |
|------|------|
| `ctypes.get_last_error()`가 Winsock 오류를 제대로 못 가져와 실패 코드가 0이나 엉뚱한 값으로 기록될 수 있다 | 반영. `use_last_error=True`를 버리고 `WSAGetLastError`를 직접 부른다. 그 옵션을 켜면 ctypes가 호출 전후로 스레드 last-error를 자기 값으로 바꿔치기해 뒤이은 조회가 오염된다. `GetProcAddress`가 last-error를 덮을 수 있으므로 함수는 호출 전에 미리 찾아 둔다 |
| 비Windows에서 `udp_connreset_detail`에 설명 문자열이 들어가, "detail은 `disabled`가 `false`일 때만"이라는 계약과 어긋난다 | 반영. `None`으로 비운다. `disabled: null`이 이미 "해당 없음"을 뜻한다 |

## 남은 것

Windows 실측이 여전히 미착수다. `2026-09-18/01-nat-probe.md`의 "남은 것"이 그대로 유효하다.
실측 JSON과 기록 2건도 여전히 커밋하지 않았다. 레포가 public이고 두 가정의 공인 IP가 들어 있다.
