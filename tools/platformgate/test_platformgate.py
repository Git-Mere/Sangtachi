"""platformgate 의 판정 검증. 케이스 표를 먼저 쓰고 그 표를 돌린다.

각 케이스는 "정상 배치가 통과하는가" 와 "위반을 잡는가" 를 쌍으로 확인한다. 일부러
판정하지 않기로 한 것도 같은 이름의 케이스로 고정한다. 그래야 다음 사람이 그것을 결함으로
착각하고 규칙을 넓히지 않는다.

## 케이스 표

| # | 입력 | 기대 |
|---|------|------|
| 1 | `client/src/platform/win_hash.cpp` 에 `<windows.h>` | 통과. 허용 자리다 |
| 2 | `client/src/platform/net/sock.cpp` (하위 디렉터리) | 통과 |
| 3 | `client/include/sangtachi/hash.hpp` 에 `<winsock2.h>` | 위반 `include` |
| 4 | `client/include/sangtachi/platform/x.hpp` | 위반 `include`. 공개 헤더에는 예외가 없다 |
| 5 | `client/src/hash.cpp` (platform 밖) 에 `<windows.h>` | 위반 `src` |
| 6 | 줄 주석 안의 `#include <windows.h>` | 세지 않는다 |
| 7 | 블록 주석 (한 줄·여러 줄) 안 | 세지 않는다 |
| 8 | 보통 문자열 안 | 세지 않는다 |
| 9 | 원시 문자열 `R"(...)"` 안 (여러 줄) | 세지 않는다 |
| 10 | `#if 0` 안 | **센다.** 전처리기를 평가하지 않는다 |
| 11 | `client/src/Platform/x.cpp` (경로 대소문자) | 위반. 허용 자리가 아니다 |
| 12 | `#include <Windows.h>` (헤더 이름 대소문자) | 센다 |
| 13 | 빈 파일 | 결함 아님 |
| 14 | 목록에 없는 헤더 (`<pthread.h>`, `<sys/epoll.h>`) | 판정하지 않는다 |
| 15 | 경로가 붙은 `<winrt/windows.h>` | 판정하지 않는다 |
| 16 | `#include "windows.h"` (따옴표) | 센다 |
| 17 | `#   include\t<windows.h>` (공백 변형) | 센다 |
| 18 | `.h`, `.txt` 등 대상 밖 확장자 | 검사하지 않는다 |
| 19 | `client/` 밖 (`tests/`) | 검사하지 않는다 |
| 20 | 줄 이어쓰기 `#inc\` + `lude` | **놓친다.** 지원하지 않는다 |
| 20a | `#/**/include <windows.h>` | 센다. 주석은 공백 한 칸이다 |
| 20b | `#include/**/<windows.h>` | 센다 |
| 20c | 지시자 안의 여러 줄 블록 주석 | 센다. 줄 번호는 `#` 이 있는 줄 |
| 20d | `# // 주석` 다음 줄의 `include` | 세지 않는다. 줄바꿈이 지시자를 끝낸다 |
| 20e | 지시자 안에서 닫히지 않은 블록 주석 | 판정하지 않는다 |
| 20f | `#include /* 주석 */` 뒤에 대상이 없다 | 판정하지 않는다 |
| 20g | `#includex <windows.h>` | 포함이 아니다 |
| 21 | 매크로 포함 `#include OS_HEADER` | 판정하지 않는다 |
| 22 | UTF-8 이 아닌 파일 | 결함으로 올린다. 통과로 적지 않는다 |
| 23 | 한 파일의 여러 위반 | 각각 한 줄 |
| 24 | CRLF·BOM | 줄 번호가 맞는다 |
| 25 | 루트에 `client/` 가 없다 | 판정하지 않고 종료 코드 2 |
| 26 | 마지막 줄과 종료 코드 | `VERDICT: pass`/`fail` 과 0/1 |
"""

import io
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stdout, redirect_stderr
from pathlib import Path

import platformgate


