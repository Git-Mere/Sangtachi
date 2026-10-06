"""2.1 room_id 케이스 표와 2.2 ~ 2.4 생성·형식 시험의 변이. 실행기는 tests/mutate.py 다."""

P = "controlplane/ids.py"
T = "tests/test_room_id.py::test_room_id"
N = "tests/test_room_id.py::test_hex32_field"


def k(*ids):
    return [f"{T}[{i}]" for i in ids]


def kn(*ids, fields=("client_nonce", "peer_token")):
    return [f"{N}[{i}-{f}]" for i in ids for f in fields]


CHECK = "if not isinstance(value, str) or _LOWER_HEX32.fullmatch(value) is None:\n        raise OpError(\"bad_request\", \"{}"
NONCE_CHECK = CHECK.format("client_nonce")
TOKEN_CHECK = CHECK.format("peer_token")


MUTANTS = [
    dict(id="room-no-upper", path=P, old="upper = value.upper()", new="upper = value",
         kills=k("lower", "mixed-case"), why="대소문자 무시(대문자로 바꾼 뒤 검사)를 지운다"),
    dict(id="room-strip", path=P, old="upper = value.upper()", new="upper = value.strip().upper()",
         kills=k("leading-space"), why="공백을 지워 주지 않는다는 규칙을 어긴다"),
    dict(id="room-len-only-min", path=P, old="if len(upper) != ROOM_ID_LEN:", new="if len(upper) < ROOM_ID_LEN:",
         kills=k("len-7"), why="길이 상한을 지운다"),
    dict(id="room-len-only-max", path=P, old="if len(upper) != ROOM_ID_LEN:", new="if len(upper) > ROOM_ID_LEN:",
         kills=k("len-5", "empty"), why="길이 하한을 지운다"),
    dict(id="room-alphabet-0", path=P, old='ROOM_ID_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"',
         new='ROOM_ID_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ234567890"',
         kills=k("digit-zero"), why="알파벳에 0 을 넣는다"),
    dict(id="room-alphabet-O", path=P, old='ROOM_ID_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"',
         new='ROOM_ID_ALPHABET = "ABCDEFGHJKLMNOPQRSTUVWXYZ23456789"',
         kills=k("letter-o"), why="알파벳에 O 를 넣는다"),
    dict(id="room-alphabet-1", path=P, old='ROOM_ID_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"',
         new='ROOM_ID_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ123456789"',
         kills=k("digit-one"), why="알파벳에 1 을 넣는다"),
    dict(id="room-alphabet-I", path=P, old='ROOM_ID_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"',
         new='ROOM_ID_ALPHABET = "ABCDEFGHIJKLMNPQRSTUVWXYZ23456789"',
         kills=k("letter-i"), why="알파벳에 I 를 넣는다"),
    dict(id="room-lookalike", path=P, old="upper = value.upper()",
         new='upper = value.upper().replace("0", "D").replace("O", "D").replace("1", "L").replace("I", "L")',
         kills=k("digit-zero", "letter-o", "digit-one", "letter-i"),
         why="닮은 글자를 알파벳 안의 글자로 바꿔 해석한다"),
    dict(id="room-alphabet-isalnum", path=P, old="if any(ch not in ROOM_ID_ALPHABET for ch in upper):",
         new="if not upper.isalnum():",
         kills=k("digit-zero", "letter-o", "digit-one", "letter-i"),
         why="알파벳 검사를 영숫자 검사로 바꾼다"),
    dict(id="room-no-ascii", path=P, old="if not value.isascii():", new="if False:",
         kills=k("long-s"), why="ASCII 검사를 지운다. 전각 행은 알파벳 검사가 따로 막으므로 long-s 행이 지킨다"),
    dict(id="room-width-fold", path=P, old="if not value.isascii():",
         new='value = __import__("unicodedata").normalize("NFKC", value)\n    if not value.isascii():',
         kills=k("fullwidth"), why="전각을 반각으로 접어 받아 준다. ASCII 만 받는다는 규칙을 어긴다"),
    dict(id="room-no-type", path=P, old="if not isinstance(value, str):", new="if False:",
         kills=k("missing", "not-string"), why="문자열 형 검사를 지운다. OpError 가 아닌 예외로 떨어진다"),
    dict(id="hex32-case-insensitive", path=P, old='_LOWER_HEX32 = re.compile(r"[0-9a-f]{32}")',
         new='_LOWER_HEX32 = re.compile(r"[0-9a-fA-F]{32}")', kills=kn("upper"),
         why="2.3/2.4 의 소문자 규칙을 두 필드에서 지운다"),
    dict(id="hex32-unicode-digit", path=P, old='_LOWER_HEX32 = re.compile(r"[0-9a-f]{32}")',
         new='_LOWER_HEX32 = re.compile(r"[\\da-f]{32}")', kills=kn("fullwidth-digit"),
         why="[0-9] 를 유니코드 숫자를 받는 \\d 로 바꾼다"),
    dict(id="hex32-len", path=P, old='_LOWER_HEX32 = re.compile(r"[0-9a-f]{32}")',
         new='_LOWER_HEX32 = re.compile(r"[0-9a-f]{31,32}")', kills=kn("len-31"), why="길이 하한을 지운다"),
    dict(id="hex32-non-hex", path=P, old='_LOWER_HEX32 = re.compile(r"[0-9a-f]{32}")',
         new='_LOWER_HEX32 = re.compile(r"[0-9a-g]{32}")', kills=kn("non-hex"), why="16진수 밖 글자를 받는다"),
    dict(id="nonce-search", path=P, old=NONCE_CHECK, new=NONCE_CHECK.replace(".fullmatch(", ".match("),
         kills=kn("len-33", "trailing-newline", fields=("client_nonce",)), why="client_nonce 의 전체 일치를 앞부분 일치로 바꾼다"),
    dict(id="nonce-no-type", path=P, old=NONCE_CHECK, new=NONCE_CHECK.replace("not isinstance(value, str) or ", ""),
         kills=kn("not-string", "missing", fields=("client_nonce",)), why="client_nonce 의 문자열 형 검사를 지운다"),
    dict(id="token-search", path=P, old=TOKEN_CHECK, new=TOKEN_CHECK.replace(".fullmatch(", ".match("),
         kills=kn("len-33", "trailing-newline", fields=("peer_token",)), why="peer_token 의 전체 일치를 앞부분 일치로 바꾼다"),
    dict(id="token-no-type", path=P, old=TOKEN_CHECK, new=TOKEN_CHECK.replace("not isinstance(value, str) or ", ""),
         kills=kn("not-string", "missing", fields=("peer_token",)), why="peer_token 의 문자열 형 검사를 지운다"),
    dict(id="token-lowercased", path=P, old=TOKEN_CHECK,
         new=TOKEN_CHECK.replace("_LOWER_HEX32.fullmatch(value)", "_LOWER_HEX32.fullmatch(value.lower())"),
         kills=kn("upper", fields=("peer_token",)), why="peer_token 을 소문자로 바꿔 받아 준다. 요청에서도 소문자만 받는다는 결정을 어긴다"),
    dict(id="token-unchecked", path=P, old=TOKEN_CHECK, new="if False:\n        raise OpError(\"bad_request\", \"peer_token",
         kills=kn("upper", "len-31", "len-33", "non-hex", "trailing-newline", "fullwidth-digit", "not-string", "missing",
                  fields=("peer_token",)),
         why="peer_token 형식 검사를 지운다. 형식 위반이 저장소 비교까지 가서 unauthorized 가 된다"),
    dict(id="peer-id-allows-zero", path=P, old="if value != 0:", new="if True:",
         kills=["tests/test_room_id.py::test_new_peer_id_redraws_zero"], why="2.2 의 0 재추첨을 지운다"),
    dict(id="peer-token-short", path=P, old="return secrets.token_hex(16)", new="return secrets.token_hex(15)",
         kills=["tests/test_room_id.py::test_new_peer_token_format", "tests/test_room_id.py::test_new_peer_token_uses_draw"], why="2.3 의 128비트를 줄인다"),
    dict(id="room-gen-short", path=P, old="for _ in range(ROOM_ID_LEN))", new="for _ in range(ROOM_ID_LEN - 1))",
         kills=["tests/test_room_id.py::test_new_room_id_is_valid", "tests/test_room_id.py::test_new_room_id_uses_draws"], why="생성 길이를 줄인다"),
    dict(id="room-gen-fixed", path=P,
         old='return "".join(secrets.choice(ROOM_ID_ALPHABET) for _ in range(ROOM_ID_LEN))',
         new='return "ABCDEF"', kills=["tests/test_room_id.py::test_new_room_id_uses_draws"],
         why="room_id 를 뽑지 않고 고정값을 돌려준다"),
    dict(id="peer-id-fixed", path=P, old="        if value != 0:\n            return value",
         new="        if value != 0:\n            return 1", kills=["tests/test_room_id.py::test_new_peer_id_redraws_zero"],
         why="peer_id 를 뽑은 값이 아니라 고정값으로 돌려준다"),
    dict(id="token-fixed", path=P, old="return secrets.token_hex(16)", new='return "0" * 32',
         kills=["tests/test_room_id.py::test_new_peer_token_uses_draw"],
         why="peer_token 을 뽑지 않고 고정값을 돌려준다. 형식 시험은 통과한다"),
]
