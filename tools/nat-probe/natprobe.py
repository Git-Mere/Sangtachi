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
PUNCH_INTERVAL_S = 0.2  # protocol.md 의 HELLO 재전송 간격과 같다
PUNCH_DEFAULT_DURATION_S = 30


def _utcnow_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


# ---------------------------------------------------------------------------
# 소켓
# ---------------------------------------------------------------------------


def make_socket(port: int) -> socket.socket:
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
        # 끄지 않으면 ICMP port unreachable 수신 시 recvfrom 이 WSAECONNRESET 로
        # 깨진다. 펀치 중에는 상대가 아직 포트를 열기 전이라 이 ICMP 가 정상적으로
        # 발생한다.
        sio = getattr(socket, "SIO_UDP_CONNRESET", 0x9800000C)
        try:
            sock.ioctl(sio, 0)
        except OSError as exc:
            print(f"[warn] SIO_UDP_CONNRESET off 실패: {exc}", file=sys.stderr)

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
    if kind not in (PUNCH_PING, PUNCH_PONG):
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
    first_inbound_ms = None
    first_inbound_seq = None
    rtts: list = []
    sources: dict = {}
    other = 0

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
        "result": result,
    }


# ---------------------------------------------------------------------------
# 출력
# ---------------------------------------------------------------------------

MAPPING_NOTE = {
    "endpoint-independent": "모든 STUN 서버가 같은 공인 엔드포인트를 봤다. 홀펀칭 가능성이 높다.",
    "destination-dependent": "서버마다 다른 공인 엔드포인트를 봤다. 대칭형 거동이다. blocker 20 위험.",
    "unknown": "서로 다른 IP 의 서버 2곳에서 응답을 받지 못해 판정할 수 없다.",
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
    default_out = Path(__file__).resolve().parent / "results"

    ap = argparse.ArgumentParser(
        description="NAT 매핑 거동과 UDP 홀펀칭 실측 (design-audit blocker 20)")
    sub = ap.add_subparsers(dest="mode", required=True)

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

    args = ap.parse_args(argv)

    try:
        local_port = valid_port(args.port, allow_zero=True)
        if args.mode == "punch":
            valid_duration(args.duration)
            if args.peer:  # 5초짜리 STUN 을 돌리기 전에 먼저 거른다
                parse_endpoint(args.peer)
        servers = DEFAULT_STUN_SERVERS
        if args.stun:
            servers = []
            for item in args.stun:
                host, _, port = item.rpartition(":")
                if not host:
                    raise ValueError(f"--stun 은 HOST:PORT 형식이어야 한다: {item!r}")
                servers.append((host, valid_port(port)))
    except ValueError as exc:
        ap.error(str(exc))

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
        },
        "stun": None,
        "punch": None,
    }

    print()
    print(f"natprobe {args.mode}  label={args.label}")
    print(f"  로컬 엔드포인트 {local_ip}:{local_port}  (이 소켓 하나만 쓴다)")
    print()

    try:
        record["stun"] = run_stun(sock, servers, local_port)
        print_stun_summary(record["stun"])

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
    finally:
        sock.close()

    path = save(record, args.out, args.label, args.mode)
    print()
    print(f"  기록: {path}")
    print("  RECORD-TEMPLATE.md 에 옮겨 적는다. 3회 반복이 필요하다.")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
