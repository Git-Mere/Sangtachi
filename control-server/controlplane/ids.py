"""식별자 생성과 정규화. 출처는 control_plane.md 2.1 room_id, 2.2 peer_id, 2.3 peer_token,
2.4 client_nonce, 2.7 name 이다. 케이스 표는 tests/test_room_id.py 와 tests/test_name.py 에 있다.
"""

from __future__ import annotations

import re
import secrets
from controlplane.constants import MAX_NAME_LEN
from controlplane.errors import OpError

# 2.1. 32자. I, O, 0, 1 을 뺐다.
ROOM_ID_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
ROOM_ID_LEN = 6

# 2.3, 2.4. 소문자 16진수 32자. [0-9a-f] 는 ASCII 범위다. \d 를 쓰지 않는다 (유니코드 숫자를 받는다).
_LOWER_HEX32 = re.compile(r"[0-9a-f]{32}")

# 2.7. ASCII 영문자, 숫자, 밑줄 63자. 집합을 직접 대조한다. str.isalnum() 과 정규식 \w 는 유니코드 글자를
# 받고 $ 는 끝 줄바꿈 앞에서도 맞는다 (9장 "코드를 읽어서만 판정할 수 있는 것").
NAME_ALPHABET = frozenset("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_")


def normalize_room_id(value: object) -> str:
    """2.1 케이스 표. 대문자로 바꾼 뒤 알파벳 검사를 한다. 공백을 지우거나 닮은 글자로 바꾸지 않는다.

    필드가 없으면 부르는 쪽이 None 을 넘긴다. 문자열이 아닌 것은 전부 bad_request 다.
    """
    if not isinstance(value, str):
        raise OpError("bad_request", "room_id 는 문자열이어야 한다")
    # ASCII 검사가 대문자 변환보다 먼저다. 'ſ'(U+017F) 는 upper() 로 'S' 가 된다.
    if not value.isascii():
        raise OpError("bad_request", "room_id 는 ASCII 만 받는다")
    upper = value.upper()
    if len(upper) != ROOM_ID_LEN:
        raise OpError("bad_request", "room_id 는 6글자다")
    if any(ch not in ROOM_ID_ALPHABET for ch in upper):
        raise OpError("bad_request", "room_id 에 알파벳 밖의 글자가 있다")
    return upper


def check_client_nonce(value: object) -> str:
    """2.4. 요청에서도 소문자 16진수 32자만 받는다. 형식 위반은 bad_request 다 (7.3 의 1번, 저장소 전)."""
    if not isinstance(value, str) or _LOWER_HEX32.fullmatch(value) is None:
        raise OpError("bad_request", "client_nonce 는 소문자 16진수 32자다")
    return value


def check_peer_token(value: object) -> str:
    """2.3. 요청에서도 소문자 16진수 32자만 받는다. 형식 위반은 unauthorized 가 아니라 bad_request 다
    (7.3 의 1번, 저장소 전). 메시지에 값을 싣지 않는다 (4.1).
    """
    if not isinstance(value, str) or _LOWER_HEX32.fullmatch(value) is None:
        raise OpError("bad_request", "peer_token 은 소문자 16진수 32자다")
    return value


def check_name(value: object) -> str:
    """2.7 name. 문자열이고, 63자 집합 밖의 글자가 없고, 1자 이상 MAX_NAME_LEN 자 이하. 등록한 그대로 돌려준다.

    셋 다 bad_request 이고 저장소를 읽기 전에 본다 (7.3 의 1번). 앞뒤 공백을 지워 주지 않는다. 메시지에 값을
    싣지 않는다 (4.1). 글자가 전부 ASCII 이므로 글자 수와 바이트 수가 같다.
    """
    if not isinstance(value, str):
        raise OpError("bad_request", "name 은 문자열이어야 한다")
    # 문자 집합 검사가 소문자 변환(name_key)보다 먼저다. 'K'(U+212A) 는 lower() 로 'k' 가 된다.
    if any(ch not in NAME_ALPHABET for ch in value):
        raise OpError("bad_request", "name 에 허용 밖의 글자가 있다")
    if not 1 <= len(value) <= MAX_NAME_LEN:
        raise OpError("bad_request", "name 은 1자 이상 16자 이하다")
    return value


def name_key(name: str) -> str:
    """2.7 방 안 유일성의 비교 키. ASCII 소문자로 바꾼 값이다 (6.3 NAME#<이름 키>).

    check_name 을 통과한 값만 받는다. 그 값은 ASCII 이므로 lower() 가 ASCII 대문자만 바꾼다.
    """
    return name.lower()


def new_room_id() -> str:
    """2.1. CSPRNG 로 알파벳에서 6글자. 충돌 재추첨은 store 쪽 조건부 쓰기가 한다."""
    return "".join(secrets.choice(ROOM_ID_ALPHABET) for _ in range(ROOM_ID_LEN))


def new_peer_id() -> int:
    """2.2. secrets.randbits(32) 로 뽑고 0 이면 다시 뽑는다."""
    while True:
        value = secrets.randbits(32)
        if value != 0:
            return value


def new_peer_token() -> str:
    """2.3. 128비트 CSPRNG, 소문자 16진수 32자."""
    return secrets.token_hex(16)
