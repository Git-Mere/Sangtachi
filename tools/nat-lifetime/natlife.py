#!/usr/bin/env python3
"""natlife: NAT UDP 매핑 수명 측정 도구. 표준 라이브러리만 쓴다. Python 3.8+.

    python natlife.py serve --port 47000              # EC2. 측정할 때만 띄운다
    python natlife.py run --server IP:47000           # 측정할 클라이언트

재는 것은 "그 경로의 필터+매핑 수명" 이다. 계약, 판정 규칙, 케이스 표는 README.md 가 갖는다.

와이어 형식과 응답기의 반사 방지 규칙도 README.md 가 갖는다.
"""

import argparse
import heapq
import json
import math
import os
import select
import socket
import struct
import sys
import time

MAGIC = b"NLT1"
T_REQ, T_ACK, T_LATE = 1, 2, 3
REQ_LEN, ACK_LEN, LATE_LEN = 64, 24, 28
ACK_NONE, ACK_SCHEDULED, ACK_REFUSED = 0, 1, 2
MAX_TRIALS = 64
MAX_DELAY_MS_WIRE = 0xFFFFFFFF
NONCE_LEN = 16

DEFAULT_DELAYS = "5,20,30,45,60,90,120,150,180,240,300"
DEFAULT_MAX_DELAY_S = 600
DEFAULT_MAX_PENDING_PER_IP = 64
DEFAULT_MAX_PENDING = 1024
DEFAULT_SERVE_DURATION_S = 3600


# ---------------------------------------------------------------- 와이어

def pack_req(seq, nonce, delay_ms):
    return MAGIC + struct.pack(">BBH", T_REQ, 0, seq) + nonce + struct.pack(">I", delay_ms) + b"\x00" * 36


def pack_ack(seq, nonce, status=ACK_NONE):
    return MAGIC + struct.pack(">BBH", T_ACK, status, seq) + nonce


def pack_late(seq, nonce, idle_ms):
    return MAGIC + struct.pack(">BBH", T_LATE, 0, seq) + nonce + struct.pack(">I", idle_ms)


def parse(data):
    """길이, magic, type 이 정확히 맞을 때만 dict. 아니면 None."""
    if len(data) < 8 or data[:4] != MAGIC:
        return None
    mtype, flag, seq = struct.unpack(">BBH", data[4:8])
    want = {T_REQ: REQ_LEN, T_ACK: ACK_LEN, T_LATE: LATE_LEN}.get(mtype)
    if want is None or len(data) != want:
        return None
    # 둘째 바이트는 ACK 에서만 상태다. 나머지는 0 이어야 한다.
    if mtype == T_ACK:
        if flag not in (ACK_NONE, ACK_SCHEDULED, ACK_REFUSED):
            return None
    elif flag != 0:
        return None
    nonce = data[8:8 + NONCE_LEN]
    out = {"type": mtype, "seq": seq, "nonce": nonce}
    if mtype == T_ACK:
        out["status"] = flag
    if mtype == T_REQ:
        (out["delay_ms"],) = struct.unpack(">I", data[24:28])
        if data[28:REQ_LEN] != b"\x00" * (REQ_LEN - 28):
            return None
    elif mtype == T_LATE:
        (out["idle_ms"],) = struct.unpack(">I", data[24:28])
    return out


# ---------------------------------------------------------------- 응답기

