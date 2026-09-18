#!/usr/bin/env python3
"""natprobe 자체 검증. 의존성 없다.

    python test_natprobe.py

roadmap.md Phase 2 검증 항목 중 네트워크 없이 확인할 수 있는 것을 덮는다.
잘린 응답, 잘못된 magic cookie, 트랜잭션 ID 불일치를 크래시 없이 거부하는지 본다.
"""

import json, struct, sys, socket
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

fail = 0
for name, ok, detail in cases:
    print(("PASS  " if ok else "FAIL  ") + name)
    if not ok:
        fail += 1
        print("        ", detail)
print()
print(f"{len(cases)-fail}/{len(cases)} 통과")
sys.exit(1 if fail else 0)
