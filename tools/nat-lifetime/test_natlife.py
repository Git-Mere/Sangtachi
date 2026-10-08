#!/usr/bin/env python3
"""natlife 자체 검증. 의존성 없다. 네트워크는 루프백만 쓴다.

    python test_natlife.py

판정 케이스 표(JUDGE_CASES)는 README.md "판정" 의 표와 같은 내용이다.
"""

import socket
import sys
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import natlife as nl  # noqa: E402


def T(delay, late, final=True, warm=6, scheduled=True):
    return {"delay_s": delay, "late": late, "final_acked": final, "warmup_acked": warm,
            "scheduled": scheduled}


# (이름, 시행, 기대). 기대는 judge 결과의 부분집합이다.
JUDGE_CASES = [
    ("J1 모두 도착", [T(5, True), T(60, True), T(300, True)],
     {"verdict": "at_least", "lower_s": 300, "upper_s": None}),
    ("J2 경계", [T(5, True), T(20, True), T(30, True), T(45, False), T(60, False)],
     {"verdict": "bounded", "lower_s": 30, "upper_s": 45}),
    ("J3 대조군만 도착", [T(5, True), T(20, False), T(30, False)],
     {"verdict": "bounded", "lower_s": 5, "upper_s": 20}),
    ("J4 대조군 없음", [T(5, False), T(20, True)],
     {"verdict": "unknown", "reason": "control_missing"}),
    ("J5 대조군 무효(마지막 ACK 없음)", [T(5, True, final=False), T(20, True)],
     {"verdict": "unknown", "reason": "control_invalid"}),
    ("J6 대조군 무효(데움 ACK 0)", [T(5, True, warm=0), T(20, True)],
     {"verdict": "unknown", "reason": "control_invalid"}),
    ("J7 비단조", [T(5, True), T(30, False), T(60, True)],
     {"verdict": "unknown", "reason": "non_monotonic"}),
    ("J8 무효 시행은 빼고 본다", [T(5, True), T(30, True), T(45, False, final=False), T(60, False)],
     {"verdict": "bounded", "lower_s": 30, "upper_s": 60, "excluded": [45]}),
    ("J9 무효 시행이 비단조를 가리지 않는다", [T(5, True), T(30, False), T(45, True, final=False), T(60, True)],
     {"verdict": "unknown", "reason": "non_monotonic"}),
    ("J10 시행 없음", [], {"verdict": "unknown", "reason": "no_trials"}),
    ("J11 입력 순서와 무관", [T(60, False), T(5, True), T(30, True)],
     {"verdict": "bounded", "lower_s": 30, "upper_s": 60}),
    ("J12 대조군은 가장 짧은 지연", [T(20, True), T(10, False), T(30, True)],
     {"verdict": "unknown", "reason": "control_missing"}),
    ("J13 같은 지연이 도착과 없음", [T(5, True), T(30, True), T(30, False)],
     {"verdict": "unknown", "reason": "non_monotonic"}),
    ("J14 대조군이 거절됐다", [T(5, False, scheduled=False), T(20, True)],
     {"verdict": "unknown", "reason": "control_invalid"}),
    ("J15 거절된 시행은 NAT 만료가 아니다", [T(5, True), T(30, True), T(60, False, scheduled=False)],
     {"verdict": "at_least", "lower_s": 30, "upper_s": None, "excluded": [60]}),
]


class Judge(unittest.TestCase):
    def test_cases(self):
        for name, trials, want in JUDGE_CASES:
            with self.subTest(name):
                got = nl.judge(trials)
                for k, v in want.items():
                    self.assertEqual(got.get(k), v, f"{name}: {k} {got}")


