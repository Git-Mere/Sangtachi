r"""플랫폼 이음새 게이트. OS 헤더가 정해진 자리 밖에 있는지 기계가 판정한다.

ADR 0010 이 정한 규칙 하나를 명령으로 옮긴 것이다.

> OS 헤더를 포함하는 클라이언트 원본은 전부 `client/src/platform/` 아래에 둔다.
> `client/include/` 아래에는 OS 헤더가 하나도 없다.

사람이 기억해야 하던 배치 규칙을 판정으로 만든다. 스크립트가 없으면 다음 Phase 에서
조용히 무너진다.

검사는 하나다.

| 검사 | 판정 | 내용 |
|------|------|------|
| `platform` | 차단 | OS 헤더 `#include` 가 `client/src/platform/` 밖에 있는가 |

위반은 자리에 따라 두 종류로 나눠 적는다. 둘 다 차단이다.

| 자리 | 뜻 |
|------|-----|
| `client/include/` 아래 | 공개 헤더가 OS 헤더를 끌어들였다. 0 건이어야 한다 |
| `client/` 의 나머지 | 원본이 `client/src/platform/` 밖에서 OS 헤더를 포함한다 |

## 판정의 근거는 이름 목록이다

OS 헤더인지는 아래 `OS_HEADERS` 이름 목록으로만 판정한다. **허용 목록이 아니다.**
목록에 없는 헤더는 OS 헤더로 단정하지 않고 아무 말도 하지 않는다 (CLAUDE.md 문서 규칙 7:
모르면 판정 불가가 기본값이다). `<sys/epoll.h>` 나 `<pthread.h>` 를 새로 쓰기 시작하면
이 목록에 손으로 넣어야 한다. **넣기 전까지 게이트는 그것을 잡지 못한다.**

## 정해 둔 것

`#include` 를 어디까지 세는지를 여기서 정한다. 정하지 않은 채로 두지 않는다.

| 자리 | 세는가 | 왜 |
|------|--------|-----|
| 줄 주석 `// #include <windows.h>` | 아니다 | 컴파일러가 보지 않는 글자다 |
| 블록 주석 `/* ... */` (여러 줄 포함) | 아니다 | 같다 |
| 보통 문자열 `"#include <windows.h>"` | 아니다 | 같다 |
| 원시 문자열 `R"(...)"` (여러 줄) | 아니다 | 같다 |
| `#if 0` ... `#endif` 안 | **센다** | 아래 참고 |
| 조건부 컴파일(`#ifdef _WIN32`) 안 | 센다 | 같은 이유다 |

지시자 **안쪽**의 주석은 다르게 다룬다. 표준이 주석을 공백 한 칸으로 바꾸므로
`#/**/include/**/<windows.h>` 는 컴파일되는 정상 포함이다. 이것을 놓치면 판정 함수가
"모른다" 가 아니라 **틀린 답**을 낸다.

| 지시자 안쪽 | 판정 |
|-------------|------|
| `#` 과 `include` 사이, `include` 와 대상 사이의 주석 | 공백 한 칸으로 보고 **센다** |
| 그 주석이 줄바꿈을 품어도 (`#include /*` 다음 줄 `*/ <windows.h>`) | 센다. 지시자는 한 논리 줄이다 |
| 보고하는 줄 번호 | `#` 이 있는 줄이다. 대상이 있는 줄이 아니다 |
| `#` 뒤에 줄 주석이 오고 `include` 가 다음 줄에 있는 경우 | 세지 않는다. 줄바꿈이 지시자를 끝낸다 |
| 대상 전에 줄이 끝나거나 블록 주석이 닫히지 않은 경우 | 판정하지 않는다. 지시자가 완성되지 않았다 |

**전처리기를 평가하지 않는다.** `#if 0` 안의 `#include` 도 센다. 평가하려면 매크로 정의와
포함 그래프를 따라가야 하고, 그것을 반쯤 흉내 내면 조용히 틀린다. 그리고 이 게이트가 지키는
것은 컴파일 결과가 아니라 **원본 파일의 배치**다. `#if 0` 으로 감싸면 통과하는 게이트는
우회 수단을 알려 주는 것과 같다. 대신 그 대가로 **죽은 코드 안의 OS 헤더가 위반으로 잡힌다.**
그때는 그 코드를 지우거나 파일을 `client/src/platform/` 으로 옮긴다.

## 판정하지 못하는 것

- **`client/` 밖.** `tests/` 와 `tools/` 는 검사하지 않는다. ADR 0010 의 규칙이 클라이언트
  원본에 대한 것이기 때문이다.
- **`.cpp` 와 `.hpp` 밖.** `.h`, `.c`, `.cc`, `.cxx`, `.inl`, `CMakeLists.txt` 는 보지 않는다.
  이 저장소는 두 확장자만 쓴다. 다른 확장자를 쓰기 시작하면 `SOURCE_SUFFIXES` 를 고친다.
- **목록에 없는 헤더.** 위 "판정의 근거는 이름 목록이다" 참고.
- **경로가 붙은 포함.** `<winrt/windows.h>` 는 목록의 `windows.h` 와 같은 이름이 아니므로
  판정하지 않는다. 이름 목록과 **정확히 같을 때만** OS 헤더로 본다.
- **줄 이어쓰기.** 역슬래시로 `#inc\` / `lude <windows.h>` 처럼 끊어 쓴 지시자는 놓친다.
- **매크로로 만든 포함.** `#include OS_HEADER` 는 이름을 알 수 없으므로 판정하지 않는다.
- **UTF-8 이 아닌 파일.** 읽지 못한 파일은 통과로 적지 않고 결함으로 올린다.

경로 비교는 **대소문자를 가린다.** `client/src/Platform/` 은 허용 자리가 아니다. Windows 는
같은 디렉터리로 열지만 Linux 는 다른 디렉터리이고, 저장소는 철자 하나를 쓴다.

사용:

    python tools/platformgate/platformgate.py            # 저장소 루트를 자동 탐색
    python tools/platformgate/platformgate.py --list     # 허용 자리의 OS 헤더까지 전부 나열
    python tools/platformgate/platformgate.py --root .   # 루트 지정

마지막 줄은 항상 `VERDICT: pass` 또는 `VERDICT: fail` 이고, 종료 코드가 같이 따라간다.
루트가 잘못되면 판정을 내지 않고 종료 코드 2로 끝난다. **판정할 수 없는 것을 통과로 적지
않는다.**
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

CLIENT = "client"

# OS 헤더 이름. 이 목록에 **정확히 같은 이름**일 때만 OS 헤더로 판정한다.
# 허용 목록이 아니라 이름 목록이다. 여기 없는 헤더는 판정하지 않는다.
OS_HEADERS = {
    "windows.h",
    "winsock2.h",
    "ws2tcpip.h",
    "ws2spi.h",
    "mswsock.h",
    "mstcpip.h",
    "iphlpapi.h",
    "bcrypt.h",
    "ncrypt.h",
    "wincrypt.h",
    "wintun.h",
    "netioapi.h",
    "objbase.h",
    "shlwapi.h",
}

# 검사 대상 확장자. 소문자로 적고 비교도 소문자로 한다.
SOURCE_SUFFIXES = (".cpp", ".hpp")

# OS 헤더를 포함해도 되는 유일한 자리. 저장소 기준 경로의 앞부분으로 비교한다.
PLATFORM_DIR = ("client", "src", "platform")

# 공개 헤더 자리. 여기의 위반은 메시지를 따로 낸다.
PUBLIC_INCLUDE_DIR = ("client", "include")

INCLUDE_TARGET = re.compile(r'<([^>\n]*)>|"([^"\n]*)"')
DIRECTIVE_SPACE = " \t\r\v\f"
# 원시 문자열 시작. 접두사(`u8R"`, `LR"` 등)까지 받는다. 구분자는 16자까지다 (표준 한도).
RAW_STRING_OPEN = re.compile(r'(?:u8|u|U|L)?R"([^ ()\\\t\v\f\n]{0,16})\(')
IDENTIFIER_CHAR = re.compile(r"[A-Za-z0-9_]")


class GateInputError(Exception):
    """판정할 수 없는 입력. 통과로 적지 않고 따로 끝낸다."""


def include_directives(text: str) -> list[tuple[int, str]]:
    """`(줄번호, 포함 대상)` 목록. 주석과 문자열 안의 것은 내지 않는다.

    한 번의 훑기로 상태를 따라간다. `#include` 를 먼저 알아보고 그 대상을 직접 먹기 때문에
    `#include "windows.h"` 의 따옴표가 문자열 시작으로 읽히지 않는다. 순서를 뒤집어
    문자열을 먼저 지우면 따옴표 형태의 포함이 통째로 사라져 **결함 있는 저장소가 통과한다.**

    주석은 공백 한 칸과 같이 다룬다 (표준과 같다). 그래서 `/* 주석 */ #include <windows.h>`
    도 줄 첫머리의 지시자로 센다. 주석을 "줄 첫머리가 아니게 만드는 글자" 로 다루면
    정상 코드가 빠져나간다.
    """
    found: list[tuple[int, str]] = []
    index = 0
    line = 1
    length = len(text)
    # 이 줄에서 아직 공백과 주석만 지나왔는가. 전처리 지시자는 그 자리에서만 시작한다.
    at_line_start = True

    def newlines(start: int, stop: int) -> int:
        return text.count("\n", start, stop)

    while index < length:
        char = text[index]

        if char == "\n":
            line += 1
            index += 1
            at_line_start = True
            continue

        if char in " \t\r\v\f":
            index += 1
            continue

        if text.startswith("//", index):
            stop = text.find("\n", index)
            index = length if stop == -1 else stop
            continue

        if text.startswith("/*", index):
            stop = text.find("*/", index + 2)
            stop = length if stop == -1 else stop + 2
            line += newlines(index, stop)
            index = stop
            continue

        if at_line_start and char == "#":
            # 지시자는 한 논리 줄이다. 줄 번호는 `#` 이 있는 줄로 적는다.
            directive_line = line
            cursor, line, ended = _skip_directive_space(text, index + 1, line)
            if not ended and _word_at(text, cursor, "include"):
                cursor, line, ended = _skip_directive_space(text, cursor + len("include"), line)
                target = None if ended else INCLUDE_TARGET.match(text, cursor)
                if target:
                    name = target.group(1) if target.group(1) is not None else target.group(2)
                    found.append((directive_line, name))
                    cursor = target.end()
                # 대상이 없으면 (`#include OS_HEADER`, 대상 전에 줄이 끝남) 판정하지 않는다.
            index = cursor
            at_line_start = False
            continue

        if char in "RuUL" and not (index > 0 and IDENTIFIER_CHAR.match(text[index - 1])):
            raw = RAW_STRING_OPEN.match(text, index)
            if raw:
                closing = ')' + raw.group(1) + '"'
                stop = text.find(closing, raw.end())
                stop = length if stop == -1 else stop + len(closing)
                line += newlines(index, stop)
                index = stop
                at_line_start = False
                continue

        if char in "\"'":
            index = _skip_quoted(text, index, char)
            at_line_start = False
            continue

        index += 1
        at_line_start = False

    return found


def _skip_directive_space(text: str, index: int, line: int) -> tuple[int, int, bool]:
    """지시자 안쪽의 공백과 주석을 지난다. `(다음 위치, 줄 번호, 지시자가 끝났는가)` 를 낸다.

    표준이 주석을 공백 한 칸으로 바꾸므로 `#/**/include/**/<windows.h>` 는 정상 포함이다.
    `#` 앞만 그렇게 다루고 지시자 안쪽을 공백과 탭으로만 보면 **컴파일되는 포함을 조용히
    놓친다.** 판정 함수가 "모른다" 가 아니라 틀린 답을 내는 자리라 여기서 막는다.

    끝났다고 보는 자리는 셋이다. 그 자리에서는 위치를 **줄바꿈 앞에 둔다.** 바깥 훑기가
    줄 번호와 줄 첫머리 상태를 이어서 맞추게 하기 위해서다.

    - **줄바꿈.** 지시자는 한 논리 줄이다.
    - **줄 주석.** 줄 끝까지가 주석이고 그 줄바꿈이 지시자를 끝낸다. 다음 줄은 그 지시자의
      일부가 아니다.
    - **닫히지 않은 블록 주석.** 파일 끝까지 주석이므로 지시자가 완성되지 않는다.

    블록 주석 안의 줄바꿈은 지시자를 끝내지 않는다. 주석이 공백 한 칸이 되기 때문이다.
    """
    length = len(text)
    while index < length:
        char = text[index]
        if char in DIRECTIVE_SPACE:
            index += 1
            continue
        if text.startswith("/*", index):
            stop = text.find("*/", index + 2)
            if stop == -1:
                # 파일 끝이므로 뒤에서 대상을 찾을 일이 없다. `True` 는 뜻을 적어 두는 것이지
                # 오늘의 판정을 바꾸지 않는다. 변이로 확인했다.
                return length, line + text.count("\n", index, length), True
            line += text.count("\n", index, stop + 2)
            index = stop + 2
            continue
        if text.startswith("//", index):
            stop = text.find("\n", index)
            return (length if stop == -1 else stop), line, True
        if char == "\n":
            return index, line, True
        return index, line, False
    return index, line, True


def _word_at(text: str, index: int, word: str) -> bool:
    """`index` 에 낱말 `word` 가 통째로 있는가. `#includex` 를 포함으로 읽지 않기 위해서다.

    이름 대조(`startswith`)는 판정을 바꾼다. `#warning "windows.h"` 처럼 글자 수가 같은
    지시자가 거짓 위반이 되기 때문이다. 뒤 경계 대조는 **막아 두는 자리다.** `include`
    바로 뒤가 낱말 글자면 그 자리에서 대상 대조가 어차피 실패한다. 변이로 확인했다.
    """
    if not text.startswith(word, index):
        return False
    after = index + len(word)
    return after >= len(text) or not IDENTIFIER_CHAR.match(text[after])


def _skip_quoted(text: str, index: int, quote: str) -> int:
    """따옴표 리터럴의 끝 다음 위치. 줄이 끝나면 거기서 끊는다.

    닫히지 않은 리터럴을 줄 끝에서 끊는 이유는, 끊지 않으면 따옴표 한 개가 뒤의 파일
    전체를 문자열로 묻어 **결함 있는 파일이 통과하기** 때문이다.
    """
    cursor = index + 1
    length = len(text)
    while cursor < length:
        char = text[cursor]
        if char == "\\" and cursor + 1 < length and text[cursor + 1] != "\n":
            cursor += 2
            continue
        if char == "\n":
            return cursor
        cursor += 1
        if char == quote:
            return cursor
    return length


def is_os_header(target: str) -> bool:
    """포함 대상이 OS 헤더 이름 목록에 있는가.

    헤더 이름은 대소문자를 가리지 않는다 (`<Windows.h>` 도 같은 헤더다). 경로가 붙은
    `<winrt/windows.h>` 는 목록의 이름과 같지 않으므로 판정하지 않는다.
    """
    return target.strip().replace("\\", "/").lower() in OS_HEADERS


def source_files(root: Path) -> list[Path]:
    """검사 대상 파일. `client/` 아래의 `.cpp` 와 `.hpp` 뿐이다."""
    client = root / CLIENT
    if not client.is_dir():
        return []
    return sorted(
        path
        for path in client.rglob("*")
        if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES
    )


def _parts(root: Path, path: Path) -> tuple[str, ...]:
    return path.relative_to(root).parts


def in_platform_dir(parts: tuple[str, ...]) -> bool:
    """`client/src/platform/` 아래인가. 대소문자를 가린다."""
    return parts[: len(PLATFORM_DIR)] == PLATFORM_DIR


def in_public_include_dir(parts: tuple[str, ...]) -> bool:
    """`client/include/` 아래인가. 대소문자를 가린다."""
    return parts[: len(PUBLIC_INCLUDE_DIR)] == PUBLIC_INCLUDE_DIR


@dataclass
class Finding:
    where: str
    line: int
    header: str
    place: str

    def text(self) -> str:
        return f"{self.where}:{self.line}:{self.header}"

    def key(self) -> tuple[str, int, str]:
        return (self.where, self.line, self.header)


@dataclass
class Report:
    findings: list[Finding] = field(default_factory=list)
    allowed: list[Finding] = field(default_factory=list)
    files: int = 0

    @property
    def ok(self) -> bool:
        return not self.findings

    def text(self, show_list: bool = False) -> str:
        public = sum(1 for finding in self.findings if finding.place == "include")
        out = [
            f"SUMMARY: files={self.files} os_includes={len(self.findings) + len(self.allowed)} "
            f"allowed={len(self.allowed)} violations={len(self.findings)} "
            f"public_header_violations={public}"
        ]
        if show_list:
            out += [f"allowed | {finding.text()}" for finding in sorted(self.allowed, key=Finding.key)]
        out += [finding.text() for finding in sorted(self.findings, key=Finding.key)]
        out.append(f"VERDICT: {'pass' if self.ok else 'fail'}")
        return "\n".join(out)


def find_root(start: Path) -> Path:
    for candidate in [start, *start.parents]:
        if (candidate / CLIENT).is_dir() and (candidate / "tools" / "platformgate").is_dir():
            return candidate
    raise GateInputError(f"{CLIENT}/ 와 tools/platformgate/ 를 가진 상위 디렉터리를 찾지 못했다: {start}")


def run(root: Path) -> Report:
    root = Path(root).resolve()
    if not (root / CLIENT).is_dir():
        raise GateInputError(f"검사할 디렉터리가 없다: {root / CLIENT}")

    report = Report()
    for path in source_files(root):
        report.files += 1
        parts = _parts(root, path)
        where = "/".join(parts)
        try:
            text = path.read_text(encoding="utf-8-sig")
        except (UnicodeDecodeError, OSError) as error:
            # 읽지 못한 파일은 판정할 수 없다. 통과로 적지 않는다.
            report.findings.append(Finding(where, 0, f"<읽지 못했다: {type(error).__name__}>", "unreadable"))
            continue
        for number, target in include_directives(text):
            if not is_os_header(target):
                continue
            name = target.strip().replace("\\", "/")
            if in_platform_dir(parts):
                report.allowed.append(Finding(where, number, name, "platform"))
            elif in_public_include_dir(parts):
                report.findings.append(Finding(where, number, name, "include"))
            else:
                report.findings.append(Finding(where, number, name, "src"))
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="OS 헤더 배치 게이트 (ADR 0010)")
    parser.add_argument("--root", default=None, help="저장소 루트. 생략하면 위로 올라가며 찾는다")
    parser.add_argument("--list", action="store_true", dest="show_list",
                        help="허용 자리의 OS 헤더까지 전부 나열한다. 종료 코드는 바뀌지 않는다")
    args = parser.parse_args(argv)

    try:
        root = Path(args.root).resolve() if args.root else find_root(Path(__file__).resolve().parent)
        report = run(root)
    except GateInputError as error:
        print(f"INPUT ERROR: {error}", file=sys.stderr)
        return 2

    print(report.text(show_list=args.show_list))
    return 0 if report.ok else 1


if __name__ == "__main__":
    sys.exit(main())
