#!/usr/bin/env python3
"""natprobe 자체 검증. 의존성 없다.

    python test_natprobe.py

roadmap.md Phase 2 검증 항목 중 네트워크 없이 확인할 수 있는 것을 덮는다.
잘린 응답, 잘못된 magic cookie, 트랜잭션 ID 불일치를 크래시 없이 거부하는지 본다.
"""

import json, os, struct, sys, socket
sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parent))
import natprobe as np

txid = b"\x01" * 12
cookie = struct.pack(">I", np.STUN_COOKIE)

def resp(mapped_ip="203.0.113.7", mapped_port=54321, txid=txid, cookie=cookie,
         mtype=np.STUN_BINDING_SUCCESS, extra_attrs=b"", bad_len=None, atype=0x0020):
    xport = struct.pack(">H", mapped_port ^ (np.STUN_COOKIE >> 16))
    xip = bytes(a ^ b for a, b in zip(socket.inet_aton(mapped_ip), cookie))
    val = b"\x00\x01" + xport + xip
    body = struct.pack(">HH", atype, len(val)) + val + extra_attrs
    ln = len(body) if bad_len is None else bad_len
    return struct.pack(">HH", mtype, ln) + cookie + txid + body

cases = []
def check(name, data, want_ok, want_reason=None, want_ep=None):
    r = np.parse_binding_response(data, txid)
    ok = r["ok"] == want_ok
    if want_reason: ok = ok and r.get("reason") == want_reason
    if want_ep: ok = ok and (r["mapped"]["ip"], r["mapped"]["port"]) == want_ep
    cases.append((name, ok, r))

check("정상 XOR-MAPPED-ADDRESS", resp(), True, want_ep=("203.0.113.7", 54321))
check("앞에 알 수 없는 속성 있어도 순회", 
      resp(extra_attrs=b"") , True)
# unknown attr before xor-mapped, with padding (len 5 -> pad to 8)
unk = struct.pack(">HH", 0x8022, 5) + b"abcde" + b"\x00\x00\x00"
body_first = unk
check("패딩 있는 미지 속성 건너뛰기",
      (lambda d: d)(struct.pack(">HH", np.STUN_BINDING_SUCCESS, len(unk)+12) + cookie + txid
                    + unk + struct.pack(">HH", 0x0020, 8) + b"\x00\x01"
                    + struct.pack(">H", 54321 ^ (np.STUN_COOKIE >> 16))
                    + bytes(a ^ b for a, b in zip(socket.inet_aton("203.0.113.7"), cookie))),
      True, want_ep=("203.0.113.7", 54321))
check("트랜잭션 ID 불일치", resp(txid=b"\x02"*12), False, "txid_mismatch")
check("magic cookie 오류", resp(cookie=b"\xde\xad\xbe\xef"), False, "bad_cookie")
check("길이 필드 불일치", resp(bad_len=99), False, "length_mismatch")
check("20바이트 미만", b"\x01\x02\x03", False, "too_short")
check("헤더만", struct.pack(">HH", np.STUN_BINDING_SUCCESS, 0) + cookie + txid, False, "no_mapped_address")
check("잘린 속성", (struct.pack(">HH", np.STUN_BINDING_SUCCESS, 6) + cookie + txid
                 + struct.pack(">HH", 0x0020, 8) + b"\x00\x01"), False, "no_mapped_address")
check("IPv6 family 폐기", 
      struct.pack(">HH", np.STUN_BINDING_SUCCESS, 24) + cookie + txid
      + struct.pack(">HH", 0x0020, 20) + b"\x00\x02" + b"\x00"*18, False, "no_mapped_address")
# error response
err = struct.pack(">HH", 0x0009, 4) + b"\x00\x00\x04\x01"
check("Binding Error Response", struct.pack(">HH", np.STUN_BINDING_ERROR, len(err)) + cookie + txid + err,
      False, "error_response")

