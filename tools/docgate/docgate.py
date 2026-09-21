"""문서 게이트. 크로스 모델 리뷰에 보내기 전에 기계가 먼저 거르는 검사.

`design-audit.md` 6장 규칙 5(같은 주장 복사 금지)와 CLAUDE.md 의 미러 규약을 명령으로
옮긴 것이다. 사람이 기억해야 하던 검사를 판정으로 만든다.

검사는 넷이다.

| 검사 | 판정 | 내용 |
|------|------|------|
| `mirror` | 차단 | `docs/kor` 와 `docs/eng` 의 문서 짝이 맞는가. `KOR_ONLY` 만 예외다 |
| `parity` | 차단 | 짝의 헤딩 레벨 순서, 표 행 수, 코드 블록 수, 링크 수가 같은가 |
| `link` | 차단 | 상대 링크가 실재 경로인가 (예외 목록만 제외) |
| `claim` | 보고 | 강한 주장 문구가 어디에 있는가. 막지 않고 목록만 낸다 |

`claim` 은 판정하지 않는다. 문구 하나가 곧 결함은 아니기 때문이다. 주장 강도를 낮출 때
전수 확인을 손으로 하지 않게 하는 것이 목적이다. `--claims` 는 종료 코드를 바꾸지 않는다.

## 일부러 좁게 둔 것

아래는 지원하지 않는다. **모른 채로 두는 것과 정하고 두는 것은 다르므로** 케이스 표에
같은 이름의 시험으로 고정해 두었다.

- **Setext 헤딩** (`제목` 다음 줄에 `===`): 헤딩으로 세지 않는다. 이 저장소는 ATX 만 쓴다
- **참조 스타일 링크 사용처** (`[글][표]`): 정의(`[표]: 경로`)는 검사하고 사용처는 세지 않는다
- **선행 파이프 없는 GFM 표**: 표 행으로 세지 않는다. 이 저장소는 선행 파이프를 쓴다
- **닫히지 않은 코드 펜스**: 파일 끝까지 코드로 본다 (CommonMark 와 같다)

**그림(`![글](경로)`)은 링크와 똑같이 다룬다.** 일부러 그렇게 뒀다. 깨진 그림도 깨진 문서이고,
미러 한쪽에만 그림이 있으면 그것도 미러가 어긋난 것이다. 빼려면 여기와 케이스를 같이 고친다.

사용:

    python tools/docgate/docgate.py            # 저장소 루트를 자동 탐색
    python tools/docgate/docgate.py --claims   # 주장 문구 전체 목록까지
    python tools/docgate/docgate.py --root .   # 루트 지정

마지막 줄은 항상 `VERDICT: pass` 또는 `VERDICT: fail` 이고, 종료 코드가 같이 따라간다.
루트가 잘못되면 판정을 내지 않고 종료 코드 2로 끝난다. **판정할 수 없는 것을 통과로 적지
않는다** (design-audit 6장 규칙 7).
"""

from __future__ import annotations

import argparse
import re
import sys
import unicodedata
from dataclasses import dataclass, field
from pathlib import Path
from urllib.parse import unquote

KOR = "kor"
ENG = "eng"

# 끊긴 채로 두어도 되는 링크. 전체 경로로 적는다. 이름만으로 두면 깊이가 다른 엉뚱한
# 링크까지 통과한다. `experiments.md` 는 roadmap.md Phase 9 의 산출물이라 아직 없다.
ALLOWED_DANGLING = {
    f"docs/{KOR}/experiments.md",
    f"docs/{ENG}/experiments.md",
}

# 한국어만 두는 문서. 전체 경로로 적는다. 미러 짝이 없어도 결함으로 세지 않는다.
# `plan.md` 는 다음 세션 인수인계용이라 번역해 둘 이유가 없고, 번역이 늦으면 오히려 낡은
# 상태를 두 곳에 두게 된다. **eng 쪽에 같은 문서가 있으면 그것은 결함이다.** 예외는
# "없어도 된다" 이지 "있어도 된다" 가 아니다.
KOR_ONLY = {
    f"docs/{KOR}/plan.md",
}

