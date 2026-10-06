"""4.4 register_candidate 의 후보 형식 검사와 위생. 출처는 control_plane.md 4.4 와 그 거부 정책,
규칙 자체는 protocol.md 10.1 후보 수집과 위생이다. 케이스 표는 tests/test_candidates.py 에 있다.

ipaddress 모듈의 술어와 inet_aton 으로 판정하지 않는다 (control_plane.md 7.1). 옥텟 4개를 직접
파싱해 앞 옥텟과 값으로 가른다.
"""

from __future__ import annotations

import re

from controlplane.constants import MAX_CANDIDATES
from controlplane.errors import OpError

KINDS = ("local", "reflexive")

# 7.1 의 ^\d{1,3}(\.\d{1,3}){3}$ 을 ASCII 로 쓴 것이다. Python 의 \d 는 유니코드 숫자도 받는다.
# fullmatch 로 본다. $ 는 끝 줄바꿈 앞에서도 맞는다.
_DOTTED = re.compile(r"[0-9]{1,3}(?:\.[0-9]{1,3}){3}")


def parse_ipv4(text: object) -> tuple[int, int, int, int] | None:
    """점 십진 4옥텟. 옥텟마다 0~255, 선행 0 거부. 형식이 아니면 None."""
    if not isinstance(text, str) or _DOTTED.fullmatch(text) is None:
        return None
    octets = []
    for part in text.split("."):
        if len(part) > 1 and part.startswith("0"):
            return None
        value = int(part)
        if value > 255:
            return None
        octets.append(value)
    return (octets[0], octets[1], octets[2], octets[3])


def _is_json_int(value: object) -> bool:
    # JSON true/false 는 Python 에서 int 의 하위형(bool)이다. 3.3 인코딩의 "정수는 JSON number" 에
    # 따라 bool 은 정수가 아니다. 소수(51000.0)는 float 라 여기서 걸린다.
    return type(value) is int


def _check_shape(raw: object) -> list[tuple[tuple[int, int, int, int], dict]]:
    """형 위반이면 요청 전체가 bad_request 다 (4.4 거부 정책). (옥텟, 원소) 목록을 돌려준다."""
    if not isinstance(raw, list):
        raise OpError("bad_request", "candidates 는 배열이다")
    if len(raw) > MAX_CANDIDATES:
        raise OpError("bad_request", "후보가 너무 많다")
    # 빈 배열은 따로 보지 않는다. 아래 저장 0개 규칙이 같은 bad_request 를 낸다.
    out = []
    for item in raw:
        if not isinstance(item, dict):
            raise OpError("bad_request", "후보는 객체다")
        octets = parse_ipv4(item.get("ip"))
        if octets is None:
            raise OpError("bad_request", "ip 는 점 십진 IPv4 다")
        port = item.get("port")
        if not _is_json_int(port) or not 0 <= port <= 65535:
            raise OpError("bad_request", "port 는 1~65535 정수다")
        kind = item.get("kind")
        if not isinstance(kind, str) or kind not in KINDS:
            raise OpError("bad_request", "kind 는 local 이나 reflexive 다")
        out.append((octets, item))
    return out


def _sanitation_reject(octets: tuple[int, int, int, int], port: int) -> bool:
    """protocol.md 10.1 위생 규칙. 걸리면 그 후보만 버린다.

    브로드캐스트는 255.255.255.255 하나다. 서버는 피어의 prefix 를 받지 않아 directed broadcast 를
    가릴 수 없다. 미지정은 0.0.0.0 하나다 (10.1 이 그 값을 적었다).
    """
    first = octets[0]
    if octets == (255, 255, 255, 255):  # 브로드캐스트
        return True
    if 224 <= first <= 239:  # 멀티캐스트 224.0.0.0/4
        return True
    if octets == (0, 0, 0, 0):  # 미지정
        return True
    if first == 127:  # 루프백 127.0.0.0/8
        return True
    if port == 0:
        return True
    return False


def sanitize_candidates(raw: object) -> tuple[list[dict], int]:
    """(저장할 후보 목록, rejected) 를 돌려준다. accepted 는 목록 길이다.

    순서는 protocol.md 10.1 표의 순서다. 목록 전체의 형 검사 -> 후보마다 위생 거부 -> 남은 것의
    ip:port 중복 제거. 그래서 같은 위생 거부 후보가 두 번 오면 rejected 는 2 다. 중복이면 먼저 온
    것을 남긴다 (kind 도 먼저 온 것). 원소의 모르는 키는 버리고 ip/port/kind 만 저장한다.
    저장할 것이 0개면 bad_request 다.
    """
    checked = _check_shape(raw)
    stored: list[dict] = []
    seen: set[tuple[tuple[int, int, int, int], int]] = set()
    rejected = 0
    for octets, item in checked:
        port = item["port"]
        if _sanitation_reject(octets, port):
            rejected += 1
            continue
        key = (octets, port)
        if key in seen:
            continue
        seen.add(key)
        stored.append({"ip": item["ip"], "port": port, "kind": item["kind"]})
    if not stored:
        raise OpError("bad_request", "저장할 후보가 없다")
    return stored, rejected