class Responder:
    """REQ 에 ACK 를 바로 보내고, delay_ms 가 있으면 ACK 를 보낸 시각부터 그만큼 뒤에 LATE 를 보낸다.

    시계와 송신을 주입받아 네트워크 없이 시험한다.
    """

    def __init__(self, send, clock, max_delay_ms, max_pending_per_ip, max_pending, max_req_per_s=200, log=None):
        self.send = send
        self.clock = clock
        self.max_delay_ms = max_delay_ms
        self.max_pending_per_ip = max_pending_per_ip
        self.max_pending = max_pending
        self.max_req_per_s = max_req_per_s
        self.log = log or (lambda event, **kw: None)
        self.heap = []  # (due, n, addr, seq, nonce, ack_at)
        self.n = 0
        self.per_ip = {}
        self.window = None
        self.window_count = 0
        # 한 건씩 로그를 남기지 않고 센다. 남이 보낸 쓰레기가 로그를 채우지 않게 한다.
        self.counts = {"ignored": 0, "rate_limited": 0, "refused_delay": 0, "refused_pending": 0}

    def on_datagram(self, data, addr):
        msg = parse(data)
        if msg is None or msg["type"] != T_REQ:
            self.counts["ignored"] += 1
            return
        sec = int(self.clock())
        if sec != self.window:
            self.window, self.window_count = sec, 0
        if self.window_count >= self.max_req_per_s:
            self.counts["rate_limited"] += 1
            return
        self.window_count += 1
        delay = msg["delay_ms"]
        ip = addr[0]
        if delay == 0:
            status = ACK_NONE
        elif delay > self.max_delay_ms:
            status = ACK_REFUSED
            self.counts["refused_delay"] += 1
        elif self.per_ip.get(ip, 0) >= self.max_pending_per_ip or len(self.heap) >= self.max_pending:
            status = ACK_REFUSED
            self.counts["refused_pending"] += 1
        else:
            status = ACK_SCHEDULED
        self.send(pack_ack(msg["seq"], msg["nonce"], status), addr)
        if status != ACK_SCHEDULED:
            return
        # 유휴의 기점은 ACK 를 보낸 뒤다. 송신이 늦어도 유휴가 짧아지지 않는다.
        now = self.clock()
        self.n += 1
        heapq.heappush(self.heap, (now + delay / 1000.0, self.n, addr, msg["seq"], msg["nonce"], now))
        self.per_ip[ip] = self.per_ip.get(ip, 0) + 1
        self.log("scheduled", seq=msg["seq"], delay_ms=delay)

    def next_due(self):
        return self.heap[0][0] if self.heap else None

    def fire_due(self):
        now = self.clock()
        while self.heap and self.heap[0][0] <= now:
            due, _, addr, seq, nonce, ack_at = heapq.heappop(self.heap)
            idle_ms = int(round((now - ack_at) * 1000))
            self.send(pack_late(seq, nonce, idle_ms), addr)
            self.per_ip[addr[0]] -= 1
            if self.per_ip[addr[0]] == 0:
                del self.per_ip[addr[0]]
            self.log("late_sent", seq=seq, idle_ms=idle_ms)


def serve(args):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", args.port))
    end = time.monotonic() + args.duration

    def log(event, **kw):
        # 클라이언트 주소는 남기지 않는다. 측정에 필요 없고 기록이 공개될 수 있다.
        print(json.dumps(dict(t=round(time.monotonic(), 3), event=event, **kw)), flush=True)

    r = Responder(lambda d, a: sock.sendto(d, a), time.monotonic,
                  int(args.max_delay * 1000), args.max_pending_per_ip, args.max_pending,
                  args.max_req_per_s, log)
    log("listening", port=sock.getsockname()[1], duration_s=args.duration)
    while True:
        now = time.monotonic()
        if now >= end:
            log("stopped", reason="duration", **r.counts)
            return 0
        due = r.next_due()
        timeout = end - now if due is None else max(0.0, min(due, end) - now)
        ready, _, _ = select.select([sock], [], [], timeout)
        if ready:
            try:
                data, addr = sock.recvfrom(2048)
            except OSError:
                continue
            r.on_datagram(data, addr)
        r.fire_due()


# ---------------------------------------------------------------- 판정

