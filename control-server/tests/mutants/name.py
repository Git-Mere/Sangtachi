"""2.7 name 케이스 표의 변이. 실행기는 tests/mutate.py 다."""

P = "controlplane/ids.py"
T = "tests/test_name.py::test_name"
K = "tests/test_name.py::test_name_key"

CHARSET = "if any(ch not in NAME_ALPHABET for ch in value):"


def k(*ids):
    return [f"{T}[{i}]" for i in ids]


MUTANTS = [
    dict(id="name-no-charset", path=P, old=CHARSET, new="if False:",
         kills=k("inner-space", "leading-space", "trailing-space", "hyphen", "hangul", "fullwidth-digits",
                 "trailing-newline", "control-char", "tab", "kelvin-sign", "latin-e-acute", "arabic-indic-digit",
                 "del-char"),
         why="문자 집합 검사를 지운다 (roadmap Phase 4 검증의 변이)"),
    dict(id="name-isalnum", path=P, old=CHARSET, new="if not value.isalnum():",
         kills=k("underscore-only", "hangul", "fullwidth-digits", "latin-e-acute", "arabic-indic-digit"),
         why="63자 집합 대신 str.isalnum() 을 쓴다. 밑줄을 거부하고 유니코드 글자를 받는다 (roadmap Phase 4 검증의 변이)"),
    dict(id="name-regex-w", path=P, old=CHARSET,
         new='if __import__("re").fullmatch(r"\\w*", value) is None:',
         kills=k("hangul", "fullwidth-digits", "latin-e-acute", "arabic-indic-digit"),
         why="63자 집합 대신 정규식 \\w 를 쓴다. 유니코드 글자를 받는다 (roadmap Phase 4 검증의 변이)"),
    dict(id="name-regex-dollar", path=P, old=CHARSET,
         new='if __import__("re").match(r"[A-Za-z0-9_]*$", value) is None:',
         kills=k("trailing-newline"),
         why="문자 집합을 ASCII 로 맞게 적고 끝을 $ 로 잡는다. $ 가 끝 줄바꿈 앞에서도 맞는다"),
    dict(id="name-lower-before-charset", path=P, old=CHARSET,
         new="if any(ch not in NAME_ALPHABET for ch in value.lower()):",
         kills=k("kelvin-sign"), why="소문자 변환을 문자 집합 검사보다 먼저 한다 (2.7 의 순서를 뒤집는다)"),
    dict(id="name-strip", path=P, old="    if not isinstance(value, str):\n        raise OpError(\"bad_request\", \"name",
         new="    if isinstance(value, str):\n        value = value.strip()\n    if not isinstance(value, str):\n        raise OpError(\"bad_request\", \"name",
         kills=k("leading-space", "trailing-space", "trailing-newline"), why="앞뒤 공백을 지워 준다"),
    dict(id="name-no-type", path=P, old="    if not isinstance(value, str):\n        raise OpError(\"bad_request\", \"name",
         new="    if False:\n        raise OpError(\"bad_request\", \"name",
         kills=k("not-string-number", "not-string-null", "not-string-bool", "missing"),
         why="문자열 형 검사를 지운다. 문자열이 아닌 값이 OpError 가 아닌 예외로 떨어진다"),
    dict(id="name-len-no-min", path=P, old="if not 1 <= len(value) <= MAX_NAME_LEN:",
         new="if not len(value) <= MAX_NAME_LEN:", kills=k("empty"), why="길이 하한을 지운다"),
    dict(id="name-len-no-max", path=P, old="if not 1 <= len(value) <= MAX_NAME_LEN:",
         new="if not 1 <= len(value):", kills=k("seventeen-chars"), why="길이 상한을 지운다"),
    dict(id="name-len-exclusive", path=P, old="if not 1 <= len(value) <= MAX_NAME_LEN:",
         new="if not 1 <= len(value) < MAX_NAME_LEN:", kills=k("sixteen-chars", "sixteen-underscores-and-digits"),
         why="상한을 >= 로 잘못 쓴다"),
    dict(id="name-case-fold-stored", path=P, old="        raise OpError(\"bad_request\", \"name 은 1자 이상 16자 이하다\")\n    return value",
         new="        raise OpError(\"bad_request\", \"name 은 1자 이상 16자 이하다\")\n    return value.lower()",
         kills=k("sixteen-chars", "mixed-case"), why="대소문자를 등록한 그대로 두지 않고 바꾼다"),
    dict(id="name-key-no-lower", path=P, old="    return name.lower()", new="    return name",
         kills=[f"{K}[upper]", f"{K}[mixed]"], why="비교 키를 소문자로 바꾸지 않는다. 대소문자 무시 유일성이 깨진다"),
]
