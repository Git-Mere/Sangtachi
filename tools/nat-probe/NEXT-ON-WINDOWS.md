# Windows 에서 이어서 할 일

2026-09-20 Linux 세션에서 넘어온다. **순서대로 하면 된다.**

---

## 0. 받기

```powershell
git pull
cd tools\nat-probe
py test_natprobe.py        # Windows 151/151, Linux 149/149
```

`py` 가 안 먹으면 `python`. 둘 다 안 되면 python.org 에서 받을 때
"Add python.exe to PATH" 를 체크한다.

---

## 1. 끝났다 — 스크립트는 돈다

2026-09-20 에 Windows 에서 처음 실행했다. **파싱조차 되지 않았다.** 원인은 UTF-8 BOM
누락이고, Windows PowerShell 5.1 이 `.ps1` 을 CP949 로 읽어 한글이 깨졌다. BOM 을 붙여
고쳤고 `test_natprobe.py` 에 회귀 시험을 넣었다. 그중 1건이 실제 PowerShell 파서를 부른다
(Windows 151/151).

기계 부분은 관리자 권한으로 끝까지 확인했다. 규칙 생성, `ActiveStore` 검증 4종, 오류
경로에서의 삭제, 순서 뒤집기, 최종 정리 보고까지다. **두 번에 나눠 확인했다** — 가짜 상대로
한 번, `natprobe` 를 스텁으로 바꿔 한 번이다. 실측은 아니다. 자세한 것은
[커밋 기록](../../docs/kor/commit_history/2026-09-20-firewall-script-first-run.md).

**남은 것은 실측이고, 상대가 있어야 한다.**

```powershell
# 관리자 PowerShell, 레포 루트에서
.\tools\nat-probe\unsolicited-firewall-test.ps1 -Label us-home -Trials 2
```

**상대도 같이 돌려야 한다.** 총 4회이고 매 시행마다 화면에 `상대도 지금 시작` 이 뜬다.

```
python tools/nat-probe/natprobe.py punch --label <상대라벨> --port 47000 --unsolicited
```

**시행마다 손으로 두 번 입력한다** — 상대 엔드포인트와 동시 시작 Enter 다. `-Trials` 는
**쌍 수**이므로 시행은 그 두 배다. 위 `-Trials 2` 는 4시행이라 입력 8번, 기본 `-Trials 4` 는
8시행이라 16번이다. 상대 주소를 명령줄로 받지 않기로 한 결정의 대가이며 고치지 않는다.
`README.md` 12.1 을 본다.

> ⚠️ 규칙이 UDP 47000 을 모든 출발지에 연다. 정상 종료하면 지우지만 **강제 종료·재부팅이면
> 영구히 남는다.** 그때는 직접 지운다.
> ```powershell
> Get-NetFirewallRule -Group 'natprobe-unsolicited-test' | Remove-NetFirewallRule
> Get-NetFirewallRule -PolicyStore ActiveStore -Group 'natprobe-unsolicited-test'
> ```

---


## 2. 측정 2건

| # | 무엇 | 왜 |
|---|------|-----|
| 1 | 위 방화벽 시험 | [ADR 0002](../../docs/kor/decisions/0002-리바인딩-복구-미보장.md) 가 **잠정**이다. 이 시험이 확정한다 |
| 2 | Windows × NAT↔NAT × **다른 ISP** 3회 | 유일하게 비어 있는 조건 조합 ([design-audit.md](../../docs/kor/audit-history/design-audit.md) 후속 5번) |

2번은 핫스팟 조합으로 일부 덮었지만 핫스팟은 필수 토폴로지가 아니다. 다른 ISP 가정망
상대를 찾으면 그걸로 한다.

결과가 나오면 [`RECORD-TEMPLATE.md`](RECORD-TEMPLATE.md) 로 기록하고
[`records/`](records/) 에 넣는다. **JSON 과 기록은 `.gitignore` 대상이다** — 공개 레포이고
공인 IP 가 들어 있다.

---

## 3. 리뷰 이어가기

17라운드에서 멈췄고 18라운드는 시작만 하고 중단했다.

```powershell
git diff HEAD~1 | codex exec -o review.txt "<프롬프트>"
```

프롬프트는 `~/.agents/skills/cross-review/SKILL.md` 에 있다. **그 파일은 Windows 기기에
없다.** `codex` CLI 는 있다. 스킬을 옮겨 오거나 같은 뜻의 프롬프트를 직접 쓴다.
**다만 1번을 먼저 한다.** 실행 한 번이 정적 리뷰보다 낫다.

---

## 4. 미결 항목

| 항목 | 상태 |
|------|------|
| `TUNNEL_DROPPED` 뒤 자동 재시도 여부 | `protocol.md` 9.6 에 없다. Phase 3~5 에서 정한다 |
| 설계 감사 후속 6건 | **전부 완료됐다.** 남은 일은 `docs/kor/plan.md` 에서 읽는다 |
| **Phase 1 구현** | **미착수.** 코드는 `CMakeLists.txt` 와 hello `main.cpp` 뿐이다 |

---

## 5. 알아둘 것

- **이 세션은 Windows 로 옮겨가지 않는다.** `prime-agent -c` 는 그 기기의 로컬 세션만 찾는다.
  상태는 전부 레포에 있다. `CLAUDE.md` 읽기 순서를 따르면 복귀된다
- 상대에게는 `natprobe.py` 파일 하나만 보내면 된다. 의존성 없다
- **양쪽이 같은 판을 써야 한다.** 패킷 형식에 판이 있고 다르면 서로 무시한다
- 측정 맥락은 [`results/README.md`](results/README.md) 의 라벨 대응표를 본다.
  같은 망이 측정마다 다른 라벨을 쓴 경우가 있다
