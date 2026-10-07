"""배포한 제어 서버에 연산 다섯을 실제로 돌린다. roadmap.md Phase 3 검증의 "배포" 묶음 가운데
연산이 권한 오류 없이 도는지와 연산별 지연을 본다.

    python tools/cp-deploy/verify.py --host 203.0.113.10 [--players 4] [--concurrent]

방 하나를 만들고 플레이어를 넣고 후보를 등록하고 호스트가 확인한 뒤 플레이어를 회수한다. 실제 테이블에
항목이 생기며 방이 만료되면 TTL 이 지운다(control_plane.md 6.5). 방 코드와 토큰은 출력하지 않는다.
"""

from __future__ import annotations

import argparse
import json
import secrets
import statistics
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from deploy import DeployError, PORT, check_host  # noqa: E402

TIMES: dict[str, list[float]] = {}
RETRIES: list[int] = []
_LOCK = threading.Lock()


def call(host: str, op: str, body: dict) -> dict:
    data = json.dumps(body).encode()
    req = urllib.request.Request(f"http://{host}:{PORT}/v1/{op}", data=data, method="POST",
                                 headers={"Content-Type": "application/json"})
    t0 = time.perf_counter()
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            payload = resp.read()
    except urllib.error.HTTPError as exc:
        payload = exc.read()
    ms = (time.perf_counter() - t0) * 1000
    with _LOCK:
        TIMES.setdefault(op, []).append(ms)
    return json.loads(payload)


def must(resp: dict, what: str) -> dict:
    if not resp.get("ok"):
        raise DeployError(f"{what}: {resp.get('error')}")
    return resp