def judge(trials):
    """시행 목록에서 수명의 구간을 낸다. 규칙과 케이스 표는 README.md "판정" 이다.

    trial: {"delay_s": float, "final_acked": bool, "scheduled": bool, "warmup_acked": int, "late": bool}
    """
    if not trials:
        return {"verdict": "unknown", "reason": "no_trials"}
    ordered = sorted(trials, key=lambda t: t["delay_s"])
    control = ordered[0]
    if not valid(control):
        return {"verdict": "unknown", "reason": "control_invalid"}
    if not control["late"]:
        return {"verdict": "unknown", "reason": "control_missing"}
    used = [t for t in ordered if valid(t)]
    excluded = [t["delay_s"] for t in ordered if not valid(t)]
    arrived = [t["delay_s"] for t in used if t["late"]]
    missing = [t["delay_s"] for t in used if not t["late"]]
    if missing and min(missing) <= max(arrived):
        return {"verdict": "unknown", "reason": "non_monotonic", "excluded": excluded}
    return {"verdict": "bounded" if missing else "at_least",
            "lower_s": max(arrived),
            "upper_s": min(missing) if missing else None,
            "excluded": excluded}


def valid(trial):
    """마지막 ACK 가 LATE 예약을 알렸고 데우기 ACK 가 하나 이상이다. 거절된 시행은 NAT 과 무관하다."""
    return bool(trial["final_acked"]) and trial["scheduled"] and trial["warmup_acked"] >= 1


# ---------------------------------------------------------------- 클라이언트

def parse_delays(text):
    out = []
    for part in text.split(","):
        v = float(part)
        if not math.isfinite(v) or not 1 <= round(v * 1000) <= MAX_DELAY_MS_WIRE:
            raise ValueError("지연은 1ms 이상의 유한한 초다")
        out.append(v)
    if len(set(out)) != len(out):
        raise ValueError("지연이 겹친다")
    if len(out) > MAX_TRIALS:
        raise ValueError(f"시행은 {MAX_TRIALS}개까지다")
    return sorted(out)


def measure(server, delays, warmup_count, warmup_interval, grace, clock=time.monotonic):
    """지연마다 소켓 하나. 모두 같은 시각에 데우고, 같은 시각에 마지막 요청을 보낸다.

    LATE 는 그 시행의 창(마지막 ACK 를 받은 시각부터 T + grace) 안에 와야 센다.
    """
    if len(delays) > MAX_TRIALS:
        raise ValueError(f"시행은 {MAX_TRIALS}개까지다")
    trials = []
    try:
        for d in delays:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            trials.append({"delay_s": d, "sock": s, "nonce": os.urandom(NONCE_LEN),
                           "warmup_sent": 0, "warmup_acked": 0, "final_acked": False,
                           "scheduled": False, "final_ack_at": None, "late": False,
                           "late_after_s": None, "late_outside_window": False,
                           "server_idle_ms": None, "ignored": 0, "send_errors": 0})
            s.bind(("0.0.0.0", 0))
            s.setblocking(False)
        _measure(trials, server, delays, warmup_count, warmup_interval, grace, clock)
    finally:
        for t in trials:
            t["sock"].close()
    for t in trials:
        del t["sock"], t["nonce"], t["final_ack_at"]
    return trials