# 파일 이름이 언어마다 다른 디렉터리. 여기서만 앞 번호로 짝을 맞춘다.
# 다른 곳까지 번호 짝을 허용하면 `2026-...` 같은 이름이 앞자리만 보고 엉뚱하게 붙는다.
NUMBER_PAIRED_DIRS = {(f"decisions",)}

# 언어별 강한 주장 문구. 판정이 아니라 목록용이다.
# 부정형(`미해결`, `unresolved`)과 더 긴 낱말(`install`) 은 세지 않는다.
CLAIM_PATTERNS = {
    KOR: [
        ("실측", r"(?<![가-힣])실측"),
        ("전부", r"(?<![가-힣])전부"),
        ("모두", r"(?<![가-힣])모두"),
        ("해결", r"(?<![가-힣])해결"),
        ("보장", r"(?<![가-힣])보장"),
        ("무조건", r"(?<![가-힣])무조건"),
        ("항상", r"(?<![가-힣])항상"),
        ("반드시", r"(?<![가-힣])반드시"),
        ("완료", r"(?<![가-힣])완료"),
        ("확정", r"(?<![가-힣])확정"),
    ],
    ENG: [
        ("measured", r"\bmeasured\b"),
        ("all", r"\ball\b"),
        ("every", r"\bevery\b"),
        ("resolved", r"\bresolved\b"),
        ("guarantee", r"\bguarantee[ds]?\b"),
        ("always", r"\balways\b"),
        ("never", r"\bnever\b"),
        ("must", r"\bmust\b"),
        ("complete", r"\bcomplete[ds]?\b"),
        ("confirmed", r"\bconfirmed\b"),
    ],
}

FENCE = re.compile(r"^\s{0,3}(```+|~~~+)")
HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
TABLE_ROW = re.compile(r"^\s{0,3}\|")
REFERENCE_DEFINITION = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*(.*)$")
EXTERNAL = re.compile(r"^[a-zA-Z][a-zA-Z0-9+.-]*:")
DRIVE_OR_ROOT = re.compile(r"^([A-Za-z]:|[/\\])")
ADR_NUMBER = re.compile(r"^(\d{1,4})-")
DATE_NAME = re.compile(r"^\d{4}-\d{2}-\d{2}")
HTML_COMMENT_OPEN = re.compile(r"<!--")
HTML_COMMENT_CLOSE = re.compile(r"-->")


def _code_span_end(line: str, index: int) -> int | None:
    """`index` 의 백틱 run 이 여는 코드 스팬이면 그 끝의 다음 위치. 아니면 None."""
    length = len(line)
    run = 0
    while index + run < length and line[index + run] == "`":
        run += 1
    closing = line.find("`" * run, index + run)
    while closing != -1 and closing + run < length and line[closing + run] == "`":
        closing = line.find("`" * run, closing + run)
    return None if closing == -1 else closing + run


def _strip_html_comments(line: str, in_comment: bool) -> tuple[str, bool]:
    """HTML 주석을 지운 줄과 주석이 이어지는지를 낸다.

    두 가지를 함께 본다.

    - 한 줄 안에서 열고 닫는 주석(`<!-- 글 -->`)도 지운다. 여러 줄 주석만 건너뛰면
      `<!-- [초안](없는.md) -->` 같은 줄이 본문으로 남아 **정상 문서가 떨어진다** (규칙 3).
    - 코드 스팬 안의 주석 기호는 글자일 뿐이다. 이것을 주석 시작으로 읽으면 뒤의 문서
      전체가 주석으로 묻혀 **결함 있는 저장소가 통과한다.** 이 파일 자신이 그런 예를 쓴다.
    """
    out = []
    index = 0
    length = len(line)
    while index < length:
        if in_comment:
            close = line.find("-->", index)
            if close == -1:
                return "".join(out), True
            index = close + 3
            in_comment = False
            continue
        if line[index] == "`":
            span_end = _code_span_end(line, index)
            if span_end is not None:
                out.append(line[index:span_end])
                index = span_end
                continue
        if line.startswith("<!--", index):
            index += 4
            in_comment = True
            continue
        out.append(line[index])
        index += 1
    return "".join(out), in_comment


