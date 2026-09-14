# 2026-09-10 레포 기초 구조 구축

## 변경

CSP GitHub Best Practices 강의(src/03)의 항목에 맞춰 빈 레포에 기본 구조를 세웠다.

- `.gitignore`: `.env`, 빌드 산출물, 의존성, 캐시, 에디터, OS 파일 제외
- `.editorconfig`: UTF-8, LF, 스페이스 4칸 (md/yml/json은 2칸)
- `.env.example`: 시크릿은 `.env`에 두고 예시 키만 커밋
- `LICENSE`: MIT (라이선스 없으면 All rights reserved 상태가 됨)
- `README.md`: 개요, 설치/실행, 환경 변수, 구조, 라이선스 섹션
- `docs/`: `spec.md`, `plan.md`, `decisions/`, `commit_history/`
- `src/`, `tests/`, `scripts/`: 빈 폴더, `.gitkeep`

## 결정

- 스택이 미정이라 빌드 파일(`pyproject.toml`/`package.json`)과 버전 핀은 제외했다.
- `.github/workflows/` CI도 제외했다. 린트/테스트 명령이 없으면 빈 껍데기가 된다.
- `dist/`, `lib/`는 생성하지 않았다. 전자는 ignore 대상이고 후자는 필요할 때 만든다.
- `docs/decisions/`와 `docs/commit_history/`는 `.gitkeep` 대신 작성 규칙을 담은
  `README.md`를 넣어 폴더 용도를 드러냈다.

## 검증

- `git check-ignore -v`: `.env.example`이 `!.env.example` 부정 규칙으로 추적 대상 유지
- `git status`: 의도한 신규 파일 12개 모두 노출

## 크로스 모델 리뷰

- 리뷰어: Codex (codex-cli 0.151.0)
- 대상: 스테이징된 diff 전체
- 결과: `LGTM - no blockers` (findings 없음)