class Wire(unittest.TestCase):
    nonce = bytes(range(16))

    def test_lengths_and_no_amplification(self):
        req = nl.pack_req(7, self.nonce, 30000)
        self.assertEqual(len(req), nl.REQ_LEN)
        self.assertEqual(len(nl.pack_ack(7, self.nonce)), nl.ACK_LEN)
        self.assertEqual(len(nl.pack_late(7, self.nonce, 1)), nl.LATE_LEN)
        # REQ 하나가 낳는 응답 전체(ACK 하나와 LATE 하나)가 REQ 보다 짧다.
        self.assertLess(nl.ACK_LEN + nl.LATE_LEN, nl.REQ_LEN)

    def test_ack_status(self):
        for st in (nl.ACK_NONE, nl.ACK_SCHEDULED, nl.ACK_REFUSED):
            self.assertEqual(nl.parse(nl.pack_ack(1, self.nonce, st))["status"], st)
        bad = nl.pack_ack(1, self.nonce)
        self.assertIsNone(nl.parse(bad[:5] + b"\x03" + bad[6:]))
        late = nl.pack_late(1, self.nonce, 5)
        self.assertIsNone(nl.parse(late[:5] + b"\x01" + late[6:]), "LATE 의 둘째 바이트는 0 이다")

    def test_roundtrip(self):
        self.assertEqual(nl.parse(nl.pack_req(7, self.nonce, 30000)),
                         {"type": 1, "seq": 7, "nonce": self.nonce, "delay_ms": 30000})
        self.assertEqual(nl.parse(nl.pack_late(3, self.nonce, 45000))["idle_ms"], 45000)

    def test_rejects(self):
        req = nl.pack_req(7, self.nonce, 30000)
        bad = [
            ("짧음", req[:-1]), ("김", req + b"\x00"), ("magic", b"XLT1" + req[4:]),
            ("type 0", req[:4] + b"\x00" + req[5:]), ("type 9", req[:4] + b"\x09" + req[5:]),
            ("예약 바이트", req[:5] + b"\x01" + req[6:]), ("꼬리 0 아님", req[:-1] + b"\x01"),
            ("꼬리 앞쪽 0 아님", req[:28] + b"\x01" + req[29:]),
            ("ACK 길이로 온 REQ", req[:nl.ACK_LEN]), ("빈 것", b""),
        ]
        for name, data in bad:
            with self.subTest(name):
                self.assertIsNone(nl.parse(data))


class FakeClock:
    def __init__(self):
        self.t = 100.0

    def __call__(self):
        return self.t