def _scan(text: str):
    """(줄번호, 내용, 종류) 를 낸다. 종류는 `body`, `fence_open`, `fence_close` 다.

    본문이 아닌 곳은 낳지 않는다.

    - 코드 펜스 안 (여는 펜스와 같은 문자·같은 길이 이상이어야 닫힌다)
    - 4칸 이상 들여쓴 코드 블록 (빈 줄 다음에 시작한다)
    - HTML 주석 안 (한 줄이든 여러 줄이든)
    - 파일 첫머리의 YAML front matter

    펜스 안의 `#` 를 헤딩으로 세는 것이 CLAUDE.md 가 쓰던 `grep -c '^#'` 의 결함이다.
    나머지 셋도 같은 종류의 구멍이라 함께 막는다. **세는 쪽은 전부 이 한 곳을 쓴다.**
    코드 블록 수만 따로 세면 주석 안의 펜스가 한쪽에서만 잡혀 정상 문서가 떨어진다.
    """
    lines = text.splitlines()
    index = 0

    # YAML front matter: 첫 줄이 `---` 일 때만.
    if lines and lines[0].strip() == "---":
        for cursor in range(1, len(lines)):
            if lines[cursor].strip() in ("---", "..."):
                index = cursor + 1
                break
        else:
            index = len(lines)

    open_fence: str | None = None
    in_comment = False
    # 들여쓴 코드 블록은 **문단을 끊지 못한다** (CommonMark). 그래서 "앞이 빈 줄인가" 가
    # 아니라 "앞이 문단인가" 를 본다. 헤딩이나 펜스 바로 다음의 들여쓴 줄은 코드다.
    in_paragraph = False
    in_indented_code = False

    for cursor in range(index, len(lines)):
        line = lines[cursor]
        number = cursor + 1
        blank = not line.strip()

        if open_fence is None:
            line, in_comment = _strip_html_comments(line, in_comment)
            blank = not line.strip()
            if in_comment and blank:
                in_paragraph = False
                continue

        match = FENCE.match(line)
        if match:
            marker = match.group(1)
            if open_fence is None:
                open_fence = marker
                yield number, line, "fence_open"
            elif marker[0] == open_fence[0] and len(marker) >= len(open_fence):
                open_fence = None
                yield number, line, "fence_close"
            in_paragraph = False
            in_indented_code = False
            continue

        if open_fence is not None:
            in_paragraph = False
            continue

        # 들여쓴 코드 블록. 빈 줄 다음의 4칸 들여쓰기에서 시작해 빈 줄이 아닌 덜 들여쓴
        # 줄에서 끝난다. 표 안의 이어지는 줄을 코드로 오인하지 않으려고 시작 조건에
        # `previous_blank` 를 건다.
        indented = line.startswith("    ") or line.startswith("\t")
        if in_indented_code:
            if blank or indented:
                continue
            in_indented_code = False
        elif indented and not in_paragraph:
            in_indented_code = True
            continue

        if blank:
            in_paragraph = False
            continue
        in_paragraph = not HEADING.match(line)
        yield number, line, "body"


def _body_lines(text: str):
    """본문 줄만 (1-기반 줄번호, 내용) 으로 낸다."""
    for number, line, kind in _scan(text):
        if kind == "body":
            yield number, line