# 펀치 패킷
pkt = np.build_punch(np.PUNCH_PING, 0xDEADBEEF, 7, 123456789)
cases.append(("펀치 패킷 왕복", np.parse_punch(pkt) == (np.PUNCH_PING, 0xDEADBEEF, 7, 123456789), None))
cases.append(("펀치 길이 28바이트", np.PUNCH_LEN == 28, np.PUNCH_LEN))
cases.append(("펀치 magic 불일치 거부", np.parse_punch(b"X"*np.PUNCH_LEN) is None, None))
cases.append(("펀치 길이 불일치 거부", np.parse_punch(b"\x00"*10) is None, None))
bad_kind = struct.pack(np.PUNCH_FMT, np.PUNCH_MAGIC, 99, 1, 1, 1)
cases.append(("모르는 종류 거부", np.parse_punch(bad_kind) is None, None))
old_fmt = struct.pack(">8sBxxxIQ", b"NATPRB1\x00", 1, 7, 123456789)
cases.append(("구판(v1) 패킷 거부", np.parse_punch(old_fmt) is None, None))
cases.append(("STUN 과 펀치 첫 바이트 구분", (np.PUNCH_MAGIC[0] & 0xC0) == 0x40, None))
cases.append(("Binding Request 20바이트", len(np.build_binding_request(txid)) == 20, None))

# NAT 유무 판정
# 주소는 전부 개인과 무관한 값이다. 실측 기록의 공인 IP를 시험에 쓰지 않는다.
def srv(ip, port=1):
    return {"ok": True, "resolved_ip": "1.1.1.1", "mapped": {"ip": ip, "port": port}}
_n = np.classify_nat
c = _n("10.0.0.1", 5000, [srv("8.8.4.4", 6000), srv("8.8.4.4", 6000)])
cases.append(("사설 로컬 + 다른 공인 -> NAT 있음",
              c["behind_nat"] is True and c["local_ip_scope"] == "private", c))
c = _n("8.8.8.8", 5000, [srv("8.8.8.8", 5000), srv("8.8.8.8", 5000)])
cases.append(("로컬 엔드포인트 = 공인 엔드포인트 -> NAT 없음",
              c["behind_nat"] is False and c["local_ip_scope"] == "public", c))
c = _n("8.8.8.8", 5000, [srv("8.8.8.8", 6000)])
cases.append(("IP 는 같은데 포트가 바뀌면 NAT 없음이라고 하지 않는다",
              c["behind_nat"] is True, c))
c = _n("192.0.0.2", 5000, [srv("1.1.1.1", 6000)])
cases.append(("464XLAT 대역(RFC 7335 192.0.0.0/29)도 사설로 본다",
              c["behind_nat"] is True and c["local_ip_scope"] == "private", c))
c = _n("100.64.0.5", 5000, [srv("8.8.4.4", 6000)])
cases.append(("CGNAT 대역은 special 로 본다",
              c["behind_nat"] is True and c["local_ip_scope"] == "special", c))
c = _n("10.0.0.1", 5000, [])
cases.append(("STUN 응답이 없으면 판정 불가", c["behind_nat"] is None, c))
c = _n("not-an-ip", 5000, [srv("8.8.4.4")])
cases.append(("해석 안 되는 로컬 주소는 단정하지 않는다",
              c["behind_nat"] is None and c["local_ip_scope"] == "unknown", c))
c = _n("0.0.0.0", 5000, [srv("8.8.4.4")])
cases.append(("0.0.0.0 은 비교 불가",
              c["behind_nat"] is None and c["local_ip_scope"] == "unknown", c))
# 공인 IPv6 로 시험한다. 2001:db8::/32 는 문서용 예약 대역이라 is_global 이 거짓이고,
# 그 경우 위 가드가 먼저 걸려 표기 비교를 확인할 수 없다.
c = _n("2606:4700:4700::1111", 5000,
       [srv("2606:4700:4700:0000:0000:0000:0000:1111", 5000)])
cases.append(("IPv6 표기가 달라도 같은 주소로 본다", c["behind_nat"] is False, c))
c = _n("2001:db8::1", 5000, [srv("2001:db8::1", 5000)])
cases.append(("문서용 IPv6 대역은 NAT 없음이라고 하지 않는다", c["behind_nat"] is None, c))
c = _n("10.0.0.1", 5000, [srv("bogus", 1)])
cases.append(("관측 주소가 잘못되면 판정 불가", c["behind_nat"] is None, c))
c = _n("8.8.8.8", 5000, [srv("8.8.8.8", 5000), srv("bogus", 1)])
cases.append(("관측값 하나만 잘못돼도 버리지 않고 판정 불가로 낸다",
              c["behind_nat"] is None, c))