def write(root: Path, rel: str, text: str) -> None:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


WINDOWS = "#include <windows.h>\n"
CLEAN_HEADER = """#pragma once

#include <cstdint>
#include <string>
"""


class GateCase(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="platformgate-test-"))
        self.addCleanup(shutil.rmtree, self.root, True)

    def build(self) -> None:
        """정상 배치. OS 헤더는 `client/src/platform/` 안에만 있다."""
        write(self.root, "client/include/sangtachi/hash.hpp", CLEAN_HEADER)
        write(self.root, "client/src/hash.cpp", '#include "sangtachi/hash.hpp"\n\n#include <span>\n')
        write(self.root, "client/src/platform/win_hash.cpp",
              '#include "sangtachi/hash.hpp"\n\n#include <windows.h>\n#include <bcrypt.h>\n')
        write(self.root, "client/src/platform/win_hash.hpp", "#pragma once\n\n#include <windows.h>\n")

    def run_gate(self) -> platformgate.Report:
        return platformgate.run(self.root)

    def headers(self, report: platformgate.Report) -> list[str]:
        return [finding.text() for finding in sorted(report.findings, key=platformgate.Finding.key)]


class TestDirectiveScan(unittest.TestCase):
    """`include_directives` 단위. 어느 자리를 세는지가 여기서 정해진다."""

    def test_plain_angle_include(self) -> None:
        self.assertEqual(platformgate.include_directives(WINDOWS), [(1, "windows.h")])

    def test_quoted_include_is_not_eaten_as_a_string(self) -> None:
        # 케이스 16. 문자열을 먼저 지우면 이 형태가 통째로 사라진다.
        self.assertEqual(platformgate.include_directives('#include "windows.h"\n'), [(1, "windows.h")])

    def test_whitespace_variants(self) -> None:
        # 케이스 17.
        text = "#   include\t<windows.h>\n  #include  <bcrypt.h>\n"
        self.assertEqual(platformgate.include_directives(text), [(1, "windows.h"), (2, "bcrypt.h")])

    def test_line_comment_is_not_counted(self) -> None:
        # 케이스 6.
        self.assertEqual(platformgate.include_directives("// #include <windows.h>\n"), [])

    def test_trailing_line_comment_does_not_hide_the_next_line(self) -> None:
        text = "int x; // #include <ws2spi.h>\n#include <windows.h>\n"
        self.assertEqual(platformgate.include_directives(text), [(2, "windows.h")])

    def test_block_comment_single_line(self) -> None:
        # 케이스 7.
        self.assertEqual(platformgate.include_directives("/* #include <windows.h> */\n"), [])

    def test_block_comment_multi_line_keeps_line_numbers(self) -> None:
        text = "/*\n#include <windows.h>\n*/\n#include <bcrypt.h>\n"
        self.assertEqual(platformgate.include_directives(text), [(4, "bcrypt.h")])

    def test_block_comment_before_an_include_on_the_same_line(self) -> None:
        # 주석은 공백과 같다. 줄 첫머리 판정을 깨지 않아야 한다 (정상 코드가 통과하는가).
        self.assertEqual(platformgate.include_directives("/* 주석 */ #include <windows.h>\n"),
                         [(1, "windows.h")])

    def test_unterminated_block_comment_swallows_the_rest(self) -> None:
        # 닫히지 않은 주석은 파일 끝까지 주석이다 (컴파일러와 같다).
        self.assertEqual(platformgate.include_directives("/* 열기\n#include <windows.h>\n"), [])

    def test_string_literal_is_not_counted(self) -> None:
        # 케이스 8.
        text = 'const char* s = "#include <windows.h>";\n'
        self.assertEqual(platformgate.include_directives(text), [])

    def test_raw_string_multi_line_is_not_counted(self) -> None:
        # 케이스 9.
        text = 'const char* s = R"(\n#include <windows.h>\n)";\n#include <bcrypt.h>\n'
        self.assertEqual(platformgate.include_directives(text), [(4, "bcrypt.h")])

    def test_raw_string_with_delimiter_and_prefix(self) -> None:
        text = 'auto s = u8R"sql(\n#include <windows.h>\n)sql";\n#include <wintun.h>\n'
        self.assertEqual(platformgate.include_directives(text), [(4, "wintun.h")])

    def test_raw_string_close_needs_the_same_delimiter(self) -> None:
        text = 'auto s = R"tag(\n)"\n#include <windows.h>\n)tag";\n#include <bcrypt.h>\n'
        self.assertEqual(platformgate.include_directives(text), [(5, "bcrypt.h")])

    def test_identifier_ending_in_r_does_not_open_a_raw_string(self) -> None:
        text = 'int FOOR = 1;\n#include <windows.h>\n'
        self.assertEqual(platformgate.include_directives(text), [(2, "windows.h")])

    def test_block_marker_inside_a_line_comment_does_not_open_a_block_comment(self) -> None:
        # 줄 주석을 건너뛰지 않으면 이 `/*` 가 뒤의 파일을 통째로 주석으로 묻는다.
        text = "// /* 설명\n#include <windows.h>\n"
        self.assertEqual(platformgate.include_directives(text), [(2, "windows.h")])

    def test_raw_string_marker_inside_a_line_comment_is_inert(self) -> None:
        text = '// R"( 설명\n#include <windows.h>\n'
        self.assertEqual(platformgate.include_directives(text), [(2, "windows.h")])

    def test_quote_inside_a_comment_does_not_open_a_string(self) -> None:
        text = '// 따옴표 " 하나\n#include <windows.h>\n'
        self.assertEqual(platformgate.include_directives(text), [(2, "windows.h")])

    def test_comment_marker_inside_a_string_does_not_open_a_comment(self) -> None:
        text = 'const char* s = "/*";\n#include <windows.h>\n'
        self.assertEqual(platformgate.include_directives(text), [(2, "windows.h")])

    def test_escaped_quote_inside_a_string(self) -> None:
        text = 'const char* s = "a\\"b";\n#include <windows.h>\n'
        self.assertEqual(platformgate.include_directives(text), [(2, "windows.h")])

    def test_unterminated_string_ends_at_the_newline(self) -> None:
        # 따옴표 한 개가 뒤의 파일 전체를 묻으면 결함 있는 파일이 통과한다.
        text = 'const char* s = "열기\n#include <windows.h>\n'
        self.assertEqual(platformgate.include_directives(text), [(2, "windows.h")])

    def test_character_literal_with_a_quote(self) -> None:
        text = "char q = '\"';\n#include <windows.h>\n"
        self.assertEqual(platformgate.include_directives(text), [(2, "windows.h")])

    def test_if_zero_is_still_counted(self) -> None:
        # 케이스 10. 전처리기를 평가하지 않는다. 우회 수단을 만들지 않기 위해서다.
        text = "#if 0\n#include <windows.h>\n#endif\n"
        self.assertEqual(platformgate.include_directives(text), [(2, "windows.h")])

    def test_ifdef_guard_is_still_counted(self) -> None:
        text = "#ifdef _WIN32\n#include <winsock2.h>\n#endif\n"
        self.assertEqual(platformgate.include_directives(text), [(2, "winsock2.h")])

    def test_include_not_at_line_start_is_not_a_directive(self) -> None:
        self.assertEqual(platformgate.include_directives("int x; #include <windows.h>\n"), [])

    def test_macro_include_is_not_judged(self) -> None:
        # 케이스 21. 이름을 알 수 없다.
        text = "#include OS_HEADER\n#include <bcrypt.h>\n"
        self.assertEqual(platformgate.include_directives(text), [(2, "bcrypt.h")])

    def test_comment_between_hash_and_include(self) -> None:
        # 케이스 20a. 표준은 주석을 공백 한 칸으로 바꾼다. 컴파일되는 포함이다.
        self.assertEqual(platformgate.include_directives("#/**/include <windows.h>\n"),
                         [(1, "windows.h")])

    def test_comment_between_include_and_target(self) -> None:
        # 케이스 20b.
        self.assertEqual(platformgate.include_directives("#include/**/<windows.h>\n"),
                         [(1, "windows.h")])

    def test_comment_on_both_sides_with_a_quoted_target(self) -> None:
        text = '#  /*a*/  include /*b*/ "windows.h"\n'
        self.assertEqual(platformgate.include_directives(text), [(1, "windows.h")])

    def test_multi_line_comment_inside_a_directive_reports_the_hash_line(self) -> None:
        # 케이스 20c. 지시자는 한 논리 줄이다. 뒤 줄 번호도 밀리지 않아야 한다.
        text = "#include /*\n\n*/ <windows.h>\n#include <bcrypt.h>\n"
        self.assertEqual(platformgate.include_directives(text), [(1, "windows.h"), (4, "bcrypt.h")])

    def test_line_comment_after_hash_does_not_continue_to_the_next_line(self) -> None:
        # 케이스 20d. 다음 줄의 `include` 를 그 지시자의 일부로 읽지 않는다.
        text = "# // 주석\ninclude <windows.h>\n"
        self.assertEqual(platformgate.include_directives(text), [])

    def test_line_comment_after_include_does_not_reach_the_next_line_target(self) -> None:
        text = "#include // 주석\n<windows.h>\n#include <bcrypt.h>\n"
        self.assertEqual(platformgate.include_directives(text), [(3, "bcrypt.h")])

    def test_unterminated_comment_inside_a_directive_is_not_judged(self) -> None:
        # 케이스 20e. 지시자가 완성되지 않는다. 뒤는 전부 주석이다.
        self.assertEqual(platformgate.include_directives("#include /* 열림\n<windows.h>\n"), [])

    def test_directive_with_a_comment_but_no_target_is_not_judged(self) -> None:
        # 케이스 20f.
        text = "#include /* 주석 */\n#include <bcrypt.h>\n"
        self.assertEqual(platformgate.include_directives(text), [(2, "bcrypt.h")])

    def test_hash_alone_on_a_line_is_harmless(self) -> None:
        self.assertEqual(platformgate.include_directives("#\n#include <bcrypt.h>\n"),
                         [(2, "bcrypt.h")])

    def test_other_directives_with_a_target_shaped_tail_are_not_includes(self) -> None:
        # 지시자 이름을 보지 않으면 이 둘이 거짓 위반이 된다. `#warning` 과 `#import` 는
        # `include` 와 글자 수가 같아서 이름을 세지 않고 건너뛰는 구현을 바로 드러낸다.
        self.assertEqual(platformgate.include_directives('#warning "windows.h" 를 쓰지 마라\n'), [])
        self.assertEqual(platformgate.include_directives("#import <windows.h>\n"), [])

    def test_include_must_be_a_whole_word(self) -> None:
        # 케이스 20g.
        self.assertEqual(platformgate.include_directives("#includex <windows.h>\n"), [])
        self.assertEqual(platformgate.include_directives("#include_next <windows.h>\n"), [])

    def test_no_space_before_the_target(self) -> None:
        self.assertEqual(platformgate.include_directives("#include<windows.h>\n"), [(1, "windows.h")])

    def test_comment_between_hash_and_a_non_include_directive(self) -> None:
        # `#/*c*/define` 을 포함으로 읽지 않고, 그 뒤 판정도 이어져야 한다.
        text = "#/*c*/define WIN32_LEAN_AND_MEAN\n#include <bcrypt.h>\n"
        self.assertEqual(platformgate.include_directives(text), [(2, "bcrypt.h")])

    def test_line_splice_is_not_supported(self) -> None:
        # 케이스 20. 놓치는 것을 알고 둔다. 넓히려면 여기와 첫머리를 같이 고친다.
        self.assertEqual(platformgate.include_directives("#inc\\\nlude <windows.h>\n"), [])

    def test_crlf_line_numbers(self) -> None:
        # 케이스 24.
        text = "#pragma once\r\n\r\n#include <windows.h>\r\n"
        self.assertEqual(platformgate.include_directives(text), [(3, "windows.h")])

    def test_empty_file(self) -> None:
        # 케이스 13.
        self.assertEqual(platformgate.include_directives(""), [])

    def test_define_is_not_an_include(self) -> None:
        text = "#define WIN32_LEAN_AND_MEAN\n#include <windows.h>\n"
        self.assertEqual(platformgate.include_directives(text), [(2, "windows.h")])