def headings(text: str) -> list[tuple[int, str]]:
    """(레벨, 제목) 목록. ATX 헤딩만 센다. Setext 는 세지 않는다 (위 주석 참고)."""
    found = []
    for _, line in _body_lines(text):
        match = HEADING.match(line)
        if match:
            found.append((len(match.group(1)), match.group(2)))
    return found


def _reference_destination(rest: str) -> str:
    """참조 정의(`[표]: 경로`)의 목적지. 꺾쇠 안의 공백 있는 경로도 읽는다."""
    rest = rest.strip()
    if rest.startswith("<"):
        end = rest.find(">")
        if end != -1:
            return rest[1:end]
    return rest.split()[0] if rest.split() else ""


def strip_code_spans(line: str) -> str:
    """인라인 코드 스팬(백틱)을 지운다.

    문서가 링크 문법 자체를 예로 들 때 그 예가 진짜 링크로 잡히면 **정상 문서가 떨어진다**
    (규칙 3). 여는 백틱과 같은 길이의 백틱으로 닫는다 (CommonMark).
    """
    out = []
    index = 0
    length = len(line)
    while index < length:
        if line[index] == "`":
            span_end = _code_span_end(line, index)
            if span_end is not None:
                index = span_end
                continue
            run = 0
            while index + run < length and line[index + run] == "`":
                run += 1
            out.append(line[index:index + run])
            index += run
            continue
        out.append(line[index])
        index += 1
    return "".join(out)


def inline_links(line: str) -> list[str]:
    """한 줄에서 인라인 링크의 목적지만 뽑는다.

    정규식으로는 두 가지를 놓친다. 링크 글에 대괄호가 겹쳐 있는 `[본 [초안]](경로)` 와
    꺾쇠로 감싼 공백 있는 경로 `[글](<빈 칸.md>)` 이다. 둘 다 **조용히 검사에서 빠져**
    끊긴 링크가 통과한다. 그래서 직접 훑는다.
    """
    targets: list[str] = []
    index = 0
    length = len(line)
    while index < length:
        if line[index] != "[":
            index += 1
            continue
        if index > 0 and line[index - 1] == "\\":
            index += 1
            continue

        depth = 0
        cursor = index
        while cursor < length:
            if line[cursor] == "\\":
                cursor += 2
                continue
            if line[cursor] == "[":
                depth += 1
            elif line[cursor] == "]":
                depth -= 1
                if depth == 0:
                    break
            cursor += 1
        if cursor >= length or depth != 0 or cursor + 1 >= length or line[cursor + 1] != "(":
            index += 1
            continue

        cursor += 2
        while cursor < length and line[cursor] in " \t":
            cursor += 1
        if cursor < length and line[cursor] == "<":
            end = line.find(">", cursor + 1)
            if end == -1:
                index += 1
                continue
            targets.append(line[cursor + 1:end])
            index = end + 1
            continue

        start = cursor
        while cursor < length and line[cursor] not in " \t)":
            cursor += 1
        targets.append(line[start:cursor])
        index = cursor if cursor > index else index + 1
    return targets


def local_links(text: str) -> list[tuple[int, str]]:
    """(줄번호, 상대 경로) 목록. 외부 URL 과 앵커 전용 링크는 뺀다.

    빈 링크 `[글]()` 도 빈 문자열로 담는다. 조용히 버리면 끊긴 링크가 통과한다.
    """
    found = []
    for number, line in _body_lines(text):
        targets = inline_links(strip_code_spans(line))
        definition = REFERENCE_DEFINITION.match(line)
        if definition:
            targets.append(_reference_destination(definition.group(1)))
        for target in targets:
            # 드라이브 문자(`C:/...`)를 URL 스킴으로 보면 안 된다. 외부 링크로 넘겨 버리면
            # 절대 경로 검사에 닿지 않고 조용히 통과한다.
            if target.startswith("#") or (EXTERNAL.match(target) and not DRIVE_OR_ROOT.match(target)):
                continue
            # 조각(`#절`)과 질의 문자열(`?plain=1`)은 파일 이름이 아니다.
            path = re.split(r"[#?]", target, maxsplit=1)[0]
            found.append((number, unquote(path)))
    return found