c = _n("8.8.8.8", 5000, [srv("8.8.8.8", 99999)])
cases.append(("관측 포트가 범위를 벗어나면 판정 불가", c["behind_nat"] is None, c))
c = _n("10.0.0.1", 5000, [srv("8.8.4.4", 6000), srv("8.8.4.4", 7000)])
cases.append(("서버마다 관측값이 다르면 NAT 있음", c["behind_nat"] is True, c))
c = _n("127.0.0.1", 5000, [srv("8.8.4.4", 6000)])
cases.append(("루프백은 외부 출발지가 아니므로 판정 불가",
              c["behind_nat"] is None and c["local_ip_scope"] == "special", c))
c = _n("10.0.0.1", 0, [srv("8.8.4.4", 6000)])
cases.append(("로컬 포트 0 은 판정 불가", c["behind_nat"] is None, c))
c = _n("10.0.0.1", "abc", [srv("8.8.4.4", 6000)])
cases.append(("로컬 포트가 숫자가 아니면 판정 불가", c["behind_nat"] is None, c))
c = _n("10.0.0.1", 5000, ["not-a-dict"])
cases.append(("서버 행이 dict 가 아니면 죽지 않고 판정 불가", c["behind_nat"] is None, c))
c = _n("10.0.0.1", 5000, [{"mapped": {"ip": "8.8.4.4", "port": 1}}])
cases.append(("ok 키가 없어도 죽지 않고 판정 불가", c["behind_nat"] is None, c))
for bad_ip in (None, 12345, ["10.0.0.1"]):
    c = _n(bad_ip, 5000, [srv("8.8.4.4", 6000)])
    cases.append((f"로컬 주소가 {type(bad_ip).__name__} 이어도 죽지 않는다",
                  c["behind_nat"] is None, c))
c = _n("10.0.0.1", 5000, [srv(12345, 6000)])
cases.append(("관측 주소가 정수면 판정 불가 (0.0.48.57 로 오해석 방지)",
              c["behind_nat"] is None, c))
cases.append(("포트로 True 를 받지 않는다", np._strict_port(True) is None, None))
cases.append(("포트로 실수를 받지 않는다", np._strict_port(5.9) is None, None))
cases.append(("포트로 '5.0' 을 받지 않는다", np._strict_port("5.0") is None, None))
cases.append(("포트로 ' 5' 를 받지 않는다", np._strict_port(" 5") is None, None))
cases.append(("포트 '65535' 는 받는다", np._strict_port("65535") == 65535, None))
cases.append(("아주 긴 숫자 문자열에 죽지 않는다", np._strict_port("1" * 9000) is None, None))
c = _n("10.0.0.1", 5000, [srv("10.0.0.1", 5000)])
cases.append(("사설 주소가 그대로 관측돼도 NAT 없음이라고 하지 않는다",
              c["behind_nat"] is None, c))
c = _n("100.64.0.5", 5000, [srv("100.64.0.5", 5000)])
cases.append(("CGNAT 주소가 그대로 관측돼도 NAT 없음이라고 하지 않는다",
              c["behind_nat"] is None, c))
for bad_obs in ("0.0.0.0", "127.0.0.1", "169.254.1.1", "203.0.113.1", "10.0.0.2"):
    c = _n("10.0.0.1", 5000, [srv(bad_obs, 6000)])
    cases.append((f"관측 주소가 공인 대역이 아니면 판정 불가: {bad_obs}",
                  c["behind_nat"] is None, c))
c = _n("10.0.0.1", 5000, [{"ok": "false", "resolved_ip": "1.1.1.1",
                           "mapped": {"ip": "8.8.4.4", "port": 1}}])
cases.append(("ok 가 문자열이면 성공으로 보지 않는다", c["behind_nat"] is None, c))
c = _n("10.0.0.1", 5000, [{"ok": False, "mapped": {"ip": "bogus", "port": 0}},
                          srv("8.8.4.4", 6000)])
cases.append(("ok=False 행은 그냥 건너뛴다", c["behind_nat"] is True, c))
for mc in ("224.0.0.1", "239.1.1.1", "ff02::1", "ff0e::1"):
    c = _n("10.0.0.1" if ":" not in mc else "fd00::1", 5000, [srv(mc, 6000)])
    cases.append((f"관측 주소가 멀티캐스트면 판정 불가: {mc}", c["behind_nat"] is None, c))