class TestHeaderNames(unittest.TestCase):
    def test_every_listed_header_is_recognised(self) -> None:
        for name in sorted(platformgate.OS_HEADERS):
            self.assertTrue(platformgate.is_os_header(name), name)

    def test_header_name_case_is_ignored(self) -> None:
        # 케이스 12.
        self.assertTrue(platformgate.is_os_header("Windows.h"))
        self.assertTrue(platformgate.is_os_header("WinSock2.H"))

    def test_unlisted_header_is_not_judged(self) -> None:
        # 케이스 14. 모르는 헤더를 OS 헤더로 단정하지 않는다.
        for name in ("pthread.h", "sys/epoll.h", "string", "sangtachi/hash.hpp", "unistd.h"):
            self.assertFalse(platformgate.is_os_header(name), name)

    def test_prefixed_path_is_not_judged(self) -> None:
        # 케이스 15.
        self.assertFalse(platformgate.is_os_header("winrt/windows.h"))
        self.assertFalse(platformgate.is_os_header("sys/windows.h"))

    def test_backslash_path_is_normalised(self) -> None:
        self.assertFalse(platformgate.is_os_header("winrt\\windows.h"))

    def test_surrounding_space_is_ignored(self) -> None:
        self.assertTrue(platformgate.is_os_header(" windows.h "))