def table_rows(text: str) -> int:
    return sum(1 for _, line in _body_lines(text) if TABLE_ROW.match(line))


def code_blocks(text: str) -> int:
    """여는 펜스의 개수. 본문 판정과 같은 훑기를 쓴다."""
    return sum(1 for _, _, kind in _scan(text) if kind == "fence_open")


def resolve_link(base_dir: Path, target: str, root: Path) -> str | None:
    """링크를 실제 파일 이름과 대조한다. 통과면 None, 아니면 이유를 낸다.

    운영체제의 경로 해석에 기대지 않고 디렉터리 항목 이름과 직접 비교한다. 그래야
    **Windows 에서 통과하고 Linux 에서 깨지는 대소문자 차이**를 여기서 잡는다. 한글 이름의
    정규화 형태(NFC/NFD)도 맞춰서 비교한다.
    """
    if target == "":
        return "빈 링크"
    if DRIVE_OR_ROOT.match(target):
        return f"절대 경로는 쓰지 않는다: {target}"
    if "\\" in target:
        return f"역슬래시는 쓰지 않는다: {target}"

    current = base_dir
    for part in target.split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            current = current.parent
            if root not in current.parents and current != root:
                return f"저장소 밖으로 나간다: {target}"
            continue
        if not current.is_dir():
            return f"끊긴 링크: {target}"
        wanted = unicodedata.normalize("NFC", part)
        for entry in current.iterdir():
            if unicodedata.normalize("NFC", entry.name) == wanted:
                current = entry
                break
        else:
            return f"끊긴 링크: {target}"
    return None


@dataclass
class Finding:
    check: str
    where: str
    detail: str

    def line(self) -> str:
        return f"{self.check} | {self.where} | {self.detail}"

    def key(self) -> tuple[str, str, str]:
        return (self.check, self.where, self.detail)


@dataclass
class Report:
    findings: list[Finding] = field(default_factory=list)
    claims: list[tuple[str, int, str]] = field(default_factory=list)
    pairs: int = 0
    links_checked: int = 0

    @property
    def ok(self) -> bool:
        return not self.findings

    def text(self, show_claims: bool = False) -> str:
        out = [f"SUMMARY: pairs={self.pairs} links={self.links_checked} "
               f"findings={len(self.findings)} claims={len(self.claims)}"]
        if self.claims:
            counted: dict[str, int] = {}
            for _, _, phrase in self.claims:
                counted[phrase] = counted.get(phrase, 0) + 1
            summary = ", ".join(f"{phrase} {count}"
                                for phrase, count in sorted(counted.items(), key=lambda item: (-item[1], item[0])))
            out.append(f"주장 문구 (판정 아님): {summary}")
            if show_claims:
                out += [f"claim | {where}:{number} | {phrase}" for where, number, phrase in self.claims]
        out += [finding.line() for finding in sorted(self.findings, key=Finding.key)]
        out.append(f"VERDICT: {'pass' if self.ok else 'fail'}")
        return "\n".join(out)


class GateInputError(Exception):
    """판정할 수 없는 입력. 통과로 적지 않고 따로 끝낸다."""


def _norm(relative: Path) -> tuple[tuple[str, ...], str]:
    """(디렉터리, 파일 이름) 을 NFC 로 맞춘다.

    macOS 와 Windows 가 한글 파일 이름을 다른 정규화 형태로 저장하므로 비교 전에 맞춘다.
    """
    parts = tuple(unicodedata.normalize("NFC", part) for part in relative.parts[:-1])
    return parts, unicodedata.normalize("NFC", relative.name)


