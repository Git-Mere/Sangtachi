#!/usr/bin/env python3
"""natprobe - NAT 매핑 거동과 UDP 홀펀칭 실측 도구.

docs/kor/plan.md "5. 직접 연결 불가 대비책" (design-audit blocker 20) 의 실측용.

표준 라이브러리만 쓴다. Python 3.8+. 대상은 Windows 10/11 x64지만 Linux/macOS에서도 돈다.

두 가지를 잰다.

  probe  STUN Binding 으로 매핑 거동만 잰다. 필터링 거동은 알 수 없다.
  punch  probe 를 먼저 하고, 같은 소켓으로 상대와 실제 홀펀칭을 시도한다.

probe 만으로 판단하면 안 된다. Binding 응답은 매핑 거동만 알려주고 필터링 거동은
알려주지 않는다. 홀펀칭 성립은 둘 다에 달려 있다.
"""

from __future__ import annotations

import argparse
import ipaddress
import json
import math
import os
import platform
import secrets
import select
import socket
import statistics
import struct
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

SCHEMA = "natprobe/1"


class QuietParser(argparse.ArgumentParser):
    """인자 오류 메시지에 인자 원문을 싣지 않는다.

    argparse 기본 동작은 잘못된 인자를 그대로 stderr 에 되쓴다. 인자에는 상대 공인
    주소가 들어올 수 있고(`--peer`, `check-peer`), 그것이 터미널 기록과 수집 로그에
    남는다. 사용법은 그대로 찍으므로 어떤 형태가 필요한지는 알 수 있다.
    """

    def error(self, message: str):  # noqa: D102
        self.print_usage(sys.stderr)
        sys.stderr.write(f"{self.prog}: 인자가 올바르지 않다. "
                         "자세한 값은 로그 노출을 막기 위해 생략한다\n")
        sys.exit(2)

# --- STUN (RFC 5389, docs/kor/protocol.md 13장 범위) -------------------------

STUN_BINDING_REQUEST = 0x0001
STUN_BINDING_SUCCESS = 0x0101
STUN_BINDING_ERROR = 0x0111
STUN_COOKIE = 0x2112A442
STUN_HEADER_LEN = 20

ATTR_MAPPED_ADDRESS = 0x0001
ATTR_ERROR_CODE = 0x0009
ATTR_XOR_MAPPED_ADDRESS = 0x0020

# protocol.md 11장 타이머표와 같은 값
STUN_RETRY_SCHEDULE = (0.0, 0.5, 1.5, 3.5)  # 첫 송신 + 500ms, 1s, 2s 간격 재시도
STUN_DEADLINE_S = 5.0

DEFAULT_STUN_SERVERS = [
    ("stun.l.google.com", 19302),
    ("stun1.l.google.com", 19302),
    ("stun.cloudflare.com", 3478),
    ("stun.nextcloud.com", 3478),
]

# --- 펀치 시험 패킷 ----------------------------------------------------------
# 터널 프로토콜(protocol.md 3장)과 무관한 별도 진단용 포맷이다.
# 상위 2비트가 01 인 첫 바이트('N' = 0x4E)라 STUN 과도 겹치지 않는다.

PUNCH_MAGIC = b"NATPRB2\x00"  # 포맷이 바뀌면 올린다. v1(24바이트)과 호환되지 않는다
PUNCH_FMT = ">8sBxxxIIQ"
PUNCH_LEN = struct.calcsize(PUNCH_FMT)  # 28
PUNCH_PING = 1
PUNCH_PONG = 2
PUNCH_UNSOL = 3      # 새 소켓에서 보내는 "요청하지 않은" 패킷
PUNCH_UNSOL_ACK = 4  # 그것을 받았다는 확인. 본 소켓에서 보낸다
PUNCH_PHASE_READY = 5  # 요청하지 않은 인바운드 단계에 들어갈 준비가 됐다는 신호
PUNCH_KINDS = (PUNCH_PING, PUNCH_PONG, PUNCH_UNSOL, PUNCH_UNSOL_ACK, PUNCH_PHASE_READY)
PUNCH_INTERVAL_S = 0.2  # protocol.md 의 HELLO 재전송 간격과 같다
PUNCH_DEFAULT_DURATION_S = 30
UNSOL_DEFAULT_DURATION_S = 10
UNSOL_INTERVAL_S = 0.5
UNSOL_SYNC_TIMEOUT_S = 15.0   # 상대가 단계에 들어오기를 기다리는 한도
UNSOL_SYNC_INTERVAL_S = 0.2


def setup_console() -> None:
    """콘솔 인코딩을 UTF-8 로 맞춘다.

    이 도구의 출력은 한국어다. Windows 에서 콘솔 코드페이지가 cp1252(영문 기본)면
    첫 print 에서 UnicodeEncodeError 로 죽는다. 출력을 파일로 넘길 때도 같은 일이 난다.
    측정 도중에 죽으면 30초를 다시 써야 하므로 시작할 때 막는다.

    `errors="replace"` 가 마지막 방어선이다. 글자가 깨질지언정 측정은 끝나고 JSON 은
    항상 UTF-8 로 정확히 저장된다.
    """
    if os.name == "nt":
        try:
            import ctypes

            ctypes.windll.kernel32.SetConsoleOutputCP(65001)
            ctypes.windll.kernel32.SetConsoleCP(65001)
        except Exception:
            pass  # 콘솔이 없거나 권한이 없으면 아래 reconfigure 로 충분하다

    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass  # Python 3.7 미만이거나 재설정할 수 없는 스트림


def _utcnow_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


# ---------------------------------------------------------------------------
# 소켓
# ---------------------------------------------------------------------------


SIO_UDP_CONNRESET = 0x9800000C

# 로컬 출발지로 인정하는 비공인 대역. `ipaddress.is_private` 를 그대로 쓰면 문서용
# (192.0.2.0/24 등)과 벤치마킹(198.18.0.0/15) 대역까지 "사설 NAT 주소" 로 보고하게 된다.
# 실제 단말에 붙을 수 있는 대역만 명시한다.
_PRIVATE_NETS = tuple(ipaddress.ip_network(n) for n in (
    "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16",
    "192.0.0.0/29",   # RFC 7335 464XLAT CLAT
    "fc00::/7",       # ULA
))
_SPECIAL_NETS = tuple(ipaddress.ip_network(n) for n in (
    "100.64.0.0/10",  # CGNAT
))

# `is_global` 이 참이지만 피어 엔드포인트가 될 수 없는 IANA 특수 목적 대역.
# 술어만으로는 걸러지지 않으므로 명시한다.
# **완전한 IANA 목록이 아니다.** 알려진 대역을 거부하는 최선 노력 목록이다. 새 특수
# 목적 대역이 할당되면 여기에 없어 통과할 수 있다. 계약은 "이 목록에 있는 것은 반드시
# 거부한다" 까지다.
_NOT_PEER_NETS = tuple(ipaddress.ip_network(n) for n in (
    "192.0.0.0/24",     # IETF Protocol Assignments 전체. 안에 PCP anycast(.9),
                        # NAT64/DNS64 discovery(.10), DS-Lite(.0/29) 등이 있고
                        # 일부는 is_global 이 참이다. 피어가 될 수 없으므로 통째로 막는다
    "192.88.99.0/24",   # 6to4 relay anycast (RFC 7526 폐기)
    "192.31.196.0/24",  # AS112-v4
    "192.52.193.0/24",  # AMT
    "192.175.48.0/24",  # AS112 direct delegation
    "64:ff9b::/96",     # NAT64 well-known prefix
    "64:ff9b:1::/48",   # NAT64 local-use
    "2001::/32",        # Teredo
    "2002::/16",        # 6to4
))

# SIO_UDP_CONNRESET off 를 실제로 적용했는지. main 이 기록에 넣는다.
_connreset_state = {"disabled": None, "detail": None}