class ResponderTest(unittest.TestCase):
    nonce = b"n" * 16
    A = ("198.51.100.7", 40000)
    B = ("198.51.100.8", 40000)

    def setUp(self):
        self.clock = FakeClock()
        self.sent = []
        self.r = nl.Responder(lambda d, a: self.sent.append((nl.parse(d), a)), self.clock,
                              max_delay_ms=600000, max_pending_per_ip=2, max_pending=3, max_req_per_s=50)

    def test_ack_then_late_after_delay_from_ack(self):
        self.r.on_datagram(nl.pack_req(6, self.nonce, 30000), self.A)
        self.assertEqual([(m["type"], m["seq"], m["status"], a) for m, a in self.sent],
                         [(nl.T_ACK, 6, nl.ACK_SCHEDULED, self.A)])
        self.clock.t += 29.999
        self.r.fire_due()
        self.assertEqual(len(self.sent), 1)
        self.clock.t += 0.001
        self.r.fire_due()
        m, a = self.sent[-1]
        self.assertEqual((m["type"], m["seq"], m["idle_ms"], a), (nl.T_LATE, 6, 30000, self.A))
        self.r.fire_due()
        self.assertEqual(len(self.sent), 2, "LATE 는 하나뿐이다")

    def test_zero_delay_is_ack_only(self):
        self.r.on_datagram(nl.pack_req(0, self.nonce, 0), self.A)
        self.clock.t += 1000
        self.r.fire_due()
        self.assertEqual([m["status"] for m, _ in self.sent], [nl.ACK_NONE])

    def test_delay_cap(self):
        self.r.on_datagram(nl.pack_req(1, self.nonce, 600001), self.A)
        self.assertEqual(self.sent[-1][0]["status"], nl.ACK_REFUSED)
        self.assertIsNone(self.r.next_due())
        self.r.on_datagram(nl.pack_req(2, self.nonce, 600000), self.A)
        self.assertEqual(self.sent[-1][0]["status"], nl.ACK_SCHEDULED)
        self.assertIsNotNone(self.r.next_due(), "상한과 같은 값은 받는다")

    def test_idle_starts_after_ack_send(self):
        def slow_send(d, a):
            self.sent.append((nl.parse(d), a))
            self.clock.t += 2.0  # 느린 송신
        self.r.send = slow_send
        self.r.on_datagram(nl.pack_req(1, self.nonce, 10000), self.A)
        self.clock.t = 100.0 + 2.0 + 9.999
        self.r.fire_due()
        self.assertEqual(len(self.sent), 1, "유휴는 ACK 를 보낸 뒤부터 센다")
        self.clock.t += 0.001
        self.r.fire_due()
        self.assertEqual(self.sent[-1][0]["type"], nl.T_LATE)

    def test_rate_limit(self):
        for i in range(60):
            self.r.on_datagram(nl.pack_req(i, self.nonce, 0), self.A)
        self.assertEqual(len(self.sent), 50)
        self.assertEqual(self.r.counts["rate_limited"], 10)
        self.clock.t += 1
        self.r.on_datagram(nl.pack_req(99, self.nonce, 0), self.A)
        self.assertEqual(len(self.sent), 51, "다음 초에는 다시 받는다")

    def test_pending_caps(self):
        for i in range(3):
            self.r.on_datagram(nl.pack_req(i, self.nonce, 1000), self.A)  # A 는 둘까지
        self.r.on_datagram(nl.pack_req(9, self.nonce, 1000), self.B)      # 전체 셋째
        self.r.on_datagram(nl.pack_req(10, self.nonce, 1000), self.B)     # 전체 상한
        self.assertEqual(len(self.r.heap), 3)
        self.assertEqual([m["status"] for m, _ in self.sent],
                         [1, 1, 2, 1, 2], "상한에 걸리면 거절을 알리는 ACK 가 나간다")
        self.clock.t += 1
        self.r.fire_due()
        self.assertEqual(self.r.per_ip, {})

    def test_reply_goes_only_to_source(self):
        self.r.on_datagram(nl.pack_req(1, self.nonce, 10), self.B)
        self.clock.t += 1
        self.r.fire_due()
        self.assertTrue(all(a == self.B for _, a in self.sent))

    def test_ignores_non_req(self):
        logged = []
        self.r.log = lambda event, **kw: logged.append(event)
        for d in (nl.pack_ack(1, self.nonce), nl.pack_late(1, self.nonce, 5), b"junk"):
            self.r.on_datagram(d, self.A)
        self.assertEqual(self.sent, [])
        self.assertEqual(logged, [], "버린 것은 한 건씩 로그에 남기지 않는다")
        self.assertEqual(self.r.counts["ignored"], 3)


