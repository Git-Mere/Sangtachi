# 2026-09-14 문서 이중 언어 구조 전환

## 변경

`docs/` 하위를 `docs/kor/`와 `docs/eng/`로 나누고, 두 트리가 동일한 파일 구성을 갖게 했다.

```text
docs/
+-- kor/                      한국어 (기존 문서 이동)
|   +-- architecture.md
|   +-- spec.md
|   +-- roadmap.md
|   +-- plan.md
|   +-- first_design.md       영어 원문을 한국어로 번역
|   +-- decisions/README.md
|   +-- commit_history/*.md
+-- eng/                      영어 (신규 번역)
    +-- (동일 구성)
```

- `first_design.md`는 원문이 영어이므로 `eng/`에 원문을 두고 `kor/`에 번역본을 새로 작성했다.
- 나머지 문서는 한국어가 원문이므로 `kor/`에 두고 `eng/`에 번역본을 작성했다.
- 모든 문서 상단에 반대 언어판으로 가는 링크를 넣었다.
- 루트 `README.md`의 레포 구조 설명을 `docs/kor/`, `docs/eng/`로 갱신했다.

## 결정

- 파일 이름은 양쪽 모두 영어로 통일했다. 경로가 언어를 나타내므로 파일명까지
  번역하면 상호 링크와 커밋 기록 대조가 번거로워진다.
- `docs/` 루트에는 인덱스 파일을 두지 않았다. 요청받은 구조는 `eng/`와 `kor/`가
  서로 동일한 형태를 갖는 것이고, 루트 파일은 그 대칭을 깨뜨린다.
- `protocol.md`와 `experiments.md`는 양쪽 모두 아직 없다. Phase 5와 Phase 9의
  산출물이며, 링크만 미리 걸려 있다. 작성 시점에 양쪽을 함께 만든다.

## 이번에 함께 고친 것

문서를 옮기면서 발견한 기존 불일치를 정리했다.

- `architecture.md` 3.1 모듈 표에 Wintun이 "승인 필요"로 남아 있었다. 11장
  의존성 표만 승인 완료로 바꾸고 모듈 표를 빠뜨린 것이라 함께 고쳤다.
- `spec.md`의 NFR-9가 NFR-1과 NFR-2 사이에 끼어 있었다. 번호순으로 옮겼다.
- `docs/experiments.md`, `docs/protocol.md` 형태의 절대 경로 참조를 같은 폴더
  기준 상대 링크로 바꿨다. 언어 폴더 분리로 기존 경로가 깨질 참조였다.
- `roadmap.md` Phase 5의 선행 조건에 `HELLO_ACK`이 빠져 있었다. Phase 4에서
  구현하기로 한 패킷 타입이므로 추가했다.
- `roadmap.md` 우선순위 블록의 정렬이 어긋나 있었다.

## 검증

- `eng/`와 `kor/`의 파일 목록 일치
- 문서별 제목 개수 일치: architecture 23, spec 15, roadmap 45, first_design 56
- `first_design.md` 절 번호(1~23) 양쪽 일치
- `roadmap.md` Phase 제목(1~9) 양쪽 일치
- 전체 마크다운 상대 링크 검사: 의도된 선행 참조 2건(`protocol.md`, `experiments.md`)을
  제외하면 모두 실재 경로로 해석됨. 이 둘은 양쪽에 대칭적으로 없으며 각각 Phase 4/5와
  Phase 9에서 작성한다

## 유지 비용

문서가 두 벌이 되었으므로 드리프트 위험이 생겼다. 문서를 고칠 때는 같은 커밋에서
양쪽을 함께 수정한다. 이 규칙을 `architecture.md` 10장 레포 구조 절에 명시했다.

## 크로스 모델 리뷰

- 리뷰어: Codex (GPT), 번역 정확성 및 내부 모순 렌즈
- 결과: blocker 2, warn 7, nit 1. 전부 반영. 양쪽 언어에 동일하게 적용했다

| 심각도 | 지적 | 처리 |
|--------|------|------|
| blocker | Phase 4의 RTT 기준선을 ICMP `ping`으로 잡았으나, NAT 뒤 피어를 향한 ICMP는 차단되거나 피어의 공유기가 응답해 같은 경로를 재지 못한다 | 같은 소켓/엔드포인트 쌍의 타임스탬프 UDP 에코를 기준선으로 변경. ICMP는 참고 정보로 강등 |
| blocker | Phase 6 산출물 그림이 두 가상 IP 간 통신을 보여주는데, 같은 절의 검증은 그것이 Phase 7 내용이라고 적고 있었다 | 산출물을 단일 호스트 read/inject 그림으로 교체하고 피어 간 통신은 Phase 7 산출물임을 명시 |
| warn | `protocol.md`를 Phase 4 착수 시 기록한다고 써놓고 레포 구조와 roadmap은 Phase 5 작성이라고 했다 | Phase 4에서 착수, Phase 5에서 완성으로 통일 |
| warn | 모든 실패에 공인 엔드포인트 기록을 요구하지만 `STUN_DISCOVERY_FAILED`는 엔드포인트 자체가 없다 | 확보 가능한 필드만 기록하도록 완화 |
| warn | 텔레메트리의 "keepalive 실패 횟수"가 수신 기반 단절 판정과 모순된다. UDP keepalive에는 응답이 없어 전달 실패를 셀 수 없다 | 로컬 `sendto` 오류로 정의를 좁히고 유휴 타임아웃 발생 횟수를 별도 지표로 추가 |
| warn | 추적표는 FR-13을 Phase 5에 할당했으나 Phase 5는 FR-13을 언급하지 않고 `TUNNEL_DROPPED`를 검증하지 않는다 | Phase 5 관련 요구사항에 FR-13 추가, 유휴 타임아웃 시험 신설 |
| warn | 지원 조건 문구 "양쪽 피어 모두 동일한 공인 IP:Port"가 두 피어가 같은 엔드포인트를 쓴다는 뜻으로 읽힌다 | "각 피어가 두 STUN 서버에 대해 자신의 엔드포인트를 동일하게 관측하는 경우"로 명확화 |
| warn | CGNAT를 정의상 미지원으로 분류했으나 일부 CGNAT는 엔드포인트 독립 매핑을 써서 홀펀칭이 된다 | 통신사 구성이 아닌 관측된 거동으로 분류하고 CGNAT는 "어려울 수 있는 토폴로지"로 강등 |
| warn | "끊긴 링크 0건"이 `protocol.md`/`experiments.md` 부재와 모순된다 | 의도된 선행 참조 2건을 명시적으로 보고하도록 문구 수정 |
| nit | 영문 "All UDP send and receive shares" 비문 | "All UDP sends and receives share"로 수정 |
