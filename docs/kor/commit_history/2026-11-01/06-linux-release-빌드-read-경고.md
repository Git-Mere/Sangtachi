# Linux Release 빌드의 `read` 경고를 고친다

## 왜 했나

집에서 두 기기 Minecraft 실측을 하려고 `mere-galaxy` 에 HEAD 를 Release 로 빌드했다. `client/src/platform/posix/console_ctrl.cpp` 의
`wait_for_enter` 에서 빌드가 실패했다.

```text
error: ignoring return value of 'ssize_t read(int, void*, size_t)' declared with attribute 'warn_unused_result' [-Werror=unused-result]
```

Ubuntu 의 g++ 15 는 최적화가 켜지면 `_FORTIFY_SOURCE=3` 을 기본으로 정의한다(`g++ -O3 -dM -E` 로 확인). 그때 glibc 의 `read` 에
`warn_unused_result` 가 붙고, 이 속성은 `(void)` 캐스트로 꺼지지 않는다. 지금까지는 Debug 로만 빌드해 드러나지 않았다
([`linux-prereq.md`](../../linux-prereq.md) 1절이 그렇게 적고 있었다).

## 무엇을 바꿨나

| 무엇 | 자리 |
|------|------|
| `read` 의 반환값을 변수에 받고 버린다 | `client/src/platform/posix/console_ctrl.cpp` |
| Release 설정을 Ubuntu 26.04 에서 빌드했다고 적었다. 24.04 에서는 돌리지 않았다 | `linux-prereq.md` 1절 |

동작은 바뀌지 않는다. 읽은 바이트도 실패도 전과 같이 쓰지 않는다.

## 검증

| 무엇 | 결과 |
|------|------|
| `mere-galaxy` Release 빌드(`-DSANGTACHI_BUILD_TESTS=OFF`) | 고치기 전 실패, 고친 뒤 경고 없이 통과 |
| `mere-galaxy` Debug 빌드와 `ctest` | 75건, 실패 0건. 어댑터 시험 1건은 권한이 없어 건너뛰었다. `sudo ctest -R linux_tun` 으로 그 시험을 포함한 4건이 통과했다 |
| Windows | 바꾼 파일은 Windows 빌드에 들지 않는다. `scripts/test.ps1` 통과 |
| `docgate.py`, `--claims` | `VERDICT: pass` |

## 크로스 모델 리뷰

Codex(`gpt-6.1-sol`). 정확성(A, 같은 모양의 다른 `(void)` 호출이 `posix/`, `linux/` 에 있는지 포함)과 문서·기록(B) 렌즈를 한 번에 돌렸다. `LGTM - no blockers` 였다. 이 절을 채운 뒤의 최종 차이도 한 번 더 확인했다.