def flow(host: str, players: int, concurrent: bool, retry: bool) -> list[str]:
    notes = []
    h = must(call(host, "create_room", {"client_nonce": secrets.token_hex(16)}), "create_room")
    room = h["room_id"]
    auth_h = {"room_id": room, "peer_id": h["peer_id"], "peer_token": h["peer_token"]}
    notes.append(f"create_room virtual_ip={h['virtual_ip']}")

    joined: list[dict] = []
    errors: list[str] = []

    def join() -> None:
        try:
            join_once()
        except Exception as exc:  # 스레드 안의 예외는 main 으로 올라가지 않는다. 모아서 판정한다
            with _LOCK:
                errors.append(f"exception:{type(exc).__name__}")

    def join_once() -> None:
        # control_plane.md 8.3: 일시 오류(internal, unavailable)면 같은 nonce 로 1초 간격, 총 3회.
        body = {"room_id": room, "client_nonce": secrets.token_hex(16)}
        for attempt in range(3 if retry else 1):
            r = call(host, "join_room", body)
            if r.get("ok") or r.get("error") not in ("internal", "unavailable"):
                break
            with _LOCK:
                RETRIES.append(attempt + 1)
            time.sleep(1)
        with _LOCK:
            (joined.append(r) if r.get("ok") else errors.append(r.get("error", "?")))

    if concurrent:
        threads = [threading.Thread(target=join) for _ in range(players)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    else:
        for _ in range(players):
            join()
    vips = sorted(p["virtual_ip"] for p in joined)
    notes.append(f"join_room x{players} ok={len(joined)} errors={errors} vips={vips}")
    if len(set(vips)) != len(vips):
        raise DeployError("가상 IP 가 겹쳤다")
    # 참가 실패를 허용하는 것은 재시도 없는 동시 참가(충돌을 일부러 보는 실험) 하나다. 그때도 internal 만이다.
    experiment = concurrent and not retry
    if errors and not (experiment and all(e == "internal" for e in errors)):
        raise DeployError(f"참가 실패: {errors}")
    if not joined:
        raise DeployError("참가한 플레이어가 없다")

    must(call(host, "register_candidate", {**auth_h, "candidates": [
        {"ip": "198.51.100.9", "port": 40000, "kind": "reflexive"}]}), "register_candidate host")
    for i, p in enumerate(joined):
        must(call(host, "register_candidate", {"room_id": room, "peer_id": p["peer_id"], "peer_token": p["peer_token"],
                                               "candidates": [{"ip": "203.0.113.7", "port": 51000 + i,
                                                               "kind": "reflexive"}]}), "register_candidate")
    for p in joined:
        r = must(call(host, "get_peers", {"room_id": room, "peer_id": p["peer_id"], "peer_token": p["peer_token"]}),
                 "get_peers before")
        if r["ready"]:
            raise DeployError("확인 전인데 ready 다")
    r = must(call(host, "host_report", {**auth_h, "confirm": [p["peer_id"] for p in joined]}), "host_report confirm")
    notes.append(f"host_report confirmed={len(r['confirmed'])} expires_in_s={r['expires_in_s']}")
    for p in joined:
        r = must(call(host, "get_peers", {"room_id": room, "peer_id": p["peer_id"], "peer_token": p["peer_token"]}),
                 "get_peers after")
        if not r["ready"]:
            raise DeployError("확인 뒤인데 ready 가 아니다")
    ids = sorted(p["peer_id"] for p in joined)
    r = must(call(host, "host_report", {**auth_h, "departed": ids}), "host_report departed")
    if sorted(r["released"]) != ids or r["peers"]:
        raise DeployError("회수가 참가한 플레이어 전부를 지우지 않았다")
    notes.append(f"host_report released={len(r['released'])} peers_left={len(r['peers'])}")
    return notes


def soak(host: str, seconds: int) -> list[str]:
    """roadmap.md Phase 3 검증의 용량 확인용 부하. 방 하나에 플레이어 넷이 들어와 확인 전까지 0.5초마다
    get_peers 를 부르고(protocol.md 11장), 호스트는 5초마다 host_report 를 부른다(4.6). 확인은 하지
    않고 seconds 동안 그 상태를 유지한 뒤 플레이어를 회수한다. 폴링이 가장 무거운 구간을 오래 만든다.
    """
    h = must(call(host, "create_room", {"client_nonce": secrets.token_hex(16)}), "create_room")
    room = h["room_id"]
    auth_h = {"room_id": room, "peer_id": h["peer_id"], "peer_token": h["peer_token"]}
    players = [must(call(host, "join_room", {"room_id": room, "client_nonce": secrets.token_hex(16)}), "join_room")
               for _ in range(4)]
    for i, p in enumerate(players):
        must(call(host, "register_candidate", {"room_id": room, "peer_id": p["peer_id"], "peer_token": p["peer_token"],
                                               "candidates": [{"ip": "203.0.113.7", "port": 51000 + i,
                                                               "kind": "reflexive"}]}), "register_candidate")
    stop = time.monotonic() + seconds
    start_utc = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())

    bad: list[str] = []
    polls = [0]

    def poll(p: dict) -> None:
        body = {"room_id": room, "peer_id": p["peer_id"], "peer_token": p["peer_token"]}
        try:
            while time.monotonic() < stop:
                r = call(host, "get_peers", body)
                with _LOCK:
                    polls[0] += 1
                    if not r.get("ok"):
                        bad.append(r.get("error", "?"))
                time.sleep(0.5)
        except Exception as exc:  # 스레드 예외는 main 으로 올라가지 않는다
            with _LOCK:
                bad.append(f"exception:{type(exc).__name__}")

    threads = [threading.Thread(target=poll, args=(p,)) for p in players]
    for t in threads:
        t.start()
    while time.monotonic() < stop:
        must(call(host, "host_report", auth_h), "host_report")
        time.sleep(5)
    for t in threads:
        t.join()
    ids = sorted(p["peer_id"] for p in players)
    r = must(call(host, "host_report", {**auth_h, "departed": ids}), "host_report departed")
    end_utc = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    if bad:
        raise DeployError(f"폴링 실패 {len(bad)}건: {sorted(set(bad))}")
    # 폴러 넷이 0.5초 간격으로 돌았다면 대략 seconds*2*4 번이다. 절반에 못 미치면 부하가 의도보다 작다.
    if polls[0] < seconds * 4:
        raise DeployError(f"폴링이 {polls[0]}번뿐이다. 의도한 부하가 아니다")
    if sorted(r["released"]) != ids or r["peers"]:
        raise DeployError("회수가 플레이어 전부를 지우지 않았다")
    return [f"soak {seconds}s polls={polls[0]} window UTC {start_utc} ~ {end_utc}"]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--players", type=int, default=4, choices=range(1, 5))
    ap.add_argument("--concurrent", action="store_true")
    ap.add_argument("--client-retry", action="store_true", help="8.3 의 클라이언트 재시도를 흉내 낸다")
    ap.add_argument("--soak", type=int, default=0, choices=range(0, 601), metavar="SECONDS",
                    help="용량 확인용 부하를 SECONDS 동안 만든다")
    ap.add_argument("--rounds", type=int, default=1, choices=range(1, 21))
    ns = ap.parse_args(argv)
    try:
        check_host(ns.host)
        if ns.soak:
            for line in soak(ns.host, ns.soak):
                print(line)
        for i in range(0 if ns.soak else ns.rounds):
            for line in flow(ns.host, ns.players, ns.concurrent, ns.client_retry):
                print(f"[{i}] {line}")
    except DeployError as exc:
        print(f"ERROR {exc}", file=sys.stderr)
        return 1
    for op, xs in sorted(TIMES.items()):
        xs = sorted(xs)
        p95 = xs[max(0, -(-len(xs) * 95 // 100) - 1)]
        print(f"latency {op}: n={len(xs)} median={statistics.median(xs):.0f}ms p95={p95:.0f}ms max={xs[-1]:.0f}ms")
    print(f"internal/unavailable 뒤 재시도 {len(RETRIES)}회")
    print("VERDICT: pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
