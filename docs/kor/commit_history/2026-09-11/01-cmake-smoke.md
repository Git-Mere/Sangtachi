# 2026-09-11 CMake 스모크

> 이 기록은 커밋 `31e4240` 시점에 남기지 못하고 2026-10-08에 소급해 작성했다.
> 내용은 그 커밋의 diff 에서만 재구성했다. 당시의 의도나 검증은 diff 에 없으므로 적지 않는다.

## 변경

- `CMakeLists.txt` 신설. `cmake_minimum_required(VERSION 3.20)`, 프로젝트 이름 `Hamychi`, C++20 필수
  (`CMAKE_CXX_STANDARD 20`, `CMAKE_CXX_STANDARD_REQUIRED ON`), 실행 파일 `Hamychi` 하나
- `src/main.cpp` 신설. `hello again!` 한 줄을 출력하고 0 을 돌려준다
- 실행 파일에 `include/` 를 포함 경로로 걸었다. 이 커밋의 트리에는 `include/` 폴더가 없다

커밋 메시지는 `test Cmake` 다.

## 그 뒤

`789da5e` (Phase 1 빌드 뼈대와 Winsock2 초기화)가 `src/main.cpp` 를 지우고 `client/src/main.cpp` 를
새로 만들었으며 루트 `CMakeLists.txt` 를 고쳐 썼다. `0195005` 가 프로젝트 이름을 `hamychi` 에서
`sangtachi` 로 바꿨다.

## 검증

당시의 빌드나 실행 결과는 남아 있지 않다.

## 크로스 모델 리뷰

당시 리뷰 기록은 없다.
