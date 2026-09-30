// 시험 하네스 보호 장치. 제품 코드가 아니다.
//
// **왜 있나.** MSVC 디버그 빌드에서 표준 라이브러리 단언(`span subscript out of range`
// 같은 것)이나 `abort()` 는 기본으로 **모달 창**을 띄운다. 창이 뜨면 프로세스가 사람을
// 기다리므로 시험 하네스가 멈춘다. 변이 시험은 일부러 결함을 넣는 작업이라 그 경로를
// 정확히 밟는다. 실측으로 겪었다. 한 변이가 창을 띄워 실행이 매달렸다.
//
// 창 대신 표준 오류로 내보내고 바로 끝낸다. 그러면 크래시하는 변이가 **매달림이 아니라
// 실패**로 나타난다. 판정이 사람 손을 타지 않는다.
//
// 이 파일은 `tests/` 에만 있고 `sangtachi_core` 에 들어가지 않는다.

#ifdef _WIN32

#include <crtdbg.h>
#include <stdlib.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

struct CrtReportGuard {
    CrtReportGuard() noexcept {
        // 단언, 오류, 경고를 전부 표준 오류로 보낸다. 창을 띄우지 않는다.
        //
        // 중괄호 목록 대신 배열을 쓴다. 범위 for 에 중괄호 목록을 넘기려면
        // <initializer_list> 가 있어야 하고, 이 파일에 표준 헤더를 하나 더 들이지
        // 않으려는 것이다.
        const int report_types[] = {_CRT_ASSERT, _CRT_ERROR, _CRT_WARN};
        for (const int report_type : report_types) {
            _CrtSetReportMode(report_type, _CRTDBG_MODE_FILE);
            _CrtSetReportFile(report_type, _CRTDBG_FILE_STDERR);
        }
        // abort() 의 "프로그램이 비정상 종료되었습니다" 창도 끈다.
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        // 접근 위반 같은 것에 Windows 오류 보고 창이 뜨는 것도 막는다.
        ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    }
};

// 정적 초기화 시점에 건다. Catch2 의 main 보다 먼저 돈다.
const CrtReportGuard g_crt_report_guard;

}  // namespace

#endif  // _WIN32