for sp in ("192.88.99.1", "192.31.196.1", "192.52.193.1", "192.175.48.1",
           "192.0.0.9", "192.0.0.10", "192.0.0.170", "192.0.0.255",
           "64:ff9b::102:304", "2001::1234", "2002::1"):
    cases.append((f"IANA 특수 목적 대역 거부: {sp}", np.is_public_unicast(sp) is False, sp))
# 표준 라이브러리의 is_global 값은 Python 판의 IANA 표에 따라 달라질 수 있다.
# 그 값을 단정하지 않고 우리 판정만 확인한다.
cases.append(("6to4 relay anycast 거부", not np.is_public_unicast("192.88.99.1"), None))
cases.append(("정상 공인 주소는 통과", np.is_public_unicast("8.8.8.8")
              and np.is_public_unicast("2606:4700:4700::1111"), None))
cases.append(("문자열이 아니면 거부", np.is_public_unicast(None) is False, None))
cases.append(("멀티캐스트 거부",
              not np._is_usable_public(__import__("ipaddress").ip_address("224.0.0.1")), None))
for doc_ip in ("192.0.2.5", "198.51.100.5", "203.0.113.5", "198.18.0.5"):
    c = _n(doc_ip, 5000, [srv("8.8.4.4", 6000)])
    cases.append((f"문서용/벤치마킹 대역은 사설로 보지 않는다: {doc_ip}",
                  c["behind_nat"] is None and c["local_ip_scope"] == "unknown", c))
c = _n("fd00::1", 5000, [srv("2606:4700:4700::1111", 6000)])
cases.append(("ULA(fc00::/7)는 사설로 본다",
              c["behind_nat"] is True and c["local_ip_scope"] == "private", c))
c = _n("169.254.10.5", 5000, [srv("8.8.4.4", 6000)])
cases.append(("링크 로컬은 외부 출발지가 아니므로 판정 불가",
              c["behind_nat"] is None and c["local_ip_scope"] == "special", c))
c = _n("10.0.0.1", 5000, [srv("8.8.4.4", 5.9)])
cases.append(("관측 포트가 실수면 판정 불가", c["behind_nat"] is None, c))


# --- check-peer CLI ---------------------------------------------------------
# `check-peer` 는 주소가 인터넷 유니캐스트인지 확인하는 독립 보조 명령이다.
# (방화벽 시험 스크립트는 상대 주소를 아예 받지 않으므로 이것을 부르지 않는다.)
# 함수만 시험하면 인자 처리, 종료 코드, 주소 노출은 확인되지 않는다.
import subprocess, sys as _sys, pathlib as _pl
_PROBE = str(_pl.Path(__file__).resolve().parent / "natprobe.py")