class TestPlacement(GateCase):
    def test_clean_tree_passes(self) -> None:
        # 케이스 1·2. 정상 배치가 통과하는지부터 본다.
        self.build()
        write(self.root, "client/src/platform/net/sock.cpp", "#include <winsock2.h>\n")
        report = self.run_gate()
        self.assertTrue(report.ok, report.text())
        self.assertEqual(len(report.allowed), 4)
        self.assertTrue(report.text().endswith("VERDICT: pass"))

    def test_public_header_violation(self) -> None:
        # 케이스 3.
        self.build()
        write(self.root, "client/include/sangtachi/hash.hpp", CLEAN_HEADER + "#include <winsock2.h>\n")
        report = self.run_gate()
        self.assertFalse(report.ok)
        self.assertEqual(self.headers(report), ["client/include/sangtachi/hash.hpp:5:winsock2.h"])
        self.assertEqual(report.findings[0].place, "include")

    def test_platform_named_directory_under_include_is_still_a_violation(self) -> None:
        # 케이스 4. `client/include/` 에는 예외가 없다.
        self.build()
        write(self.root, "client/include/sangtachi/platform/win.hpp", WINDOWS)
        report = self.run_gate()
        self.assertFalse(report.ok)
        self.assertEqual(report.findings[0].place, "include")

    def test_source_outside_platform_violation(self) -> None:
        # 케이스 5.
        self.build()
        write(self.root, "client/src/hash.cpp", WINDOWS)
        report = self.run_gate()
        self.assertFalse(report.ok)
        self.assertEqual(self.headers(report), ["client/src/hash.cpp:1:windows.h"])
        self.assertEqual(report.findings[0].place, "src")

    def test_client_root_file_is_checked(self) -> None:
        self.build()
        write(self.root, "client/main.cpp", WINDOWS)
        self.assertFalse(self.run_gate().ok)

    def test_platform_directory_case_does_not_count(self) -> None:
        # 케이스 11. 저장소는 철자 하나를 쓴다. Linux 에서 `Platform` 은 다른 디렉터리다.
        # `build()` 를 쓰지 않는다. Windows 는 대소문자를 가리지 않는 파일 시스템이라
        # 소문자 `platform/` 이 이미 있으면 `Platform/` 을 만들 수 없고, 그 트리에서는
        # 이 케이스를 세울 수 없다. 그 한계는 README 에 적어 두었다.
        write(self.root, "client/src/Platform/win.cpp", WINDOWS)
        report = self.run_gate()
        self.assertEqual(self.headers(report), ["client/src/Platform/win.cpp:1:windows.h"])
        self.assertFalse(report.ok)

    def test_similar_directory_name_does_not_count(self) -> None:
        self.build()
        write(self.root, "client/src/platform_win/win.cpp", WINDOWS)
        self.assertFalse(self.run_gate().ok)

    def test_platform_directory_elsewhere_does_not_count(self) -> None:
        self.build()
        write(self.root, "client/platform/win.cpp", WINDOWS)
        self.assertFalse(self.run_gate().ok)

    def test_multiple_violations_in_one_file(self) -> None:
        # 케이스 23.
        self.build()
        write(self.root, "client/src/hash.cpp", "#include <windows.h>\n#include <string>\n#include <bcrypt.h>\n")
        report = self.run_gate()
        self.assertEqual(self.headers(report),
                         ["client/src/hash.cpp:1:windows.h", "client/src/hash.cpp:3:bcrypt.h"])

    def test_comment_and_string_do_not_produce_a_violation(self) -> None:
        # 케이스 6·8·9 를 게이트 수준에서 다시 본다.
        self.build()
        write(self.root, "client/src/hash.cpp",
              '// #include <windows.h>\n/* #include <bcrypt.h> */\nconst char* s = "#include <wintun.h>";\n'
              'const char* r = R"(\n#include <ncrypt.h>\n)";\n')
        self.assertTrue(self.run_gate().ok, self.run_gate().text())

    def test_comment_inside_the_directive_is_still_a_violation(self) -> None:
        # 케이스 20a~20c 를 게이트 수준에서 다시 본다. 컴파일되는 포함을 놓치면 안 된다.
        self.build()
        write(self.root, "client/src/hash.cpp", "#/**/include <windows.h>\n")
        write(self.root, "client/include/sangtachi/hash.hpp", "#include /*\n*/ <bcrypt.h>\n")
        report = self.run_gate()
        self.assertFalse(report.ok)
        self.assertEqual(self.headers(report),
                         ["client/include/sangtachi/hash.hpp:1:bcrypt.h",
                          "client/src/hash.cpp:1:windows.h"])

    def test_unlisted_header_outside_platform_passes(self) -> None:
        # 케이스 14. 판정하지 않는다는 것은 통과한다는 뜻이다. README 에 적어 둔다.
        self.build()
        write(self.root, "client/src/hash.cpp", "#include <pthread.h>\n#include <sys/epoll.h>\n")
        self.assertTrue(self.run_gate().ok)

    def test_other_suffixes_are_not_checked(self) -> None:
        # 케이스 18.
        self.build()
        write(self.root, "client/src/win.h", WINDOWS)
        write(self.root, "client/src/notes.txt", WINDOWS)
        write(self.root, "client/src/win.inl", WINDOWS)
        self.assertTrue(self.run_gate().ok)

    def test_outside_client_is_not_checked(self) -> None:
        # 케이스 19.
        self.build()
        write(self.root, "tests/loop_test.cpp", WINDOWS)
        write(self.root, "tools/x/probe.cpp", WINDOWS)
        self.assertTrue(self.run_gate().ok)

    def test_empty_file_is_not_a_finding(self) -> None:
        # 케이스 13.
        self.build()
        write(self.root, "client/src/empty.cpp", "")
        write(self.root, "client/include/sangtachi/empty.hpp", "")
        self.assertTrue(self.run_gate().ok)

    def test_bom_file_line_numbers(self) -> None:
        # 케이스 24.
        self.build()
        path = self.root / "client/src/hash.cpp"
        path.write_bytes("\ufeff#pragma once\r\n#include <windows.h>\r\n".encode("utf-8"))
        report = self.run_gate()
        self.assertEqual(self.headers(report), ["client/src/hash.cpp:2:windows.h"])

    def test_undecodable_file_is_a_finding_not_a_pass(self) -> None:
        # 케이스 22. 판정할 수 없는 것을 통과로 적지 않는다.
        self.build()
        (self.root / "client/src/cp949.cpp").write_bytes(b"// \xc7\xd1\xb1\xdb\n#include <string>\n")
        report = self.run_gate()
        self.assertFalse(report.ok)
        self.assertEqual(report.findings[0].place, "unreadable")
        self.assertIn("client/src/cp949.cpp:0:", report.findings[0].text())

    def test_file_count(self) -> None:
        self.build()
        self.assertEqual(self.run_gate().files, 4)