def _number_key(directory: tuple[str, ...], name: str) -> tuple[str, ...] | None:
    """`0002-...md` 형태의 일련번호 열쇠. 쓸 수 없으면 None.

    `NUMBER_PAIRED_DIRS` 안에서만, 그리고 날짜가 아닌 이름에만 쓴다. 날짜를 허용하면
    `2026-09-10-가.md` 와 `2026-09-14-나.md` 가 앞자리 `2026` 으로 붙어 **한쪽에만 있는
    기록 두 건이 짝 하나로 보인다.** 날짜 이름은 언어가 같으므로 이름 짝으로 이미 붙는다.
    """
    if directory not in NUMBER_PAIRED_DIRS or DATE_NAME.match(name):
        return None
    match = ADR_NUMBER.match(name)
    return (*directory, "#" + match.group(1)) if match else None


def _collect(docs_root: Path) -> list[tuple[tuple[str, ...], str, Path]]:
    collected = []
    for path in sorted(docs_root.rglob("*.md")):
        directory, name = _norm(path.relative_to(docs_root))
        collected.append((directory, name, path))
    return collected


def pair_documents(
    kor_files: list[tuple[tuple[str, ...], str, Path]],
    eng_files: list[tuple[tuple[str, ...], str, Path]],
) -> tuple[list[tuple[Path, Path]], list[Path], list[Path]]:
    """두 단계로 짝을 맞춘다.

    1. 같은 디렉터리의 같은 파일 이름끼리. 대부분의 문서가 여기서 붙는다.
    2. 남은 것끼리 `NUMBER_PAIRED_DIRS` 안에서만 앞 번호로. `decisions/` 는 이름이
       언어마다 다르기 때문이다.

    한 단계 열쇠를 쓰면 `commit_history/2026-...` 처럼 앞자리가 같은 파일이 전부 한 열쇠로
    뭉쳐 조용히 사라진다. 그래서 이름 짝을 먼저 쓰고, 번호 짝은 남은 것에만 쓰며,
    번호가 겹치면 짝으로 인정하지 않고 미러 결함으로 올린다.
    """
    eng_by_name = {(directory, name): path for directory, name, path in eng_files}
    pairs: list[tuple[Path, Path]] = []
    matched_eng: set[Path] = set()
    for directory, name, kor_path in kor_files:
        eng_path = eng_by_name.get((directory, name))
        if eng_path is not None and eng_path not in matched_eng:
            pairs.append((kor_path, eng_path))
            matched_eng.add(eng_path)
    matched_kor = {kor for kor, _ in pairs}

    def by_number(entries):
        grouped: dict[tuple[str, ...], list[Path]] = {}
        for directory, name, path in entries:
            key = _number_key(directory, name)
            if key is not None:
                grouped.setdefault(key, []).append(path)
        # 번호가 겹치는 쪽은 짝을 확정할 수 없으므로 후보에서 뺀다.
        return {key: paths[0] for key, paths in grouped.items() if len(paths) == 1}

    kor_numbered = by_number([e for e in kor_files if e[2] not in matched_kor])
    eng_numbered = by_number([e for e in eng_files if e[2] not in matched_eng])
    for key, kor_path in kor_numbered.items():
        eng_path = eng_numbered.get(key)
        if eng_path is not None:
            pairs.append((kor_path, eng_path))
            matched_kor.add(kor_path)
            matched_eng.add(eng_path)

    unmatched_kor = [path for _, _, path in kor_files if path not in matched_kor]
    unmatched_eng = [path for _, _, path in eng_files if path not in matched_eng]
    return pairs, unmatched_kor, unmatched_eng


def find_root(start: Path) -> Path:
    for candidate in [start, *start.parents]:
        if (candidate / "docs" / KOR).is_dir() and (candidate / "docs" / ENG).is_dir():
            return candidate
    raise GateInputError(f"docs/{KOR} 와 docs/{ENG} 를 가진 상위 디렉터리를 찾지 못했다: {start}")