def cli(args, stdin=None):
    p = subprocess.run([_sys.executable, _PROBE, "check-peer", *args],
                       input=stdin, capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr

for args, stdin, want, name in [
    (["8.8.8.8"], None, 0, "위치 인자 공인 주소 통과"),
    (["10.0.0.1"], None, 2, "위치 인자 사설 주소 거부"),
    (["192.0.0.9"], None, 2, "PCP anycast 거부"),
    (["--stdin"], "8.8.8.8\n", 0, "stdin 한 줄 통과"),
    (["--stdin"], "8.8.8.8", 0, "stdin 줄바꿈 없어도 통과"),
    (["--stdin"], "8.8.8.8\n10.0.0.1\n", 2, "stdin 여러 줄 거부"),
    (["--stdin"], " 8.8.8.8 ", 2, "stdin 공백 거부"),
    (["--stdin"], "", 2, "stdin 빈 입력 거부"),
    (["--stdin", "8.8.8.8"], "8.8.8.8\n", 2, "위치 인자와 --stdin 동시 사용 거부"),
    ([], None, 2, "주소도 --stdin 도 없으면 거부"),
    (["--ipv4", "2606:4700:4700::1111"], None, 2, "--ipv4 는 IPv6 거부"),
    (["2606:4700:4700::1111"], None, 0, "--ipv4 없으면 공인 IPv6 통과"),
    (["--ipv4", "8.8.8.8"], None, 0, "--ipv4 + IPv4 통과"),
    (["Any"], None, 2, "방화벽 키워드 거부"),
    (["0.0.0.0/0"], None, 2, "CIDR 거부"),
]:
    rc, out, err = cli(args, stdin)
    cases.append((f"CLI: {name}", rc == want, (rc, out.strip(), err.strip())))

# argparse 자체 오류 경로도 주소를 되쓰면 안 된다.
for args, stdin in [(["8.8.8.8", "9.9.9.9"], None), (["--bogus", "8.8.8.8"], None),
                    (["--ipv4=8.8.8.8"], None)]:
    rc, out, err = cli(args, stdin)
    leaked = [a for a in ("8.8.8.8", "9.9.9.9") if a in out or a in err]
    cases.append((f"CLI: 인자 오류에도 주소를 남기지 않는다 {args}",
                  rc != 0 and leaked == [], (rc, leaked, err.strip()[:60])))

# 주소가 출력에 절대 남으면 안 된다. 터미널 기록과 수집 로그로 새는 경로다.
# 성공 시 stdout 이 비어 있어야 한다. 조용한 검증 명령이라는 계약이다.
for args, stdin in [(["8.8.8.8"], None), (["--stdin"], "8.8.8.8\n"),
                    (["--ipv4", "8.8.8.8"], None)]:
    rc, out, err = cli(args, stdin)
    cases.append((f"CLI: 통과 시 stdout 이 비어 있다 {args}", rc == 0 and out == "", (rc, out)))

for args, stdin in [(["8.8.8.8"], None), (["10.0.0.1"], None),
                    (["--stdin"], "8.8.8.8\n"), (["--stdin"], "10.0.0.1\n"),
                    (["--ipv4", "2606:4700:4700::1111"], None)]:
    rc, out, err = cli(args, stdin)
    leaked = [a for a in ("8.8.8.8", "10.0.0.1", "2606:4700:4700::1111")
              if a in out or a in err]
    cases.append((f"CLI: 출력에 주소를 남기지 않는다 {args}", leaked == [], leaked))

# 요청하지 않은 인바운드 시험
for k in (np.PUNCH_UNSOL, np.PUNCH_UNSOL_ACK, np.PUNCH_PHASE_READY):
    pk = np.build_punch(k, 1, 2, 3)
    cases.append((f"펀치 종류 {k} 왕복", np.parse_punch(pk) == (k, 1, 2, 3), None))
cases.append(("정의되지 않은 종류 거부",
              np.parse_punch(struct.pack(np.PUNCH_FMT, np.PUNCH_MAGIC, 99, 1, 1, 1)) is None, None))
cases.append(("종류 0 거부",
              np.parse_punch(struct.pack(np.PUNCH_FMT, np.PUNCH_MAGIC, 0, 1, 1, 1)) is None, None))
_v = np.unsolicited_verdict
# _v(recv, ack_recv, sent, peer_ready, peer_sent)
cases.append(("양방향 통과", _v(5, 5, 10, True, 10) == (True, "allowed", "allowed"), _v(5, 5, 10, True, 10)))
cases.append(("내 쪽만 통과", _v(5, 0, 10, True, 10) == (True, "allowed", "blocked"), _v(5, 0, 10, True, 10)))
cases.append(("상대 쪽만 통과", _v(0, 5, 10, True, 10) == (True, "blocked", "allowed"), _v(0, 5, 10, True, 10)))
cases.append(("동기화 성공 + 양방향 차단 -> blocked/blocked",
              _v(0, 0, 10, True, 10) == (True, "blocked", "blocked"), _v(0, 0, 10, True, 10)))
cases.append(("동기화 실패 + 아무것도 못 받음 -> unknown",
              _v(0, 0, 10, False, 0) == (False, "unknown", "unknown"), _v(0, 0, 10, False, 0)))
cases.append(("동기화 실패해도 탐침이 왔으면 증거가 된다",
              _v(5, 0, 10, False, 0) == (True, "allowed", "blocked"), _v(5, 0, 10, False, 0)))
cases.append(("내가 한 발도 못 쏘면 상대를 blocked 라고 하지 않는다",
              _v(5, 0, 0, True, 10) == (True, "allowed", "unknown"), _v(5, 0, 0, True, 10)))
cases.append(("상대가 한 발도 못 쐈으면 내 쪽을 blocked 라고 하지 않는다",
              _v(0, 5, 10, True, 0) == (True, "unknown", "allowed"), _v(0, 5, 10, True, 0)))
cases.append(("양쪽 다 못 쐈으면 둘 다 unknown",
              _v(0, 0, 0, True, 0) == (True, "unknown", "unknown"), _v(0, 0, 0, True, 0)))

# 입력 검증
def raises(fn, *a, **kw):
    try:
        fn(*a, **kw); return False
    except ValueError:
        return True

cases.append(("포트 0 거부(기본)", raises(np.valid_port, 0), None))
cases.append(("포트 0 허용(로컬 bind)", np.valid_port(0, allow_zero=True) == 0, None))
cases.append(("포트 65536 거부", raises(np.valid_port, 65536), None))
cases.append(("포트 음수 거부", raises(np.valid_port, -1), None))
cases.append(("포트 비숫자 거부", raises(np.valid_port, "abc"), None))
cases.append(("포트 65535 허용", np.valid_port("65535") == 65535, None))
cases.append(("duration NaN 거부", raises(np.valid_duration, float("nan")), None))
cases.append(("duration inf 거부", raises(np.valid_duration, float("inf")), None))
cases.append(("duration 0 거부", raises(np.valid_duration, 0), None))
cases.append(("duration 음수 거부", raises(np.valid_duration, -5), None))
cases.append(("duration 30 허용", np.valid_duration(30.0) == 30.0, None))
cases.append(("엔드포인트 파싱", np.parse_endpoint(" 127.0.0.1:65535 ") == ("127.0.0.1", 65535), None))
cases.append(("콜론 없는 엔드포인트 거부", raises(np.parse_endpoint, "127.0.0.1"), None))
cases.append(("포트 범위 밖 엔드포인트 거부", raises(np.parse_endpoint, "127.0.0.1:99999"), None))

# Windows 전용 경로
cases.append(("SIO_UDP_CONNRESET 상수", np.SIO_UDP_CONNRESET == 0x9800000C, hex(np.SIO_UDP_CONNRESET)))
_s = np.make_socket(0)
try:
    st = np._connreset_state
    if os.name == "nt":
        cases.append(("Windows: connreset 상태가 기록됨", st["disabled"] in (True, False), st))
        cases.append(("Windows: connreset off 적용", st["disabled"] is True, st))
    else:
        cases.append(("비Windows: connreset 해당 없음", st["disabled"] is None, st))
    cases.append(("소켓이 bind 되고 논블로킹", _s.getsockname()[1] > 0 and _s.gettimeout() == 0.0, None))
finally:
    _s.close()

# 결과 파일이 덮어써지지 않는다
import tempfile, pathlib as _pl
with tempfile.TemporaryDirectory() as td:
    d = _pl.Path(td)
    p1 = np.save({"a": 1}, d, "lbl", "probe")
    p2 = np.save({"a": 2}, d, "lbl", "probe")
    ok = p1 != p2 and json.loads(p1.read_text())["a"] == 1 and json.loads(p2.read_text())["a"] == 2
    cases.append(("같은 초에 저장해도 덮어쓰지 않음", ok, (p1.name, p2.name)))

# classify_mapping
mk = lambda ip, ep: {"ok": True, "resolved_ip": ip, "mapped": {"ip": ep[0], "port": ep[1]}}
c = np.classify_mapping([mk("1.1.1.1", ("9.9.9.9", 100)), mk("2.2.2.2", ("9.9.9.9", 100))], 100)
cases.append(("EIM 판정 + 포트 보존", c["mapping"] == "endpoint-independent" and c["port_preserving"], c))
c = np.classify_mapping([mk("1.1.1.1", ("9.9.9.9", 100)), mk("2.2.2.2", ("9.9.9.9", 200))], 100)
cases.append(("목적지 의존 판정", c["mapping"] == "destination-dependent", c))
c = np.classify_mapping([mk("1.1.1.1", ("9.9.9.9", 100))], 100)
cases.append(("서버 1곳이면 unknown", c["mapping"] == "unknown" and not c["mapping_confident"], c))
c = np.classify_mapping([mk("1.1.1.1", ("9.9.9.9", 100)), mk("1.1.1.1", ("9.9.9.9", 100))], 100)
cases.append(("같은 서버 IP 2회도 unknown", c["mapping"] == "unknown", c))

# ps1 파일 인코딩
# Windows PowerShell 5.1 은 BOM 없는 .ps1 을 ANSI 코드페이지(한국어 Windows 면 CP949)로
# 읽는다. 한글 주석과 문자열이 깨져 스크립트가 파싱조차 되지 않는다. 2026-09-20 에
# 실제로 이 이유로 방화벽 시험 스크립트가 실행 불가였다. 정적 리뷰 17라운드가 놓쳤다.
_ps1 = _pl.Path(__file__).resolve().parent / "unsolicited-firewall-test.ps1"
cases.append(("방화벽 시험 스크립트가 존재한다", _ps1.is_file(), str(_ps1)))
_ps1_raw = _ps1.read_bytes() if _ps1.is_file() else b""
cases.append(("ps1 이 UTF-8 BOM 으로 시작한다", _ps1_raw[:3] == b"\xef\xbb\xbf", _ps1_raw[:8]))
try:
    _ps1_raw.decode("utf-8")
    _ps1_utf8 = True
except UnicodeDecodeError:
    _ps1_utf8 = False
cases.append(("ps1 본문 전체가 UTF-8 로 해독된다", _ps1_utf8, None))

# BOM 이 있어도 파싱이 된다는 보장은 없다. 위 두 건은 원인 하나를 볼 뿐이다.
# Windows 에서는 실제 파서에 물어본다. 이것이 판정이고 위 두 건은 진단이다.
if os.name == "nt":
    import shutil as _sh, subprocess as _sp
    # 찾지 못하면 조용히 건너뛰지 않는다. 검증하지 못했다는 사실 자체가 실패다.
    # 조용히 넘어가면 파서 검증이 한 번도 안 돈 채로 전건 통과가 찍힌다.
    _psexe = _sh.which("powershell")
    cases.append(("Windows 에서 powershell 을 찾는다", _psexe is not None, _psexe))
    if _psexe:
        _lit = str(_ps1).replace("'", "''")
        _snip = ("$errs=$null; $toks=$null; "
                 "$null=[System.Management.Automation.Language.Parser]::ParseFile("
                 "'" + _lit + "',[ref]$toks,[ref]$errs); "
                 "if ($errs.Count -gt 0) { exit 1 } else { exit 0 }")
        try:
            _rc = _sp.run([_psexe, "-NoProfile", "-NonInteractive", "-Command", _snip],
                          capture_output=True, timeout=120).returncode
        except Exception as _exc:
            _rc = f"실행 실패: {_exc}"
        cases.append(("Windows PowerShell 이 ps1 을 파싱한다", _rc == 0, _rc))

# --- _timed_send_recv -------------------------------------------------------
# run_punch 와 run_unsolicited 가 공유하는 타이밍 뼈대다. 둘 다 이것에 달려 있으므로
# 여기가 틀리면 두 측정이 함께 틀린다. 실제 소켓으로 돌린다.
import time as _t

def _loop_case(duration, interval, feed=0):
    """루프를 한 번 돌리고 (송신 횟수, 수신 횟수, 경과, seq 목록) 을 낸다.

    `feed` 만큼 다른 소켓에서 패킷을 밀어 넣는다.
    """
    a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); a.bind(("127.0.0.1", 0))
    b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); b.bind(("127.0.0.1", 0))
    try:
        sends = []; recvs = 0; left = feed
        t0 = _t.monotonic()
        for ev, pay in np._timed_send_recv([a], t0, duration, interval):
            if ev == "send":
                seq, ts = pay
                sends.append((seq, ts))
                if left > 0:
                    b.sendto(b"x" * 8, a.getsockname()); left -= 1
            else:
                sock, data, src = pay
                recvs += 1
        return sends, recvs, _t.monotonic() - t0
    finally:
        a.close(); b.close()