def _disable_udp_connreset(sock: socket.socket) -> None:
    """Windows 에서 SIO_UDP_CONNRESET 을 끈다.

    **`socket.ioctl()` 로는 안 된다.** CPython 은 `SIO_RCVALL`,
    `SIO_KEEPALIVE_VALS`, `SIO_LOOPBACK_FAST_PATH` 세 개만 허용하고 나머지는
    `ValueError: invalid ioctl command` 로 막는다. `socket.SIO_UDP_CONNRESET`
    상수도 없다. 그래서 `ws2_32.dll` 의 `WSAIoctl` 을 직접 부른다.

    끄지 않으면 ICMP port unreachable 을 받은 뒤 `recvfrom` 이 `WSAECONNRESET` 로
    깨진다. 펀치 초반에는 상대가 아직 포트를 열기 전이라 이 ICMP 가 정상적으로 발생한다.
    """
    import ctypes
    from ctypes import wintypes

    # use_last_error 를 쓰지 않는다. 그것을 켜면 ctypes 가 호출 전후로 스레드의
    # last-error 값을 자기 것으로 바꿔치기해서, 뒤이어 부르는 WSAGetLastError 가
    # 엉뚱한 값을 돌려준다. 대신 WSAGetLastError 를 직접 부른다.
    ws2 = ctypes.WinDLL("ws2_32")

    # GetProcAddress 자체가 last-error 를 덮을 수 있으므로 호출 전에 미리 찾아 둔다.
    wsa_last_error = ws2.WSAGetLastError
    wsa_last_error.argtypes = []
    wsa_last_error.restype = ctypes.c_int

    wsa_ioctl = ws2.WSAIoctl
    wsa_ioctl.argtypes = [
        ctypes.c_void_p,                 # SOCKET (64비트에서 8바이트)
        wintypes.DWORD,                  # dwIoControlCode
        ctypes.c_void_p,                 # lpvInBuffer
        wintypes.DWORD,                  # cbInBuffer
        ctypes.c_void_p,                 # lpvOutBuffer
        wintypes.DWORD,                  # cbOutBuffer
        ctypes.POINTER(wintypes.DWORD),  # lpcbBytesReturned
        ctypes.c_void_p,                 # lpOverlapped
        ctypes.c_void_p,                 # lpCompletionRoutine
    ]
    wsa_ioctl.restype = ctypes.c_int

    value = wintypes.BOOL(False)  # FALSE = 이 동작을 끈다
    returned = wintypes.DWORD(0)
    rc = wsa_ioctl(
        ctypes.c_void_p(sock.fileno()), SIO_UDP_CONNRESET,
        ctypes.byref(value), ctypes.sizeof(value),
        None, 0, ctypes.byref(returned), None, None,
    )
    if rc != 0:  # SOCKET_ERROR
        err = wsa_last_error()
        raise OSError(err, f"WSAIoctl(SIO_UDP_CONNRESET) 실패. WSAGetLastError={err}")