def run(root: Path) -> Report:
    root = Path(root).resolve()
    for language in (KOR, ENG):
        if not (root / "docs" / language).is_dir():
            raise GateInputError(f"검사할 디렉터리가 없다: {root / 'docs' / language}")

    report = Report()
    kor_files = _collect(root / "docs" / KOR)
    eng_files = _collect(root / "docs" / ENG)
    pairs, unmatched_kor, unmatched_eng = pair_documents(kor_files, eng_files)

    def _rel(path: Path) -> str:
        return str(path.relative_to(root)).replace("\\", "/")

    # 한국어 전용 문서는 eng 쪽에 **있으면** 안 된다. 없는 것만 봐주는 예외다.
    for kor_path, eng_path in pairs:
        if _rel(kor_path) in KOR_ONLY:
            report.findings.append(Finding(
                "mirror", _rel(eng_path), f"한국어 전용 문서인데 {ENG} 쪽에 파일이 있다"))

    for path, other in [(p, ENG) for p in unmatched_kor if _rel(p) not in KOR_ONLY] + \
                       [(p, KOR) for p in unmatched_eng]:
        report.findings.append(Finding("mirror", _rel(path), f"{other} 쪽에 짝이 없다"))

    for kor_path, eng_path in pairs:
        report.pairs += 1
        kor_text = kor_path.read_text(encoding="utf-8-sig")
        eng_text = eng_path.read_text(encoding="utf-8-sig")
        where = (f"{kor_path.relative_to(root)} <-> {eng_path.relative_to(root)}").replace("\\", "/")

        kor_levels = [level for level, _ in headings(kor_text)]
        eng_levels = [level for level, _ in headings(eng_text)]
        if kor_levels != eng_levels:
            report.findings.append(Finding(
                "parity", where,
                f"헤딩 구조가 다르다. kor={len(kor_levels)}개 {kor_levels} "
                f"eng={len(eng_levels)}개 {eng_levels}"))

        for label, measure in (("표 행", table_rows), ("코드 블록", code_blocks),
                               ("링크", lambda text: len(local_links(text)))):
            left, right = measure(kor_text), measure(eng_text)
            if left != right:
                report.findings.append(Finding(
                    "parity", where, f"{label} 수가 다르다. kor={left} eng={right}"))

    for language in (KOR, ENG):
        for path in sorted((root / "docs" / language).rglob("*.md")):
            text = path.read_text(encoding="utf-8-sig")
            where = str(path.relative_to(root)).replace("\\", "/")
            for number, target in local_links(text):
                report.links_checked += 1
                problem = resolve_link(path.parent, target, root)
                if problem is None:
                    continue
                if problem.startswith("끊긴 링크") and _relative_to_root(path.parent, target, root) in ALLOWED_DANGLING:
                    continue
                report.findings.append(Finding("link", f"{where}:{number}", problem))
            for number, line in _body_lines(text):
                for phrase, pattern in CLAIM_PATTERNS[language]:
                    if re.search(pattern, line):
                        report.claims.append((where, number, phrase))

    return report


def _relative_to_root(base_dir: Path, target: str, root: Path) -> str:
    """예외 목록과 대조할 저장소 기준 경로. 대조할 수 없으면 빈 문자열."""
    try:
        return (base_dir / target).resolve().relative_to(root).as_posix()
    except (ValueError, OSError):
        return ""


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="문서 미러·링크·주장 게이트")
    parser.add_argument("--root", default=None, help="저장소 루트. 생략하면 위로 올라가며 찾는다")
    parser.add_argument("--claims", action="store_true", help="주장 문구 목록을 전부 낸다")
    args = parser.parse_args(argv)

    try:
        root = Path(args.root).resolve() if args.root else find_root(Path(__file__).resolve().parent)
        report = run(root)
    except GateInputError as error:
        print(f"INPUT ERROR: {error}", file=sys.stderr)
        return 2

    print(report.text(show_claims=args.claims))
    return 0 if report.ok else 1


if __name__ == "__main__":
    sys.exit(main())
