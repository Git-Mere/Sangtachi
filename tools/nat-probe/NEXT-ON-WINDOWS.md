# Windows 에서 이어서 할 일

2026-09-20 Linux 세션에서 넘어온다. **순서대로 하면 된다.**

---

## 0. 받기

```powershell
git pull
cd tools\nat-probe
py test_natprobe.py        # 145/145 나와야 한다
```

`py` 가 안 먹으면 `python`. 둘 다 안 되면 python.org 에서 받을 때
"Add python.exe to PATH" 를 체크한다.

---

## 1. 제일 먼저 — 스크립트를 실제로 돌려 본다

**`unsolicited-firewall-test.ps1` 은 한 번도 실행된 적이 없다.** 리뷰 17라운드를 정적으로만
돌렸고, blocker 6건 중 **3건이 "정상 상태에서 실행조차 안 되는" 문제**였다. 남아 있을 수 있다.

```powershell
# 관리자 PowerShell, 레포 루트에서
.\tools\nat-probe\unsolicited-firewall-test.ps1 -Label us-home -Trials 2
```

**상대도 같이 돌려야 한다.** 총 4회이고 매 시행마다 화면에 `상대도 지금 시작` 이 뜬다.

```
python tools/nat-probe/natprobe.py punch --label <상대라벨> --port 47000 --unsolicited
```

**한 번 돌리는 것이 리뷰 한 라운드보다 많은 것을 알려준다.** 깨지면 그 자리에서 고친다.

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
| 2 | Windows × NAT↔NAT × **다른 ISP** 3회 | 유일하게 비어 있는 조건 조합 ([plan.md](../../docs/kor/plan.md) 5.7) |

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

프롬프트는 `~/.agents/skills/cross-review/SKILL.md` 에 있다. **다만 1번을 먼저 한다.**
실행 한 번이 정적 리뷰보다 낫다.

---

## 4. 미결 항목

| 항목 | 상태 |
|------|------|
| `TUNNEL_DROPPED` 뒤 자동 재시도 여부 | `protocol.md` 9.6 에 없다. Phase 3~5 에서 정한다 |
| 후속 3번 Windows 사전조건 절 | `plan.md`. **다음 권장 작업.** 방화벽 규칙 등록 항목은 넣지 않는다 |
| 후속 4번 spec 논리 오류 | `plan.md` |
| 후속 6번 과잉 검증 기준 | `plan.md` |
| **Phase 1 구현** | **미착수.** 코드는 `CMakeLists.txt` 와 hello `main.cpp` 뿐이다 |

---

## 5. 알아둘 것

- **이 세션은 Windows 로 옮겨가지 않는다.** `prime-agent -c` 는 그 기기의 로컬 세션만 찾는다.
  상태는 전부 레포에 있다. `CLAUDE.md` 읽기 순서를 따르면 복귀된다
- 상대에게는 `natprobe.py` 파일 하나만 보내면 된다. 의존성 없다
- **양쪽이 같은 판을 써야 한다.** 패킷 형식에 판이 있고 다르면 서로 무시한다
- 측정 맥락은 [`results/README.md`](results/README.md) 의 라벨 대응표를 본다.
  같은 망이 측정마다 다른 라벨을 쓴 경우가 있다