class TestReportAndMain(GateCase):
    def test_verdict_line_and_exit_code_on_pass(self) -> None:
        # 케이스 26.
        self.build()
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = platformgate.main(["--root", str(self.root)])
        self.assertEqual(code, 0)
        self.assertEqual(buffer.getvalue().strip().splitlines()[-1], "VERDICT: pass")

    def test_verdict_line_and_exit_code_on_fail(self) -> None:
        self.build()
        write(self.root, "client/src/hash.cpp", WINDOWS)
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = platformgate.main(["--root", str(self.root)])
        lines = buffer.getvalue().strip().splitlines()
        self.assertEqual(code, 1)
        self.assertEqual(lines[-1], "VERDICT: fail")
        self.assertIn("client/src/hash.cpp:1:windows.h", lines)

    def test_missing_client_directory_exits_two_without_a_verdict(self) -> None:
        # 케이스 25. 판정할 수 없으면 판정하지 않는다.
        empty = Path(tempfile.mkdtemp(prefix="platformgate-empty-"))
        self.addCleanup(shutil.rmtree, empty, True)
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = platformgate.main(["--root", str(empty)])
        self.assertEqual(code, 2)
        self.assertNotIn("VERDICT", out.getvalue())
        self.assertIn("INPUT ERROR", err.getvalue())

    def test_list_flag_shows_allowed_includes_and_keeps_the_exit_code(self) -> None:
        self.build()
        out = io.StringIO()
        with redirect_stdout(out):
            code = platformgate.main(["--root", str(self.root), "--list"])
        text = out.getvalue()
        self.assertEqual(code, 0)
        self.assertIn("allowed | client/src/platform/win_hash.cpp:3:windows.h", text)
        self.assertTrue(text.strip().endswith("VERDICT: pass"))

    def test_summary_counts(self) -> None:
        self.build()
        write(self.root, "client/include/sangtachi/hash.hpp", WINDOWS)
        write(self.root, "client/src/hash.cpp", WINDOWS)
        summary = self.run_gate().text().splitlines()[0]
        self.assertIn("os_includes=5", summary)
        self.assertIn("allowed=3", summary)
        self.assertIn("violations=2", summary)
        self.assertIn("public_header_violations=1", summary)

    def test_find_root_walks_up(self) -> None:
        (self.root / "client").mkdir(parents=True, exist_ok=True)
        (self.root / "tools" / "platformgate").mkdir(parents=True, exist_ok=True)
        found = platformgate.find_root(self.root / "tools" / "platformgate")
        self.assertEqual(found, self.root)

    def test_find_root_raises_when_absent(self) -> None:
        empty = Path(tempfile.mkdtemp(prefix="platformgate-empty-"))
        self.addCleanup(shutil.rmtree, empty, True)
        with self.assertRaises(platformgate.GateInputError):
            platformgate.find_root(empty)


if __name__ == "__main__":
    unittest.main()
