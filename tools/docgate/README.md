# docgate

문서 게이트. 크로스 모델 리뷰에 보내기 전에 기계가 먼저 거른다.

```bash
python tools/docgate/docgate.py            # 마지막 줄이 VERDICT: pass 여야 한다
python tools/docgate/docgate.py --claims   # 강한 주장 문구 목록. 종료 코드는 안 바뀐다
python tools/docgate/docgate.py --root .   # 루트 지정
```

종료 코드는 통과 0, 실패 1, 루트를 찾지 못하면 2다.

## 검사 넷

| 검사 | 판정 | 내용 |
|------|------|------|
| `mirror` | 차단 | `docs/kor` 와 `docs/eng` 의 문서 짝이 맞는가 |
| `parity` | 차단 | 짝의 헤딩 레벨 순서, 표 행 수, 코드 블록 수, 링크 수가 같은가 |
| `link` | 차단 | 상대 링크가 실재 경로인가 |
| `claim` | 보고 | 강한 주장 문구가 어디에 있는가. 막지 않는다 |

## 예외 둘

| 상수 | 지금 값 | 뜻 |
|------|---------|-----|
| `ALLOWED_DANGLING` | `docs/kor/experiments.md`, `docs/eng/experiments.md` | 끊긴 채로 두어도 되는 링크. Phase 9 산출물이라 아직 없다 |
| `KOR_ONLY` | `docs/kor/plan.md` | 미러 짝이 **없어도** 되는 문서. `docs/eng` 에 **있으면** 결함이다 |

둘 다 전체 경로로 적는다. 이름만 적으면 깊이가 다른 엉뚱한 파일까지 통과한다.

## 시험

```bash
cd tools/docgate && python -m unittest test_docgate
```

109건이다. **판정을 고치기 전에 케이스를 먼저 쓴다.** 무엇을 일부러 지원하지 않는지도
같은 이름의 케이스로 고정해 두었다. 자세한 것은 `docgate.py` 첫머리에 있다.
