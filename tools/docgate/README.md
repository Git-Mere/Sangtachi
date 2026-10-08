# docgate

문서 게이트. 크로스 모델 리뷰에 보내기 전에 기계가 먼저 거른다.

```bash
python tools/docgate/docgate.py            # 마지막 줄이 VERDICT: pass 여야 한다
python tools/docgate/docgate.py --claims   # 강한 주장 문구 목록. 종료 코드는 안 바뀐다
python tools/docgate/docgate.py --root .   # 루트 지정
```

종료 코드는 통과 0, 실패 1, 루트를 찾지 못하면 2다.

## 검사 다섯

| 검사 | 판정 | 내용 |
|------|------|------|
| `mirror` | 차단 | `docs/kor` 와 `docs/eng` 의 문서 짝이 맞는가 |
| `parity` | 차단 | 짝의 헤딩 레벨 순서, 표 행 수, 코드 블록 수, 링크 수가 같은가 |
| `link` | 차단 | 상대 링크가 실재 경로인가 |
| `counter` | 차단 | 살아 있는 문서의 카운터 이름과 `client/src/counters.cpp` 의 이름이 맞는가 |
| `claim` | 보고 | 강한 주장 문구가 어디에 있는가. 막지 않는다 |

## `counter` 검사

카운터 이름은 문서가 짓고 코드가 옮겨 적는다. 둘이 어긋나면 이 검사가 떨어진다.

- **문서 쪽**은 `docs/kor` 와 `docs/eng` 바로 아래의 문서다. 하위 폴더(`commit_history/`, `decisions/` 등)와
  `plan.md` 는 세지 않는다. 본문 코드 스팬만 보고 코드 블록은 보지 않는다
- 문서에서 카운터로 읽는 것은 `drop_`, `tx_drop_`, `tx_err_`, `rx_err_` 로 시작하거나 `_dropped`, `_failed`,
  `_learned`, `_rx` 로 끝나는 코드 스팬이다
- 코드 스팬은 닫는 백틱 run 이 여는 run 과 길이가 정확히 같아야 한다. 더 긴 run 의 끝부분은 닫지 못하고,
  짝이 없는 백틱은 글자로 본다
- **코드 쪽**은 `case Counter::X: return "이름";` 줄이다. 주석을 걷어 낸 뒤 읽는다. 다음 경우는 통과로
  적지 않는다. 파일이 없을 때, 이름을 하나도 읽지 못했을 때, 파일의 `case` 가운데 이 형식으로 읽지
  못한 것이 있을 때. `::` 앞뒤 공백과 이름공간 한정(`sangtachi::Counter::X`)은 읽는다
- 코드의 이름은 모양과 무관하게 문서에 그대로 있어야 한다. 그래서 모양에 맞지 않는 새 카운터도 걸린다

떨어지는 경우는 넷이다. 문서에만 있는 이름, 코드에만 있는 이름, 코드에 이미 있는데 `PENDING_COUNTERS`
에 남은 이름, `PENDING_COUNTERS` 에 있는데 문서에 없는 이름이다. 두 언어를 따로 본다.

**카운터를 구현하면 `PENDING_COUNTERS` 에서 그 이름을 뺀다.** 빼지 않으면 게이트가 떨어진다.

## 예외 셋

| 상수 | 지금 값 | 뜻 |
|------|---------|-----|
| `ALLOWED_DANGLING` | `docs/kor/experiments.md`, `docs/eng/experiments.md` | 끊긴 채로 두어도 되는 링크. Phase 9 산출물이라 아직 없다 |
| `KOR_ONLY` | `docs/kor/plan.md` | 미러 짝이 **없어도** 되는 문서. `docs/eng` 에 **있으면** 결함이다 |
| `PENDING_COUNTERS` | 이름과 들어올 Phase. 값은 `docgate.py` 에 있다 | 문서에는 있고 코드에는 아직 없어도 되는 카운터 |

앞의 둘은 전체 경로로 적는다. 이름만 적으면 깊이가 다른 엉뚱한 파일까지 통과한다.

## 시험

```bash
cd tools/docgate && python -m unittest test_docgate
```

139건이다. **판정을 고치기 전에 케이스를 먼저 쓴다.** 무엇을 일부러 지원하지 않는지도
같은 이름의 케이스로 고정해 두었다. 자세한 것은 `docgate.py` 첫머리에 있다.
