# Phase 9 의 Linux 빌드, GUI 빌드, 텔레메트리 서비스 배포 확인

## 왜 했나

Phase 9 구현 커밋에서 확인하지 못한 것이 셋 남았다. Linux 빌드(`mere-galaxy` 에 닿지 않음), GUI 빌드, 실제 인스턴스의 `deploy.py --target tm` 과
`verify.py --target tm` 이다. 텔레메트리 서비스 배포는 저장소 소유자가 승인했다.

## 무엇을 했나

| 무엇 | 결과 |
|------|------|
| Linux 빌드. `mere-galaxy`(Ubuntu 26.04, g++ 15)에 HEAD `5d5b594` 를 `git archive` 로 옮겨 Debug, `SANGTACHI_BUILD_TESTS=ON` | 경고와 오류 없이 빌드. `ctest` 75 통과, 0 실패. 어댑터 시험 1개는 root 가 없어 건너뜀 |
| 위 어댑터 시험을 `sudo ctest -R linux_tun` 으로 | 처음에는 `TUNSETIFF code=16`(EBUSY)로 실패. 그 기기에 앞서 띄운 호스트가 `Sangtachi` 장치를 쥐고 있었다. 소유자 승인으로 그 호스트를 SIGINT 로 내리자 장치가 사라졌고 `linux_tun` 4개 통과 |
| GUI 빌드. Release, `SANGTACHI_BUILD_GUI=ON`, Qt 6.12.0 `msvc2022_64`, Ninja | `sangtachi_gui.exe` 링크까지 성공. 띄워 보지는 않았다 |
| `deploy.py --target tm` | `VERDICT: pass`. `server.started` 포트 8001, journald 상한을 썼다, 연기 시험 `bad_request` |
| `verify.py --target tm` | `VERDICT: pass` |
| 인스턴스 상태 | `sangtachi-tm` 과 `sangtachi-cp` 둘 다 `active`. `MemoryMax` 256M, `CPUQuotaPerSecUSec` 200ms |

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| 남은 일 표에서 세 행을 지우고 배포했다고 적었다 | `plan.md` |
| `mere-galaxy` 의 새 빌드 폴더와 어댑터 시험이 EBUSY 로 실패하는 조건 | `plan.md` 시험 기기 절 |
| `--target tm` 의 배포와 연기 시험을 실제 인스턴스에서 돌렸다고 적었다. `--tm-load` 는 아직이다 | `tools/cp-deploy/README.md` |

## 확인하지 못한 것

- `verify.py --tm-load`. 시험 레코드가 실제 텔레메트리 테이블에 남는다
- 정상 `report` 를 받아 CloudWatch 의 `Sangtachi` 이름 공간에 쓰는 경로. 인스턴스 역할의 그쪽 `PutMetricData` 허용도 이것으로 확인된다
- `roadmap.md` Phase 9 검증의 포트 8001 판정 둘

## 검증

| 무엇 | 결과 |
|------|------|
| `docgate.py`, `--claims` | `VERDICT: pass` |

## 크로스 모델 리뷰

Codex(`gpt-6.1-sol`). 사실 일관성(A), 한영 일관성(B), 공개 레포의 계정·주소 노출(C) 렌즈를 한 번에 돌렸다. `LGTM - no blockers`. 이 절을 채운 뒤 최종 staged 차이를 같은 렌즈로 다시 돌렸다.

| 지적 | 처리 |
|------|------|
| [A] nit. `plan.md` 의 "`sudo ctest` 로만 돈다" 가 지나치다. 시험이 보는 것은 `CAP_NET_ADMIN` 이다 | 반영. 그 능력이 없으면 건너뛰고, 이 기기에서는 `sudo ctest` 로 돌린다고 적었다 |

고친 뒤 최종 staged 차이를 같은 렌즈로 한 번 더 돌렸다.