_sends, _recvs, _dt = _loop_case(0.6, 0.1, feed=3)
cases.append(("정해진 시간 안에 끝난다", 0.55 <= _dt <= 1.2, _dt))
cases.append(("간격대로 송신 차례가 온다", 5 <= len(_sends) <= 7, len(_sends)))
cases.append(("seq 는 1부터 하나씩 는다",
              [s for s, _ in _sends] == list(range(1, len(_sends) + 1)), _sends))
cases.append(("ts_ns 는 단조 증가한다",
              all(_sends[i][1] < _sends[i + 1][1] for i in range(len(_sends) - 1)), _sends))
cases.append(("밀어 넣은 패킷이 수신 이벤트로 나온다", _recvs == 3, _recvs))

_s0, _r0, _dt0 = _loop_case(0.0, 0.1)
cases.append(("duration 0 이면 아무것도 내지 않는다",
              _s0 == [] and _r0 == 0, (_s0, _r0)))

# 간격이 남은 시간보다 길어도 첫 송신은 나온다. 나오지 않으면 짧은 측정이 조용히 빈다.
_s1, _r1, _dt1 = _loop_case(0.1, 10.0)
cases.append(("간격이 duration 보다 길어도 첫 송신은 나온다", len(_s1) == 1, _s1))

# 밀린 주기를 따라잡지 않는다.
#
# 앞의 두 번만 일부러 느리게 처리해 예정 시각을 크게 밀어 둔다. 그다음부터는 바로
# 반환한다. 따라잡기를 하면 밀린 만큼을 몰아서 내보내므로 **간격이 0 에 가까운 송신이
# 연달아** 나온다. 따라잡지 않으면 느린 구간이 끝나는 즉시 간격이 제자리로 돌아온다.
# 송신 횟수로는 구분되지 않는다. 느린 처리 자체가 횟수를 눌러 버리기 때문이다.
def _catchup_gaps():
    a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); a.bind(("127.0.0.1", 0))
    try:
        stamps = []
        t0 = _t.monotonic()
        for ev, pay in np._timed_send_recv([a], t0, 1.0, 0.05):
            if ev == "send":
                stamps.append(_t.monotonic())
                if len(stamps) <= 2:
                    _t.sleep(0.3)   # 간격의 6배. 예정 시각이 크게 밀린다
        return [stamps[i + 1] - stamps[i] for i in range(2, len(stamps) - 1)]
    finally:
        a.close()