def make_socket(port: int, record_state: bool = True) -> socket.socket:
    """진단 전체가 공유할 UDP 소켓 하나를 만든다.

    protocol.md 15장 체크리스트를 따른다.
      - SO_EXCLUSIVEADDRUSE 설정, SO_REUSEADDR 미사용
      - SIO_UDP_CONNRESET off
      - connect() 미호출
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    if os.name == "nt":
        so_excl = getattr(socket, "SO_EXCLUSIVEADDRUSE", -5)
        try:
            sock.setsockopt(socket.SOL_SOCKET, so_excl, 1)
        except OSError as exc:
            print(f"[warn] SO_EXCLUSIVEADDRUSE 설정 실패: {exc}", file=sys.stderr)

    sock.bind(("0.0.0.0", port))

    if os.name == "nt":
        try:
            _disable_udp_connreset(sock)
            if record_state:
                _connreset_state.update(disabled=True, detail=None)
        except Exception as exc:  # ctypes 실패, 권한, 드문 Windows 구성
            if record_state:
                _connreset_state.update(disabled=False, detail=str(exc))
            print(f"[warn] SIO_UDP_CONNRESET off 실패: {exc}", file=sys.stderr)
            print("       측정은 계속된다. ICMP port unreachable 수신 시 recvfrom 오류는 "
                  "무시하도록 되어 있다.", file=sys.stderr)
    elif record_state:
        # Windows 가 아니면 해당 없음. disabled=None 이 그 뜻이므로 detail 은 비운다.
        _connreset_state.update(disabled=None, detail=None)

    sock.setblocking(False)
    return sock


def primary_local_ip() -> str:
    """기본 경로가 쓰는 로컬 IP. 별도 소켓을 쓴다 (진단 소켓에 connect 하지 않는다)."""
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.connect(("8.8.8.8", 53))  # 패킷은 나가지 않는다
        return probe.getsockname()[0]
    except OSError:
        try:
            return socket.gethostbyname(socket.gethostname())
        except OSError:
            return "0.0.0.0"
    finally:
        probe.close()


# ---------------------------------------------------------------------------
# STUN
# ---------------------------------------------------------------------------


def build_binding_request(txid: bytes) -> bytes:
    assert len(txid) == 12
    return struct.pack(">HHI", STUN_BINDING_REQUEST, 0, STUN_COOKIE) + txid


def _decode_xor_mapped(value: bytes) -> "dict | None":
    if len(value) < 8:
        return None
    family = value[1]
    if family != 0x01:  # IPv4 만 받는다 (protocol.md 13장)
        return None
    port = struct.unpack(">H", value[2:4])[0] ^ (STUN_COOKIE >> 16)
    cookie = struct.pack(">I", STUN_COOKIE)
    ip = bytes(a ^ b for a, b in zip(value[4:8], cookie))
    return {"ip": socket.inet_ntoa(ip), "port": port}


def _decode_plain_mapped(value: bytes) -> "dict | None":
    if len(value) < 8 or value[1] != 0x01:
        return None
    port = struct.unpack(">H", value[2:4])[0]
    return {"ip": socket.inet_ntoa(value[4:8]), "port": port}


def parse_binding_response(data: bytes, txid: bytes) -> dict:
    """응답 1개를 검증하고 해석한다. 실패하면 {'ok': False, 'reason': ...}."""
    if len(data) < STUN_HEADER_LEN:
        return {"ok": False, "reason": "too_short"}
    msg_type, msg_len, cookie = struct.unpack(">HHI", data[:8])
    if cookie != STUN_COOKIE:
        return {"ok": False, "reason": "bad_cookie"}
    if data[8:20] != txid:
        return {"ok": False, "reason": "txid_mismatch"}
    if STUN_HEADER_LEN + msg_len != len(data):
        return {"ok": False, "reason": "length_mismatch"}
    if msg_type == STUN_BINDING_ERROR:
        return {"ok": False, "reason": "error_response",
                "error_code": _scan_error_code(data[20:20 + msg_len])}
    if msg_type != STUN_BINDING_SUCCESS:
        return {"ok": False, "reason": f"unexpected_type_0x{msg_type:04x}"}

    mapped = None
    fallback = None
    off = STUN_HEADER_LEN
    end = STUN_HEADER_LEN + msg_len
    while off + 4 <= end:
        atype, alen = struct.unpack(">HH", data[off:off + 4])
        off += 4
        if off + alen > end:  # 경계 초과. 순회 중단
            break
        value = data[off:off + alen]
        off += (alen + 3) & ~3  # 4바이트 경계 패딩
        if atype == ATTR_XOR_MAPPED_ADDRESS and mapped is None:
            mapped = _decode_xor_mapped(value)
        elif atype == ATTR_MAPPED_ADDRESS and fallback is None:
            fallback = _decode_plain_mapped(value)

    if mapped is None:
        mapped = fallback
    if mapped is None:
        return {"ok": False, "reason": "no_mapped_address"}
    return {"ok": True, "mapped": mapped}


def _scan_error_code(body: bytes) -> "int | None":
    off = 0
    while off + 4 <= len(body):
        atype, alen = struct.unpack(">HH", body[off:off + 4])
        off += 4
        if off + alen > len(body):
            break
        if atype == ATTR_ERROR_CODE and alen >= 4:
            cls = body[off + 2] & 0x07
            num = body[off + 3]
            return cls * 100 + num
        off += (alen + 3) & ~3
    return None


def stun_query(sock: socket.socket, host: str, port: int, stray: list) -> dict:
    """서버 한 곳에 Binding Request 를 보내고 응답을 기다린다.

    수신은 이 소켓 하나에서만 한다. 다른 출발지의 데이터그램은 stray 에 기록하고 버린다.
    """
    result = {
        "host": host, "port": port, "resolved_ip": None,
        "ok": False, "attempts": 0, "rtt_ms": None,
        "mapped": None, "error": None,
    }
    try:
        info = socket.getaddrinfo(host, port, socket.AF_INET, socket.SOCK_DGRAM)
        server = info[0][4]
    except OSError as exc:
        result["error"] = f"resolve_failed: {exc}"
        return result
    result["resolved_ip"] = server[0]

    txid = secrets.token_bytes(12)
    req = build_binding_request(txid)

    start = time.monotonic()
    deadline = start + STUN_DEADLINE_S
    sent_at = None
    next_idx = 0

    while True:
        now = time.monotonic()
        if now >= deadline:
            result["error"] = "timeout: STUN_DISCOVERY_FAILED"
            return result

        if next_idx < len(STUN_RETRY_SCHEDULE) and now >= start + STUN_RETRY_SCHEDULE[next_idx]:
            try:
                sock.sendto(req, server)
                sent_at = time.monotonic()
                result["attempts"] += 1
            except OSError as exc:
                result["error"] = f"sendto_failed: {exc}"
                return result
            next_idx += 1

        if next_idx < len(STUN_RETRY_SCHEDULE):
            wake = start + STUN_RETRY_SCHEDULE[next_idx]
        else:
            wake = deadline
        timeout = max(0.0, min(wake, deadline) - time.monotonic())

        ready, _, _ = select.select([sock], [], [], timeout)
        if not ready:
            continue
        try:
            data, src = sock.recvfrom(2048)
        except OSError as exc:
            stray.append({"kind": "recv_error", "detail": str(exc)})
            continue

        if src != server:
            # IP만 같고 포트가 다른 응답도 버린다. 트랜잭션 ID가 이미 강한 보호지만
            # 같은 호스트의 다른 서비스가 끼어드는 경로를 남겨둘 이유가 없다.
            stray.append({"kind": "other_source", "from": f"{src[0]}:{src[1]}", "len": len(data)})
            continue
        parsed = parse_binding_response(data, txid)
        if not parsed["ok"]:
            stray.append({"kind": "rejected", "from": f"{src[0]}:{src[1]}",
                          "reason": parsed["reason"]})
            if parsed["reason"] == "error_response":
                result["error"] = f"error_response code={parsed.get('error_code')}"
                return result
            continue

        result["ok"] = True
        result["mapped"] = parsed["mapped"]
        result["rtt_ms"] = round((time.monotonic() - sent_at) * 1000.0, 2)
        return result


def classify_mapping(servers: list, local_port: int = 0) -> dict:
    """매핑 거동을 판정한다.

    서로 다른 IP 를 가진 서버 2곳 이상에서 응답을 받아야 판정할 수 있다.
    한 서버만 답했거나 같은 IP 로만 물었으면 unknown 이다.
    """
    ok = [s for s in servers if s["ok"]]
    server_ips = {s["resolved_ip"] for s in ok}
    endpoints = {(s["mapped"]["ip"], s["mapped"]["port"]) for s in ok}

    confident = len(server_ips) >= 2
    if not confident:
        mapping = "unknown"
    elif len(endpoints) == 1:
        mapping = "endpoint-independent"
    else:
        mapping = "destination-dependent"

    # 포트 보존 여부. 판정에는 쓰지 않고 관측값으로만 남긴다.
    port_preserving = None
    if ok and local_port:
        port_preserving = all(s["mapped"]["port"] == local_port for s in ok)

    return {
        "distinct_server_ips": len(server_ips),
        "distinct_mapped_endpoints": len(endpoints),
        "mapping": mapping,
        "mapping_confident": confident,
        "port_preserving": port_preserving,
    }


def _is_usable_public(ip) -> bool:
    """인터넷에서 유니캐스트로 도달 가능한 주소인가.

    `is_global` 만으로는 부족하다. **멀티캐스트 주소는 `is_global` 이 참이다**
    (`224.0.0.1`, `ff02::1` 모두 그렇다). STUN 응답이 멀티캐스트를 담고 있으면
    유효한 공인 엔드포인트로 받아들여 잘못된 판정을 만든다.
    """
    if not (ip.is_global and not ip.is_multicast and not ip.is_unspecified
            and not ip.is_loopback and not ip.is_link_local and not ip.is_reserved):
        return False
    return not any(ip in n for n in _NOT_PEER_NETS if n.version == ip.version)


def is_public_unicast(text: str) -> bool:
    """문자열이 인터넷에서 도달 가능한 유니캐스트 주소인가.

    `check-peer` 하위 명령과 `classify_nat` 이 **같은 구현**을 쓴다. 검증을 두 번 쓰면
    한쪽만 고치게 되고, 그 차이가 잘못된 방화벽 규칙이나 잘못된 판정을 만든다.

    **완전한 IANA 특수 목적 대역 검사가 아니다.** `_NOT_PEER_NETS` 는 알려진 대역만
    거부한다. 새로 할당된 대역은 통과할 수 있다.
    """
    if not isinstance(text, str):
        return False
    try:
        return _is_usable_public(ipaddress.ip_address(text))
    except ValueError:
        return False


def _strict_port(value) -> "int | None":
    """정수이거나 표준 10진 문자열일 때만 포트로 받는다.

    `int()` 를 그냥 쓰면 `True` 가 1이 되고 `5.9` 가 5로 잘린다. 잘못된 입력이
    조용히 그럴듯한 값으로 바뀌면 그 위에서 내린 판정이 틀렸는지 알 수 없다.
    """
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        port = value
    elif (isinstance(value, str) and value.isascii() and value.isdigit()
            and len(value) <= 5):
        # 길이를 먼저 본다. Python 3.11+ 는 아주 긴 숫자 문자열에 int() 를 쓰면
        # int_max_str_digits 제한으로 ValueError 를 던진다.
        port = int(value)
    else:
        return None
    return port if 0 < port < 65536 else None


def classify_nat(local_ip: str, local_port: int, servers: list) -> dict:
    """로컬 엔드포인트와 STUN 관측 엔드포인트를 비교해 NAT 뒤인지 판정한다.

    **이것을 기록하지 않으면 측정을 잘못 보고하게 된다.** 2026-09-20 US-KR 측정에서
    한국 쪽은 로컬 주소가 곧 공인 주소였다(KT가 단말에 공인 IP를 직접 준다). 그것을
    사후에 `socket.local_ip` 와 대조해서야 알았다. 몰랐다면 그 측정을 NAT ↔ NAT 으로
    보고했을 것이고, blocker 20 판정이 틀렸을 것이다.

    **IP 와 포트를 둘 다 본다.** IP 만 비교하면 포트만 바꾸는 상위 장비를 "NAT 없음" 으로
    잘못 보고한다. `behind_nat` 이 `False` 라는 것은 변환이 전혀 없다는 강한 주장이므로
    두 값이 모두 같을 때만 낸다.

      True   로컬 엔드포인트와 관측 엔드포인트가 다르다. 중간에 변환이 있다
      False  둘이 완전히 같다. 이 호스트가 공인 엔드포인트를 직접 갖는다
      None   비교할 수 없다
    """
    # 서버 행 자체가 잘못된 형태일 수 있다. 여기서 터지면 측정 전체가 죽는다.
    ok = []
    try:
        for row in servers:
            if not isinstance(row, dict):
                raise TypeError("서버 행이 dict 가 아니다")
            # 불리언 True 만 받는다. "false" 같은 문자열도 truthy 라서
            # 단순 조건으로는 실패한 질의를 성공으로 처리한다.
            if row["ok"] is True:
                ok.append(row)
            elif row["ok"] is not False:
                raise TypeError("ok 가 불리언이 아니다")
    except (TypeError, KeyError):
        return {"behind_nat": None, "local_ip_scope": "unknown",
                "nat_note": "STUN 결과의 형태가 잘못되어 판정하지 않는다"}

    # 해석되지 않는 로컬 주소로 위상을 단정하지 않는다. 검증 안 된 입력에서
    # behind_nat=True 를 내면 없는 사실을 만들어 낸다.
    # 문자열만 받는다. ipaddress 는 정수도 받아 12345 를 0.0.48.57 로 해석하므로,
    # 문자열을 강제하지 않으면 잘못된 입력이 그럴듯한 주소로 조용히 바뀐다.
    if not isinstance(local_ip, str):
        return {"behind_nat": None, "local_ip_scope": "unknown",
                "nat_note": f"로컬 주소가 문자열이 아니다: {local_ip!r}"}
    try:
        addr = ipaddress.ip_address(local_ip)
    except ValueError:
        return {"behind_nat": None, "local_ip_scope": "unknown",
                "nat_note": f"로컬 주소를 해석할 수 없어 비교할 수 없다: {local_ip!r}"}
    if addr.is_unspecified:
        return {"behind_nat": None, "local_ip_scope": "unknown",
                "nat_note": "로컬 주소가 0.0.0.0(미지정)이라 비교할 수 없다"}

    # 루프백, 멀티캐스트, 예약 대역은 외부로 나가는 출발지가 될 수 없다. ipaddress 는
    # 루프백도 is_private 로 보므로 먼저 걸러내지 않으면 127.0.0.1 을 "사설 대역이고
    # 관측값과 다르니 NAT 이 있다" 로 오판한다.
    if (addr.is_loopback or addr.is_multicast or addr.is_reserved
            or addr.is_link_local):
        # 링크 로컬(169.254.x.x)은 DHCP 실패 시 붙는 주소다. 인터넷 출발지가 될 수
        # 없으므로, 이 주소로 STUN 관측값과 비교해 위상을 말하면 안 된다.
        return {"behind_nat": None, "local_ip_scope": "special",
                "nat_note": f"로컬 주소가 외부 출발지가 될 수 없는 대역이다: {local_ip}"}

    lport = _strict_port(local_port)
    if lport is None:
        return {"behind_nat": None, "local_ip_scope": "unknown",
                "nat_note": f"로컬 포트가 유효하지 않다: {local_port!r}"}

    if addr.is_global:
        scope = "public"
    elif any(addr in n for n in _PRIVATE_NETS if n.version == addr.version):
        scope = "private"
    elif any(addr in n for n in _SPECIAL_NETS if n.version == addr.version):
        scope = "special"          # CGNAT
    else:
        # 문서용, 벤치마킹, 그 밖의 특수 대역. 단말의 실제 출발지가 아니다.
        return {"behind_nat": None, "local_ip_scope": "unknown",
                "nat_note": f"로컬 주소가 단말 출발지로 쓰이는 대역이 아니다: {local_ip}"}

    # 관측값도 검증한다. 문자열이 아니라 해석한 주소 객체로 비교한다. IPv6 는 같은
    # 주소를 여러 방식으로 적을 수 있어(2001:0db8::1 과 2001:db8::1) 문자열 비교가 틀린다.
    # **하나라도 해석되지 않으면 판정하지 않는다.** 남은 값 하나가 우연히 일치해서
    # "NAT 없음" 이 나오면 틀린 위상을 보고하게 된다.
    observed = set()
    for row in ok:
        try:
            raw_ip = row["mapped"]["ip"]
            if not isinstance(raw_ip, str):
                raise TypeError("관측 주소가 문자열이 아니다")
            ip = ipaddress.ip_address(raw_ip)
        except (ValueError, KeyError, TypeError):
            return {"behind_nat": None, "local_ip_scope": scope,
                    "nat_note": "해석할 수 없는 STUN 관측 주소가 있어 판정하지 않는다"}
        # STUN 서버가 보고하는 것은 공인 도달 가능 주소여야 한다. 0.0.0.0, 루프백,
        # 링크 로컬, 예약 대역이 오면 그 값으로 위상을 말할 수 없다.
        if not _is_usable_public(ip):
            return {"behind_nat": None, "local_ip_scope": scope,
                    "nat_note": f"STUN 관측 주소가 도달 가능한 공인 유니캐스트가 아니다: {raw_ip}"}
        try:
            port = _strict_port(row["mapped"]["port"])
        except (KeyError, TypeError):
            port = None
        if port is None:
            return {"behind_nat": None, "local_ip_scope": scope,
                    "nat_note": "유효하지 않은 STUN 관측 포트가 있어 판정하지 않는다"}
        observed.add((ip, port))

    if not observed:
        return {"behind_nat": None, "local_ip_scope": scope,
                "nat_note": "비교할 수 있는 STUN 관측 엔드포인트가 없다"}

    mine = (addr, lport)
    behind = not (len(observed) == 1 and mine in observed)

    # behind_nat=False 는 "이 호스트가 공인 엔드포인트를 직접 갖는다" 는 강한 주장이다.
    # 사설·특수 대역이 관측값과 일치하는 경우는 그 주장을 뒷받침하지 못한다. 잘못된
    # STUN 응답이 로컬 주소를 그대로 돌려주기만 해도 성립해 버린다.
    if not behind and not _is_usable_public(addr):
        return {"behind_nat": None, "local_ip_scope": scope,
                "nat_note": (f"관측값이 로컬 엔드포인트와 같은데 그 주소가 공인 대역이 "
                             f"아니다({scope}). 판정하지 않는다")}

    if not behind:
        note = ("로컬 엔드포인트가 곧 공인 엔드포인트다. 주소도 포트도 바뀌지 않았다. "
                "이 호스트의 인바운드는 방화벽과 상위 경로만이 거른다")
    elif len(observed) == 1 and next(iter(observed))[0] == addr:
        note = ("IP 는 같은데 포트가 바뀐다. 주소 변환은 없고 포트만 바꾸는 장비가 있다")
    elif scope == "public":
        note = "로컬 주소가 공인 대역인데 관측값과 다르다. 상위에 변환 장비가 있다"
    else:
        note = f"로컬 주소가 {scope} 대역이고 관측값과 다르다. 통상적인 NAT 구성이다"

    return {"behind_nat": behind, "local_ip_scope": scope, "nat_note": note}


def run_stun(sock: socket.socket, servers: list, local_port: int = 0) -> dict:
    stray: list = []
    rows = []
    for host, port in servers:
        print(f"  STUN {host}:{port} ... ", end="", flush=True)
        row = stun_query(sock, host, port, stray)
        rows.append(row)
        if row["ok"]:
            m = row["mapped"]
            print(f"{m['ip']}:{m['port']}  ({row['rtt_ms']} ms, {row['attempts']}회 송신)")
        else:
            print(f"실패 ({row['error']})")

    out = {"servers": rows}
    out.update(classify_mapping(rows, local_port))
    out["stray_datagrams"] = stray
    return out


# ---------------------------------------------------------------------------
# 펀치 시험
# ---------------------------------------------------------------------------


def build_punch(kind: int, session: int, seq: int, ts_ns: int) -> bytes:
    return struct.pack(PUNCH_FMT, PUNCH_MAGIC, kind, session, seq, ts_ns)


def parse_punch(data: bytes) -> "tuple | None":
    """형식과 종류까지 검증한다. 모르는 종류는 여기서 버린다."""
    if len(data) != PUNCH_LEN:
        return None
    magic, kind, session, seq, ts_ns = struct.unpack(PUNCH_FMT, data)
    if magic != PUNCH_MAGIC:
        return None
    if kind not in PUNCH_KINDS:
        return None
    return kind, session, seq, ts_ns


def run_punch(sock: socket.socket, peer: tuple, duration_s: float) -> dict:
    """양쪽이 동시에 실행해야 한다. 200ms 간격으로 PING 을 쏘며 PONG 을 기다린다.

    PONG 은 내가 실제로 보낸 PING 과 세션, 시퀀스, 타임스탬프가 모두 맞을 때만 센다.
    중복 PONG 도 한 번만 센다. 그렇지 않으면 위조나 망 중복 때문에 성공과 손실률이 부풀려진다.

    **출발지로는 거르지 않는다.** 상대 NAT 이 나에게 다른 매핑을 쓰면 예상과 다른 포트에서
    오는 것이 정상이고, 그 사실 자체가 관측 대상이다(`inbound_sources`).
    """
    session = secrets.randbits(32)  # 이전 회차의 지연 패킷을 이번 회차로 오인하지 않는다
    start = time.monotonic()
    end = start + duration_s
    next_send = start
    seq = 0

    sent_pings: dict = {}   # seq -> ts_ns. 내가 실제로 보낸 것
    acked: set = set()      # 이미 센 seq. 중복 PONG 방지
    sent = 0
    sent_after_first_inbound = 0
    recv_ping = 0
    recv_pong = 0
    pong_after_first_inbound = 0
    rejected_pong = 0
    peer_session = None
    first_inbound_ms = None
    first_inbound_seq = None
    rtts: list = []
    sources: dict = {}
    other = 0
    next_phase = 0

    print(f"  {peer[0]}:{peer[1]} 로 {duration_s:.0f}초 동안 펀치 시도. Ctrl+C 로 중단")

    try:
        while True:
            now = time.monotonic()
            if now >= end:
                break

            if now >= next_send:
                seq += 1
                ts = time.monotonic_ns()
                try:
                    sock.sendto(build_punch(PUNCH_PING, session, seq, ts), peer)
                    sent_pings[seq] = ts
                    sent += 1
                    if first_inbound_ms is not None:
                        sent_after_first_inbound += 1
                except OSError as exc:
                    print(f"  [warn] sendto 실패: {exc}")
                next_send += PUNCH_INTERVAL_S
                if next_send < now:  # 밀렸으면 따라잡지 않고 재기준
                    next_send = now + PUNCH_INTERVAL_S

            timeout = max(0.0, min(next_send, end) - time.monotonic())
            ready, _, _ = select.select([sock], [], [], timeout)
            if not ready:
                continue
            try:
                data, src = sock.recvfrom(2048)
            except OSError:
                # Windows 에서 SIO_UDP_CONNRESET off 가 실패했을 때의 ICMP 경로
                continue

            parsed = parse_punch(data)
            if parsed is None:
                other += 1
                continue
            kind, rsession, rseq, ts_ns = parsed

            if kind not in (PUNCH_PING, PUNCH_PONG):
                # 다음 단계(PHASE_READY 등)의 패킷이 먼저 도착할 수 있다. 상대가 나보다
                # 먼저 펀치를 끝낸 경우다. 이 루프의 지표에 섞으면 안 된다.
                # non_punch_datagrams 와 섞지 않는다. 그쪽은 "포트가 남의 트래픽과
                # 겹쳤다" 는 뜻이고, 이것은 정상 동작이다.
                next_phase += 1
                continue

            if kind == PUNCH_PONG:
                # 내가 보낸 PING 의 응답인지 확인한다. 셋 다 맞아야 한다.
                if (rsession != session or rseq not in sent_pings
                        or sent_pings[rseq] != ts_ns or rseq in acked):
                    rejected_pong += 1
                    continue

            key = f"{src[0]}:{src[1]}"
            sources[key] = sources.get(key, 0) + 1
            if first_inbound_ms is None:
                first_inbound_ms = round((time.monotonic() - start) * 1000.0, 1)
                first_inbound_seq = seq
                print(f"  [{first_inbound_ms:.0f} ms] {key} 로부터 첫 수신")

            if kind == PUNCH_PING:
                recv_ping += 1
                # 상대의 세션 값은 상대가 보낸 PING 에서만 알 수 있다. PONG 은 내 세션을
                # 되돌려 주는 것이라 알려주지 않는다. 이후 단계의 인증에 쓴다.
                if peer_session is None:
                    peer_session = rsession
                try:
                    # 상대의 세션을 그대로 돌려준다. 내 세션이 아니다.
                    sock.sendto(build_punch(PUNCH_PONG, rsession, rseq, ts_ns), src)
                except OSError:
                    pass
            else:  # PUNCH_PONG. 위에서 검증을 통과한 것만 온다
                acked.add(rseq)
                recv_pong += 1
                # 첫 수신 이전에 보낸 PING 의 응답은 분자에서 뺀다. 분모(sent_after)와
                # 같은 구간을 써야 손실률이 분모를 넘지 않는다.
                if first_inbound_seq is not None and rseq > first_inbound_seq:
                    pong_after_first_inbound += 1
                rtts.append((time.monotonic_ns() - ts_ns) / 1e6)
    except KeyboardInterrupt:
        print("  중단됨")

    if recv_pong > 0:
        result = "success"
    elif recv_ping > 0:
        result = "one-way"
    else:
        result = "failure"

    expected = f"{peer[0]}:{peer[1]}"
    denom = sent_after_first_inbound
    loss = None
    if denom > 0:
        loss = round(max(0.0, 1.0 - pong_after_first_inbound / denom) * 100.0, 1)

    rtt_stats = None
    if rtts:
        rtt_stats = {
            "min": round(min(rtts), 2),
            "median": round(statistics.median(rtts), 2),
            "max": round(max(rtts), 2),
            "samples": len(rtts),
        }

    return {
        "peer_endpoint": expected,
        "duration_s": duration_s,
        "session": session,
        "peer_session": peer_session,
        "sent": sent,
        "recv_ping": recv_ping,
        "recv_pong": recv_pong,
        "rejected_pong": rejected_pong,
        "first_inbound_ms": first_inbound_ms,
        "rtt_ms": rtt_stats,
        "loss_pct": loss,
        "loss_basis": {"sent_after_first_inbound": denom,
                       "pong_after_first_inbound": pong_after_first_inbound},
        "inbound_sources": sorted(sources),
        "source_matches_expected": (list(sources) == [expected]) if sources else False,
        "non_punch_datagrams": other,
        "next_phase_datagrams": next_phase,
        "result": result,
    }


def unsolicited_verdict(recv: int, ack_recv: int, sent: int,
                       peer_ready: bool, peer_sent: int) -> tuple:
    """(상대가 단계를 돌았는가, 내 쪽 판정, 상대 쪽 판정).

    **상대가 이 단계를 돌았다는 증거가 있어야 `blocked` 라고 말할 수 있다.**
    증거가 없으면 `unknown` 이다. 상대가 구판을 쓰거나 `--unsolicited` 를 안 줬을 수도
    있는데, 그것을 차단으로 기록하면 없는 결함을 만들어 낸다.

      peer_ready  상대의 READY 를 받았다. 상대가 단계에 들어왔다는 직접 증거다
      recv        상대의 탐침이 나에게 도달했다
      ack_recv    내 탐침이 상대에게 도달했다

    `peer_ready` 를 빼면 **양쪽이 다 막혔을 때 `unknown` 이 나온다.** 그것이 바로
    우리가 찾는 `blocked` 인데 증거가 없다고 판단해 버린다. READY 는 펀치로 이미 열린
    경로로 오가므로 탐침이 전부 막혀도 도착한다. 그래서 유효한 증거다.

    **다만 들어왔다는 것과 실제로 쐈다는 것은 다르다.** 각 방향마다 "쏜 쪽이 한 발이라도
    성공했는가" 를 따로 요구한다. 송신이 전부 실패한 것을 수신 차단으로 기록하면 안 된다.

      mine   내 수신 판정   -> 상대가 쐈어야(peer_sent > 0) blocked 라고 할 수 있다
      theirs 상대 수신 판정 -> 내가 쐈어야(sent > 0) blocked 라고 할 수 있다
    """
    peer_ran = bool(peer_ready) or (recv > 0) or (ack_recv > 0)
    if recv > 0:
        mine = "allowed"
    elif peer_ran and peer_sent > 0:
        mine = "blocked"
    else:
        mine = "unknown"
    if ack_recv > 0:
        theirs = "allowed"
    elif peer_ran and sent > 0:
        theirs = "blocked"
    else:
        theirs = "unknown"
    return peer_ran, mine, theirs


def _send_ready(main_sock, peer, my_session, sent_count: int = 0) -> None:
    """READY 를 보낸다. seq 자리에 **지금까지 성공한 탐침 송신 수**를 싣는다.

    상대가 단계에 들어왔다는 것만으로는 부족하다. 상대의 송신이 전부 실패했다면 내가
    못 받은 것은 내 쪽이 막혀서가 아니다. 그 구분을 하려면 상대가 몇 발 쐈는지 알아야
    한다. READY 는 펀치로 이미 열린 경로로 오가므로 탐침이 전부 막혀도 도착한다.
    """
    try:
        main_sock.sendto(
            build_punch(PUNCH_PHASE_READY, my_session, sent_count, time.monotonic_ns()), peer)
    except OSError:
        pass


def _sync_phase(main_sock, peer, my_session, peer_session, timeout_s) -> dict:
    """양쪽이 단계에 들어올 때까지 맞춘다.

    맞추지 않으면 시작 시각이 어긋나 한쪽 창이 닫힌 뒤에 상대 패킷이 도착한다. 그러면
    통과하는 경로인데도 `blocked` 로 기록된다. **거짓 blocked 가 이 시험에서 가장 나쁜
    결과다.** 그 결과로 없는 요구사항(방화벽 규칙 등록)을 설계에 넣게 된다.

    READY 는 본 소켓에서 상대의 본 엔드포인트로 보낸다. 이 경로는 펀치로 이미 열려 있다.
    """
    start = time.monotonic()
    deadline = start + timeout_s
    next_send = start
    peer_ready = False

    while time.monotonic() < deadline:
        now = time.monotonic()
        if now >= next_send:
            _send_ready(main_sock, peer, my_session)
            next_send = now + UNSOL_SYNC_INTERVAL_S

        if peer_ready and time.monotonic() - start > 0.6:
            # 상대를 확인했고, 내 READY 도 몇 번 나갔다. 상대도 나를 봤을 것이다.
            break

        ready, _, _ = select.select(
            [main_sock], [], [], max(0.0, min(next_send, deadline) - time.monotonic()))
        if not ready:
            continue
        try:
            data, src = main_sock.recvfrom(2048)
        except OSError:
            continue
        parsed = parse_punch(data)
        if parsed is None or src[0] != peer[0]:
            continue
        kind, rsession, _, _ = parsed
        if kind == PUNCH_PHASE_READY and (peer_session is None or rsession == peer_session):
            peer_ready = True

    return {"peer_ready": peer_ready,
            "sync_wait_ms": round((time.monotonic() - start) * 1000.0, 1)}


def run_unsolicited(main_sock: socket.socket, peer: tuple, duration_s: float,
                    my_session: int, peer_session: "int | None") -> dict:
    """요청하지 않은 인바운드가 통과하는지 시험한다.

    펀치가 성립한 **뒤에** 돈다. 새 소켓을 하나 더 열어서 상대의 같은 엔드포인트로 쏜다.
    상대 입장에서 이 패킷의 출발지 포트는 **자신이 한 번도 보낸 적 없는 포트**다. 따라서
    상태 기반 방화벽이나 포트 의존 필터링이 있으면 막힌다.

    받는 쪽은 출발지 **IP** 와 상대 세션 값으로 인증한다. 출발지 **포트** 로는 거르지
    않는다. 포트가 다른 것이 이 시험의 전제이기 때문이다.

      recv     > 0  ->  상대의 요청하지 않은 패킷이 나에게 도달했다 (내 쪽이 허용)
      ack_recv > 0  ->  내 요청하지 않은 패킷이 상대에게 도달했다 (상대 쪽이 허용)
    """
    sync = _sync_phase(main_sock, peer, my_session, peer_session, UNSOL_SYNC_TIMEOUT_S)
    if not sync["peer_ready"]:
        print(f"  [warn] {UNSOL_SYNC_TIMEOUT_S:.0f}초 안에 상대가 단계에 들어오지 않았다. 건너뛴다.")
        return {
            "enabled": True, "duration_s": duration_s, "skipped": "peer_not_ready",
            "sync": sync, "peer_ran_phase": False,
            "inbound_unsolicited": "unknown", "peer_inbound_unsolicited": "unknown",
        }

    probe = make_socket(0, record_state=False)
    probe_port = probe.getsockname()[1]

    start = time.monotonic()
    end = start + duration_s
    next_send = start
    seq = 0
    sent = recv = ack_recv = 0
    rejected = 0
    sent_probes: dict = {}   # seq -> ts_ns. 내가 실제로 보낸 것
    acked: set = set()
    peer_sent = 0            # 상대가 READY 로 알려온 성공 송신 수
    sources: dict = {}
    ack_sources: dict = {}

    print(f"  요청하지 않은 인바운드 시험 {duration_s:.0f}초. "
          f"새 소켓 로컬 포트 {probe_port} (본 소켓과 다르다)")

    def authentic(src, rsession) -> bool:
        """상대 IP 에서 왔고, 상대 세션 값을 가졌고, **본 엔드포인트가 아닌** 것만 받는다.

        출발지 포트로는 거르지 않는다. 포트가 다른 것이 이 시험의 전제다. 다만 출발지가
        **펀치에 쓴 바로 그 엔드포인트**면 그 경로는 이미 요청된 상태라 아무것도 증명하지
        못한다. 그것을 세면 거짓 `allowed` 가 된다.
        """
        if src[0] != peer[0]:
            return False
        if src == peer:
            return False
        if peer_session is not None and rsession != peer_session:
            return False
        return True

    try:
        while True:
            now = time.monotonic()
            if now >= end:
                break

            if now >= next_send:
                seq += 1
                ts = time.monotonic_ns()
                try:
                    probe.sendto(build_punch(PUNCH_UNSOL, my_session, seq, ts), peer)
                    sent_probes[seq] = ts
                    sent += 1
                except OSError as exc:
                    print(f"  [warn] sendto 실패: {exc}")
                # 측정 중에도 READY 를 계속 보낸다. 상대가 나보다 늦게 단계에 들어오면
                # 이것을 보고 맞춘다. 멈추면 늦은 쪽이 영영 나를 못 본다.
                _send_ready(main_sock, peer, my_session, sent)
                next_send += UNSOL_INTERVAL_S
                if next_send < now:
                    next_send = now + UNSOL_INTERVAL_S

            timeout = max(0.0, min(next_send, end) - time.monotonic())
            ready, _, _ = select.select([main_sock, probe], [], [], timeout)

            for sock in ready:
                try:
                    data, src = sock.recvfrom(2048)
                except OSError:
                    continue
                parsed = parse_punch(data)
                if parsed is None:
                    continue
                kind, rsession, rseq, ts_ns = parsed
                key = f"{src[0]}:{src[1]}"

                if sock is main_sock and kind == PUNCH_PHASE_READY:
                    # 상대의 성공 송신 수를 seq 자리에 싣고 온다. 본 엔드포인트에서
                    # 오는 것이 정상이므로 src == peer 를 거부하지 않는다.
                    if src[0] == peer[0] and (peer_session is None or rsession == peer_session):
                        peer_sent = max(peer_sent, rseq)
                elif sock is main_sock and kind == PUNCH_UNSOL:
                    if not authentic(src, rsession):
                        rejected += 1
                        continue
                    recv += 1
                    sources[key] = sources.get(key, 0) + 1
                    try:
                        # 본 소켓에서 되돌려 준다. 상대의 새 소켓은 이미 송신했으므로
                        # 그쪽 방화벽은 이 응답을 요청된 것으로 본다.
                        main_sock.sendto(
                            build_punch(PUNCH_UNSOL_ACK, rsession, rseq, ts_ns), src)
                    except OSError:
                        pass
                elif sock is probe and kind == PUNCH_UNSOL_ACK:
                    # 내가 실제로 보낸 탐침의 응답인지 확인한다. 중복은 한 번만 센다.
                    if (src[0] != peer[0] or rsession != my_session
                            or rseq not in sent_probes or sent_probes[rseq] != ts_ns
                            or rseq in acked):
                        rejected += 1
                        continue
                    acked.add(rseq)
                    ack_recv += 1
                    ack_sources[key] = ack_sources.get(key, 0) + 1
    except KeyboardInterrupt:
        print("  중단됨")
    finally:
        probe.close()

    peer_ran, mine, theirs = unsolicited_verdict(
        recv, ack_recv, sent, sync["peer_ready"], peer_sent)

    return {
        "enabled": True,
        "duration_s": duration_s,
        "sync": sync,
        "probe_local_port": probe_port,
        "peer_session_known": peer_session is not None,
        "sent": sent,
        "recv": recv,
        "recv_sources": sorted(sources),
        "ack_recv": ack_recv,
        "ack_sources": sorted(ack_sources),
        "rejected": rejected,
        "peer_sent_reported": peer_sent,
        "peer_ran_phase": peer_ran,
        "inbound_unsolicited": mine,
        "peer_inbound_unsolicited": theirs,
    }


# ---------------------------------------------------------------------------
# 출력
# ---------------------------------------------------------------------------

MAPPING_NOTE = {
    "endpoint-independent": "모든 STUN 서버가 같은 공인 엔드포인트를 봤다. 홀펀칭 가능성이 높다.",
    "destination-dependent": "서버마다 다른 공인 엔드포인트를 봤다. 대칭형 거동이다. blocker 20 위험.",
    "unknown": "서로 다른 IP 의 서버 2곳에서 응답을 받지 못해 판정할 수 없다.",
}

UNSOL_NOTE = {
    "allowed": "요청하지 않은 인바운드가 통과한다. 목적지 의존 매핑 상대도 받을 수 있다.",
    "blocked": "막힌다. 상대가 예상과 다른 포트로 응답하면 홀펀칭이 실패한다.",
    "unknown": "상대가 이 단계를 돌지 않아 판정할 수 없다.",
}

RESULT_NOTE = {
    "success": "왕복 성립. 이 망 조합에서 직접 UDP 가 된다.",
    "one-way": "상대 패킷은 받았으나 왕복이 확인되지 않았다. 필터링 또는 한쪽 방화벽을 의심한다.",
    "failure": "아무것도 받지 못했다. 방화벽 허용 여부부터 확인한 뒤 NAT 거동을 의심한다.",
}


def print_stun_summary(stun: dict) -> None:
    print()
    print(f"  판정: {stun['mapping']}  ({MAPPING_NOTE[stun['mapping']]})")
    print(f"  서로 다른 서버 IP {stun['distinct_server_ips']}개, "
          f"관측된 서로 다른 엔드포인트 {stun['distinct_mapped_endpoints']}개")
    if stun["port_preserving"] is not None:
        print(f"  포트 보존: {'예' if stun['port_preserving'] else '아니오'}")
    if not stun["mapping_confident"]:
        print("  [warn] 판정 근거가 부족하다. --stun 으로 서버를 추가해 다시 재라.")
    if stun["stray_datagrams"]:
        print(f"  [note] 무시한 데이터그램 {len(stun['stray_datagrams'])}건")


def shareable_endpoint(stun: dict) -> "str | None":
    ok = [s for s in stun["servers"] if s["ok"]]
    if not ok:
        return None
    m = ok[0]["mapped"]
    return f"{m['ip']}:{m['port']}"


def valid_port(text, *, allow_zero: bool = False) -> int:
    """포트를 1..65535 로 강제한다. 범위를 안 보면 bind/sendto 에서 OverflowError 가 난다."""
    try:
        port = int(text)
    except (TypeError, ValueError):
        raise ValueError(f"포트가 숫자가 아니다: {text!r}")
    low = 0 if allow_zero else 1
    if not (low <= port <= 65535):
        raise ValueError(f"포트가 범위를 벗어났다 ({low}..65535): {port}")
    return port


def valid_duration(value: float) -> float:
    """NaN 과 무한대를 막는다. 무한대면 펀치 루프가 끝나지 않는다."""
    if not math.isfinite(value) or value <= 0:
        raise ValueError(f"--duration 은 유한한 양수여야 한다: {value}")
    return value


def parse_endpoint(text: str) -> tuple:
    text = text.strip()
    if ":" not in text:
        raise ValueError("IP:PORT 형식이어야 한다")
    host, _, port = text.rpartition(":")
    host = host.strip("[]")
    if not host:
        raise ValueError("호스트가 비어 있다")
    ip = socket.gethostbyname(host)
    return (ip, valid_port(port))


def save(record: dict, out_dir: Path, label: str, mode: str) -> Path:
    """기존 측정을 덮어쓰지 않는다. 같은 초에 두 번 저장되면 접미사를 붙인다."""
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    safe = "".join(c if c.isalnum() or c in "-_" else "-" for c in label)
    body = json.dumps(record, indent=2, ensure_ascii=False) + "\n"
    for suffix in [""] + [f"-{i}" for i in range(1, 100)]:
        path = out_dir / f"{stamp}-{safe}{suffix}-{mode}.json"
        try:
            with open(path, "x", encoding="utf-8") as fh:  # 배타 생성
                fh.write(body)
            return path
        except FileExistsError:
            continue
    raise RuntimeError(f"결과 파일 이름이 100번 충돌했다: {out_dir}")


# ---------------------------------------------------------------------------


def main(argv=None) -> int:
    setup_console()
    default_out = Path(__file__).resolve().parent / "results"

    ap = QuietParser(
        description="NAT 매핑 거동과 UDP 홀펀칭 실측 (design-audit blocker 20)")
    sub = ap.add_subparsers(dest="mode", required=True, parser_class=QuietParser)

    cp = sub.add_parser("check-peer",
                        help="주소가 도달 가능한 공인 유니캐스트인지 확인한다. "
                             "맞으면 종료 코드 0, 아니면 2")
    cp.add_argument("address", nargs="?",
                    help="검사할 IP 주소. --stdin 을 쓰면 생략한다")
    cp.add_argument("--stdin", action="store_true",
                    help="주소를 표준 입력에서 읽는다. 명령줄 인자는 프로세스 목록과 "
                         "감사 로그에 남으므로 민감한 주소는 이쪽을 쓴다")
    cp.add_argument("--ipv4", action="store_true",
                    help="IPv4 만 허용한다. 이 도구의 소켓이 AF_INET 이므로 "
                         "방화벽 시험처럼 IPv4 경로를 전제하는 곳에서 쓴다")

    for name, help_text in (("probe", "STUN 매핑 거동만 측정"),
                            ("punch", "STUN 측정 후 같은 소켓으로 홀펀칭 시도")):
        p = sub.add_parser(name, help=help_text)
        p.add_argument("--label", default=socket.gethostname(), help="망 식별자. 기본값 호스트명")
        p.add_argument("--port", type=int, default=0, help="로컬 UDP 포트. 기본값 0(임시)")
        p.add_argument("--stun", action="append", metavar="HOST:PORT",
                       help="STUN 서버 지정. 반복 가능. 지정하면 기본 목록을 대체한다")
        p.add_argument("--out", type=Path, default=default_out, help="결과 JSON 디렉터리")
        if name == "punch":
            p.add_argument("--peer", metavar="IP:PORT", help="상대 공인 엔드포인트")
            p.add_argument("--duration", type=float, default=PUNCH_DEFAULT_DURATION_S,
                           help="펀치 시도 시간(초). 기본 30")
            p.add_argument("--unsolicited", action="store_true",
                           help="펀치 성립 후, 새 소켓에서 보내는 요청하지 않은 인바운드가 "
                                "통과하는지 시험한다. 양쪽이 함께 줘야 한다")
            p.add_argument("--unsolicited-duration", type=float,
                           default=UNSOL_DEFAULT_DURATION_S,
                           help="위 시험 시간(초). 기본 10")

    args = ap.parse_args(argv)

    if args.mode == "check-peer":
        # 소켓을 열지 않는다. 주소는 출력하지 않는다 — 터미널 기록에 남는다.
        if args.stdin and args.address is not None:
            # 둘 다 주면 어느 쪽을 검증했는지 호출자가 알 수 없다. 조용히 하나를
            # 고르면 입력한 주소와 다른 값을 검증하고 통과시킬 수 있다.
            sys.stderr.write("address 와 --stdin 을 함께 줄 수 없다\n")
            return 2
        if args.stdin:
            # 첫 줄만 읽고 strip 하면 나머지 입력이 검증되지 않은 채 남는다.
            # 전부 읽고, 끝의 줄바꿈 하나만 허용한다.
            raw = sys.stdin.read()
            address = raw[:-1] if raw.endswith("\n") else raw
            if not address or any(ch.isspace() for ch in address):
                sys.stderr.write("표준 입력은 공백 없는 주소 한 줄이어야 한다\n")
                return 2
        elif args.address is not None:
            address = args.address
        else:
            sys.stderr.write("주소를 인자로 주거나 --stdin 을 쓴다\n")
            return 2
        ok = is_public_unicast(address)
        if ok and args.ipv4:
            ok = isinstance(ipaddress.ip_address(address), ipaddress.IPv4Address)
        if ok:
            # 아무것도 출력하지 않는다. 종료 코드만으로 답한다는 계약이고,
            # 조용한 검증 명령으로 다른 스크립트에서 쓸 수 있어야 한다.
            return 0
        sys.stderr.write("도달 가능한 공인 유니캐스트 주소가 아니다"
                         + (" (IPv4 만 허용)" if args.ipv4 else "") + "\n")
        return 2

    try:
        local_port = valid_port(args.port, allow_zero=True)
        if args.mode == "punch":
            valid_duration(args.duration)
            if args.unsolicited:
                valid_duration(args.unsolicited_duration)
            if args.peer:  # 5초짜리 STUN 을 돌리기 전에 먼저 거른다
                parse_endpoint(args.peer)
        servers = DEFAULT_STUN_SERVERS
        if args.stun:
            servers = []
            for item in args.stun:
                host, _, port = item.rpartition(":")
                if not host:
                    # 입력값을 되쓰지 않는다. 로그에 남는다.
                    raise ValueError("--stun 은 HOST:PORT 형식이어야 한다")
                servers.append((host, valid_port(port)))
    except ValueError as exc:
        ap.error(str(exc))
    except OSError:
        # 이름 해석 실패. 예외 문자열과 역추적에 입력값이 들어가므로 되쓰지 않는다.
        ap.error("상대 엔드포인트의 호스트를 해석할 수 없다")

    sock = make_socket(local_port)
    local_ip = primary_local_ip()
    local_port = sock.getsockname()[1]  # --port 0 이면 여기서 실제 포트가 정해진다

    record = {
        "schema": SCHEMA,
        "label": args.label,
        "mode": args.mode,
        "started_utc": _utcnow_iso(),
        "platform": {
            "system": platform.system(),
            "release": platform.release(),
            "python": platform.python_version(),
        },
        "socket": {
            "local_ip": local_ip,
            "local_port": local_port,
            "reused_for_punch": args.mode == "punch",
            "udp_connreset_disabled": _connreset_state["disabled"],
            "udp_connreset_detail": _connreset_state["detail"],
            "behind_nat": None,
            "local_ip_scope": None,
            "nat_note": None,
        },
        "stun": None,
        "punch": None,
        "unsolicited": None,
    }

    print()
    print(f"natprobe {args.mode}  label={args.label}")
    print(f"  로컬 엔드포인트 {local_ip}:{local_port}  (이 소켓 하나만 쓴다)")
    if os.name == "nt":
        ok = _connreset_state["disabled"]
        print(f"  SIO_UDP_CONNRESET off: {'적용됨' if ok else '실패'}")
    print()

    try:
        record["stun"] = run_stun(sock, servers, local_port)
        record["socket"].update(
            classify_nat(local_ip, local_port, record["stun"]["servers"]))
        print_stun_summary(record["stun"])
        nat = record["socket"]["behind_nat"]
        label = {True: "예", False: "아니오", None: "판정 불가"}[nat]
        print(f"  NAT 뒤인가: {label}  ({record['socket']['nat_note']})")

        mine = shareable_endpoint(record["stun"])
        if mine:
            print()
            print(f"  >>> 상대에게 알려줄 내 엔드포인트: {mine}")
        if record["stun"]["mapping"] == "destination-dependent":
            print("  [warn] 매핑이 목적지마다 달라서 위 값이 상대에게 유효하지 않을 수 있다.")

        if args.mode == "punch":
            peer_text = args.peer
            if not peer_text:
                print()
                peer_text = input("  상대 엔드포인트 (IP:PORT) > ")
            try:
                peer = parse_endpoint(peer_text)
            except (ValueError, OSError) as exc:
                print(f"  [error] 상대 엔드포인트를 읽을 수 없다: {exc}")
                return 2
            if not args.peer:
                input("  양쪽 준비되면 Enter. 두 대에서 거의 동시에 눌러라 > ")
            print()
            record["punch"] = run_punch(sock, peer, args.duration)
            r = record["punch"]
            print()
            print(f"  판정: {r['result']}  ({RESULT_NOTE[r['result']]})")
            print(f"  송신 {r['sent']}, PING 수신 {r['recv_ping']}, PONG 수신 {r['recv_pong']}")
            if r["first_inbound_ms"] is not None:
                print(f"  첫 수신까지 {r['first_inbound_ms']} ms")
            if r["rtt_ms"]:
                s = r["rtt_ms"]
                print(f"  RTT min {s['min']} / median {s['median']} / max {s['max']} ms "
                      f"({s['samples']} 표본)")
            if r["loss_pct"] is not None:
                print(f"  손실률 {r['loss_pct']}% (첫 수신 이후 송신분 기준)")
            if r["inbound_sources"] and not r["source_matches_expected"]:
                print(f"  [warn] 예상과 다른 출발지: {r['inbound_sources']}")
                print("         상대 NAT 이 나에게 다른 매핑을 썼다. 목적지 의존 매핑의 증거다.")

            if args.unsolicited:
                print()
                if r["result"] != "success":
                    record["unsolicited"] = {
                        "enabled": True, "skipped": "punch 가 성립하지 않아 건너뛴다"}
                    print("  펀치가 성립하지 않아 요청하지 않은 인바운드 시험을 건너뛴다.")
                else:
                    record["unsolicited"] = run_unsolicited(
                        sock, peer, args.unsolicited_duration,
                        r["session"], r["peer_session"])
                    u = record["unsolicited"]
                    print()
                    if u.get("skipped"):
                        print(f"  건너뜀: {u['skipped']}")
                    else:
                        print(f"  동기화: 상대 확인 {'예' if u['sync']['peer_ready'] else '아니오'}"
                              f" ({u['sync']['sync_wait_ms']:.0f} ms 대기)")
                        print(f"  보냄 {u['sent']}, 받음 {u['recv']}, "
                              f"확인응답 {u['ack_recv']}, 인증 실패로 버림 {u['rejected']}")
                    print(f"  내 쪽 인바운드   : {u['inbound_unsolicited']}"
                          f"  ({UNSOL_NOTE[u['inbound_unsolicited']]})")
                    print(f"  상대 쪽 인바운드 : {u['peer_inbound_unsolicited']}"
                          f"  ({UNSOL_NOTE[u['peer_inbound_unsolicited']]})")
    finally:
        sock.close()

    path = save(record, args.out, args.label, args.mode)
    print()
    print(f"  기록: {path}")
    print("  RECORD-TEMPLATE.md 에 옮겨 적는다. 3회 반복이 필요하다.")
    print()
    return 0


if __name__ == "__main__":
    if sys.version_info < (3, 8):
        sys.stderr.write(
            "natprobe requires Python 3.8 or newer. Found %d.%d.\n"
            "On Windows try:  py -3 natprobe.py ...\n"
            % (sys.version_info[0], sys.version_info[1])
        )
        sys.exit(2)
    sys.exit(main())