class Loopback(unittest.TestCase):
    """실제 소켓으로 끝까지. NAT 만료는 보내는 함수가 LATE 를 버리는 것으로 흉내 낸다."""

    def run_with(self, send_filter, delays, max_delay_ms=5000, grace=0.3):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", 0))
        sock.settimeout(0.02)

        def send(d, a):
            send_filter(sock, d, a)

        r = nl.Responder(send, time.monotonic, max_delay_ms=max_delay_ms,
                         max_pending_per_ip=64, max_pending=1024)
        stop = threading.Event()

        def loop():
            while not stop.is_set():
                try:
                    data, addr = sock.recvfrom(2048)
                    r.on_datagram(data, addr)
                except socket.timeout:
                    pass
                r.fire_due()

        th = threading.Thread(target=loop, daemon=True)
        th.start()
        try:
            return nl.measure(sock.getsockname(), delays, warmup_count=3,
                              warmup_interval=0.05, grace=grace)
        finally:
            stop.set()
            th.join()
            sock.close()

    def test_end_to_end(self):
        def expire_long(sock, d, a):
            m = nl.parse(d)
            if m["type"] == nl.T_LATE and m["idle_ms"] >= 700:
                return  # NAT 이 닫혔다
            sock.sendto(d, a)

        trials = self.run_with(expire_long, [0.1, 0.3, 0.8])
        got = {t["delay_s"]: (t["warmup_acked"], t["final_acked"], t["scheduled"], t["late"]) for t in trials}
        self.assertEqual(got, {0.1: (3, True, True, True), 0.3: (3, True, True, True),
                               0.8: (3, True, True, False)})
        for t in trials:
            if t["late"]:
                self.assertGreaterEqual(t["late_after_s"], t["delay_s"] - 0.02)
        v = nl.judge(trials)
        self.assertEqual((v["verdict"], v["lower_s"], v["upper_s"]), ("bounded", 0.3, 0.8))

    def test_refused_is_not_expiry(self):
        trials = self.run_with(lambda s, d, a: s.sendto(d, a), [0.1, 0.3, 0.8], max_delay_ms=500)
        t8 = [t for t in trials if t["delay_s"] == 0.8][0]
        self.assertEqual((t8["final_acked"], t8["scheduled"], t8["late"]), (True, False, False))
        v = nl.judge(trials)
        self.assertEqual((v["verdict"], v["lower_s"], v["excluded"]), ("at_least", 0.3, [0.8]))

    def test_wrong_nonce_is_not_late(self):
        def corrupt(sock, d, a):
            m = nl.parse(d)
            if m["type"] == nl.T_LATE:
                d = nl.pack_late(m["seq"], b"x" * 16, m["idle_ms"])
            sock.sendto(d, a)

        trials = self.run_with(corrupt, [0.1])
        self.assertEqual((trials[0]["final_acked"], trials[0]["late"], trials[0]["ignored"]), (True, False, 1))

    def test_window_follows_late_final_ack(self):
        slow = {}

        def slow_path(sock, d, a):
            # 긴 지연 시행의 경로만 마지막 ACK 부터 0.2초 늦다. ACK 도 LATE 도 늦게 닿는다.
            m = nl.parse(d)
            if m["type"] == nl.T_ACK and m["status"] == nl.ACK_SCHEDULED:
                slow.setdefault("order", []).append(a)
            order = slow.get("order", [])
            if len(order) >= 2 and a == order[1]:
                threading.Timer(0.2, sock.sendto, (d, a)).start()
                return
            sock.sendto(d, a)

        trials = self.run_with(slow_path, [0.05, 0.3], grace=0.1)
        # LATE 는 마지막 데우기 뒤 0.5초에 닿는다. 고정된 끝(0.3 + 0.1)으로 자르면 놓친다.
        self.assertEqual([t["late"] for t in trials], [True, True])

    def test_late_outside_window_is_not_counted(self):
        def hold_short(sock, d, a):
            m = nl.parse(d)
            if m["type"] == nl.T_LATE and m["idle_ms"] < 200:
                threading.Timer(0.4, sock.sendto, (d, a)).start()  # 창(0.1 + 0.1)을 넘겨 도착
                return
            sock.sendto(d, a)

        trials = self.run_with(hold_short, [0.1, 0.6], grace=0.1)
        t1 = trials[0]
        self.assertEqual((t1["late"], t1["late_outside_window"]), (False, True))


class Delays(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(nl.parse_delays("30,5,20"), [5.0, 20.0, 30.0])
        too_many = ",".join(str(i) for i in range(1, nl.MAX_TRIALS + 2))
        for bad in ("0,5", "-1", "5,5", "x", "inf", "nan", "0.0004", "4294968", too_many):
            with self.subTest(bad):
                with self.assertRaises(ValueError):
                    nl.parse_delays(bad)


if __name__ == "__main__":
    unittest.main(verbosity=1)