_gaps = _catchup_gaps()
cases.append(("밀린 주기를 따라잡지 않는다",
              bool(_gaps) and min(_gaps) >= 0.02, _gaps[:6]))


# --- run_punch 실동작 -------------------------------------------------------
# 루프프백으로 상대를 흉내 내 한 번 돌린다. 함수 단위 시험만으로는 송신 분기와 수신 분기가
# 이어 붙는 자리를 확인할 수 없다. 크로스 모델 리뷰가 바로 그 자리를 결함으로 지목했고,
# 이 케이스가 있었으면 한 번에 반박됐다.
import threading as _th

def _punch_roundtrip():
    peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); peer.bind(("127.0.0.1", 0))
    mine = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); mine.bind(("127.0.0.1", 0))
    stop = _th.Event()

    def peer_loop():
        peer.settimeout(0.1)
        while not stop.is_set():
            try:
                data, src = peer.recvfrom(2048)
            except OSError:
                continue
            p = np.parse_punch(data)
            if p and p[0] == np.PUNCH_PING:
                _k, sess, seq, ts = p
                peer.sendto(np.build_punch(np.PUNCH_PONG, sess, seq, ts), src)
                peer.sendto(np.build_punch(np.PUNCH_PING, 0xABCD, seq, ts), src)

    t = _th.Thread(target=peer_loop, daemon=True)
    t.start()
    try:
        return np.run_punch(mine, peer.getsockname(), 0.8)
    finally:
        stop.set(); t.join(timeout=1); peer.close(); mine.close()

_pr = _punch_roundtrip()
cases.append(("run_punch: 양방향이면 success", _pr["result"] == "success", _pr["result"]))
cases.append(("run_punch: 송신 분기 뒤에도 수신을 처리한다",
              _pr["sent"] >= 3 and _pr["recv_pong"] >= 3, (_pr["sent"], _pr["recv_pong"])))
cases.append(("run_punch: 상대 세션은 상대 PING 에서만 얻는다",
              _pr["peer_session"] == 0xABCD, _pr["peer_session"]))
cases.append(("run_punch: RTT 표본이 쌓인다",
              (_pr["rtt_ms"] or {}).get("samples", 0) >= 3, _pr["rtt_ms"]))


fail = 0
for name, ok, detail in cases:
    print(("PASS  " if ok else "FAIL  ") + name)
    if not ok:
        fail += 1
        print("        ", detail)
print()
print(f"{len(cases)-fail}/{len(cases)} 통과")
sys.exit(1 if fail else 0)
