# 경고 수준을 CMake 구성에 고정한다 (roadmap.md Phase 1 검증).
#
# 이 인터페이스 대상에 링크한 것에만 건다. FetchContent 로 가져오는 Catch2 에는 걸지
# 않는다. 남의 코드를 우리 경고 정책으로 깨뜨리면 빌드가 그 라이브러리 버전에 묶인다.
add_library(sangtachi_warnings INTERFACE)

if(MSVC)
    target_compile_options(sangtachi_warnings INTERFACE
        /W4              # roadmap.md Phase 1 이 요구하는 수준
        /WX              # 경고를 오류로 올린다
        /permissive-     # 표준 준수 모드
        /utf-8           # 원본과 실행 문자 집합을 UTF-8 로 고정한다
    )
else()
    # spec.md C-1 / C-2 가 대상을 Windows x64 / MSVC 로 못박았다. 다른 컴파일러는
    # 지원 대상이 아니며, 그 위에서 무엇이 통과하는지를 검증 근거로 쓰지 않는다.
    target_compile_options(sangtachi_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Werror
    )
endif()