def _measure(trials, server, delays, warmup_count, warmup_interval, grace, clock):
    by_fd = {t["sock"].fileno(): t for t in trials}
    start = clock()
    final_at = start + warmup_count * warmup_interval
    # 보낼 일정. (시각, 순번, 시행, seq, delay_ms)
    plan = []
    for n, t in enumerate(trials):
        for i in range(warmup_count):
            plan.append((start + i * warmup_interval, n, t, i, 0))
        plan.append((final_at, n, t, warmup_count, int(round(t["delay_s"] * 1000))))
    plan.sort(key=lambda p: (p[0], p[1]))
    # 끝은 시행마다의 창이 모두 닫힌 뒤다. 마지막 ACK 가 늦게 오면 그 시행의 창도 늦게 닫힌다.
    # 마지막 ACK 가 없는 시행은 어차피 무효이므로 보낸 시각 기준으로만 기다린다.
    end = final_at + max(delays) + grace
    pi = 0
    while True:
        now = clock()
        while pi < len(plan) and plan[pi][0] <= now:
            _, _, t, seq, delay_ms = plan[pi]
            try:
                t["sock"].sendto(pack_req(seq, t["nonce"], delay_ms), server)
                if delay_ms == 0:
                    t["warmup_sent"] += 1
            except OSError:
                t["send_errors"] += 1
            pi += 1
        if now >= end:
            break
        nxt = plan[pi][0] if pi < len(plan) else end
        ready, _, _ = select.select([t["sock"] for t in trials], [], [], max(0.0, min(nxt, end) - now))
        for s in ready:
            t = by_fd[s.fileno()]
            try:
                data, addr = s.recvfrom(2048)
            except OSError:
                continue
            at = clock()
            msg = parse(data)
            if addr != server or msg is None or msg["nonce"] != t["nonce"]:
                t["ignored"] += 1
                continue
            if msg["type"] == T_ACK and msg["seq"] < warmup_count:
                t["warmup_acked"] += 1
            elif msg["type"] == T_ACK and msg["seq"] == warmup_count and not t["final_acked"]:
                t["final_acked"] = True
                t["scheduled"] = msg["status"] == ACK_SCHEDULED
                t["final_ack_at"] = at
                end = max(end, at + t["delay_s"] + grace)
            elif msg["type"] == T_LATE and msg["seq"] == warmup_count and not t["late"]:
                if t["final_ack_at"] is None:
                    # 마지막 ACK 가 없으면 유휴의 기점이 없다. 그 시행은 어차피 무효다.
                    t["ignored"] += 1
                    continue
                after = at - t["final_ack_at"]
                if after > t["delay_s"] + grace:
                    t["late_outside_window"] = True
                    continue
                t["late"] = True
                t["late_after_s"] = round(after, 3)
                t["server_idle_ms"] = msg["idle_ms"]
            else:
                t["ignored"] += 1


def run(args):
    host, _, port = args.server.rpartition(":")
    server = (socket.gethostbyname(host), int(port))
    delays = parse_delays(args.delays)
    total = args.warmup_count * args.warmup_interval + max(delays) + args.grace
    print(f"측정 시작. 시행 {len(delays)}개, 약 {int(total)}초 걸린다. 이 동안 이 기기의 망을 바꾸지 않는다.",
          flush=True)
    started = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    trials = measure(server, delays, args.warmup_count, args.warmup_interval, args.grace)
    verdict = judge(trials)
    result = {"tool": "natlife", "label": args.label, "started_utc": started,
              "params": {"delays_s": delays, "warmup_count": args.warmup_count,
                         "warmup_interval_s": args.warmup_interval, "grace_s": args.grace},
              "trials": trials, "verdict": verdict}
    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, f"natlife-{args.label}-{started.replace(':', '')}.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(result, f, ensure_ascii=False, indent=2)
    for t in trials:
        print(f"  {t['delay_s']:>6g}s  데움 {t['warmup_acked']}/{t['warmup_sent']}  "
              f"마지막ACK {'예' if t['final_acked'] else '아니오'}"
              f"{'' if t['scheduled'] or not t['final_acked'] else '(거절)'}  LATE {'도착' if t['late'] else '없음'}",
              flush=True)
    print("판정:", json.dumps(verdict, ensure_ascii=False))
    print("결과 파일:", path)
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(prog="natlife")
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("serve")
    s.add_argument("--port", type=int, required=True)
    s.add_argument("--duration", type=float, default=DEFAULT_SERVE_DURATION_S)
    s.add_argument("--max-delay", type=float, default=DEFAULT_MAX_DELAY_S)
    s.add_argument("--max-pending-per-ip", type=int, default=DEFAULT_MAX_PENDING_PER_IP)
    s.add_argument("--max-pending", type=int, default=DEFAULT_MAX_PENDING)
    s.add_argument("--max-req-per-s", type=int, default=200)
    r = sub.add_parser("run")
    r.add_argument("--server", required=True)
    r.add_argument("--delays", default=DEFAULT_DELAYS)
    r.add_argument("--warmup-count", type=int, default=6)
    r.add_argument("--warmup-interval", type=float, default=1.0)
    r.add_argument("--grace", type=float, default=5.0)
    r.add_argument("--label", default=socket.gethostname())
    r.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "results"))
    a = p.parse_args(argv)
    return serve(a) if a.cmd == "serve" else run(a)


if __name__ == "__main__":
    sys.exit(main())
