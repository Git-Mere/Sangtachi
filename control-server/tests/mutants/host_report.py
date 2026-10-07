"""4.6 host_report 표의 변이. 시험은 tests/test_host_report.py.

확인 경합 표와 서버 회수 표의 store 행(needs="store")을 지키는 변이는 이 목록의 뒤쪽에 있다. --store 로
돌린다. ops.Service 의 변이와 store.py 의 조건식 변이가 섞여 있다. 저장소 계층의 나머지 변이는
tests/mutants/store.py, 연산의 나머지 변이는 tests/mutants/ops.py 다.
"""

T = "tests/test_host_report.py::"
P = "controlplane/ops.py"
ST = "controlplane/store.py"


def race(row):
    return T + f"test_confirm_race[{row}]"


def reclaim(row):
    return T + f"test_reclaim_store[{row}]"


MUTANTS = [
    {"id": "reclaim-boundary-strict", "path": P,
     "old": "JOIN_REGISTER_GRACE_S * 1000 > now_ms", "new": "JOIN_REGISTER_GRACE_S * 1000 >= now_ms",
     "kills": [T + "test_reclaim[empty-at-grace]"],
     "why": "경계는 회수 쪽이다. now == J + G 를 남기는 구현"},
    {"id": "reclaim-only-at-boundary", "path": P,
     "old": "JOIN_REGISTER_GRACE_S * 1000 > now_ms", "new": "JOIN_REGISTER_GRACE_S * 1000 != now_ms",
     "kills": [T + "test_reclaim[empty-after-grace]"],
     "why": "유예가 지난 뒤에도 회수해야 한다"},
    {"id": "reclaim-grace-seconds-as-ms", "path": P,
     "old": "JOIN_REGISTER_GRACE_S * 1000 > now_ms", "new": "JOIN_REGISTER_GRACE_S > now_ms",
     "kills": [T + "test_reclaim[empty-before-grace]"],
     "why": "유예의 단위를 틀린 구현. 90초가 아니라 90ms 뒤에 회수한다"},
    {"id": "reclaim-host-too", "path": P,
     "old": "if peer_id == host_peer_id:", "new": "if False:",
     "kills": [T + "test_reclaim[host-self-empty]", T + "test_reclaim_read[candidates-not-list-host]",
               T + "test_reclaim_read[joined-at-absent-host]"],
     "why": "호스트를 회수 대상에서 빼지 않는다. 호출자 자신이 사라진다"},
    {"id": "reclaim-ignores-candidates", "path": P,
     "old": "if candidates:", "new": "if False:",
     "kills": [T + "test_reclaim[has-candidates-after-grace]",
               T + "test_reclaim_read[joined-at-absent-with-candidates]"],
     "why": "후보가 있는 피어도 회수한다. 터널이 있을 수 있는 피어는 호스트만 회수한다"},
    {"id": "reclaim-empty-list-counts-as-has", "path": P,
     "old": "if candidates:", "new": "if candidates is not None:",
     "kills": [T + "test_reclaim[empty-at-grace]", T + "test_reclaim[empty-after-grace]"],
     "why": "빈 후보 목록을 후보 있음으로 읽는다. 5.3 joined 는 candidates 가 비어 있는 것이다"},
    {"id": "reclaim-missing-joined-reclaims", "path": P,
     "old": 'return SKIP, "joined_at_unreadable"', "new": "joined_at_ms = 0",
     "kills": [T + "test_reclaim[joined-at-missing]", T + "test_reclaim_read[joined-at-not-int]"],
     "why": "판정 불가를 회수로 바꾼다"},
    {"id": "reclaim-missing-joined-silent", "path": P,
     "old": 'return SKIP, "joined_at_unreadable"', "new": "return KEEP, None",
     "kills": [T + "test_reclaim[joined-at-missing]", T + "test_reclaim_read[joined-at-not-int]"],
     "why": "판정 불가를 조용히 남긴다. 7.5 peer.reclaim_skipped 를 낼 근거가 사라진다"},
    {"id": "confirm-ignores-pair", "path": P,
     "old": "if pair is not None:\n        return False", "new": "if False:\n        return False",
     "kills": [T + "test_confirm_core[pair-exists]"],
     "why": "이미 PAIR# 가 있는 쌍도 다시 쓰려 한다"},
    {"id": "confirm-ignores-host-candidates", "path": P,
     "old": "return has_candidates(host) and has_candidates(other)", "new": "return has_candidates(other)",
     "kills": [T + "test_confirm_core[host-empty]"],
     "why": "호출자 쪽 후보를 보지 않는다. 조건은 둘 다다"},
    {"id": "confirm-ignores-other-candidates", "path": P,
     "old": "return has_candidates(host) and has_candidates(other)", "new": "return has_candidates(host)",
     "kills": [T + "test_confirm_core[other-empty]", T + "test_confirm_core[other-no-attr]",
               T + "test_confirm_core[other-absent]"],
     "why": "3번을 상대 후보 조건 없이 쓰는 구현. 확인 경합 표 마지막 행의 결함의 순수한 핵심"},
    {"id": "confirm-absent-peer-has", "path": P,
     "old": "if peer is None:\n        return False", "new": "if peer is None:\n        return True",
     "kills": [T + "test_confirm_core[other-absent]"],
     "why": "없는 피어를 후보 있음으로 읽는다"},
    {"id": "confirm-empty-list-has", "path": P,
     "old": "len(candidates) > 0", "new": "len(candidates) >= 0",
     "kills": [T + "test_confirm_core[other-empty]", T + "test_confirm_core[host-empty]"],
     "why": "빈 후보 목록을 후보 있음으로 읽는다"},
    {"id": "confirm-never", "path": P,
     "old": "return has_candidates(host) and has_candidates(other)", "new": "return False",
     "kills": [T + "test_confirm_core[both-have]"],
     "why": "확인을 한 번도 쓰지 않는다. 확인 경합 표 1행의 사전 조건이 성립해도 거짓이다"},
    {"id": "reclaim-peer-id-unread-ignored", "path": P,
     "old": 'if not _is_int(peer_id):\n        return SKIP, "peer_id_unreadable"',
     "new": 'if False:\n        return SKIP, "peer_id_unreadable"',
     "kills": [T + "test_reclaim_read[peer-id-absent]", T + "test_reclaim_read[peer-id-not-int]",
               T + "test_reclaim_read[peer-id-absent-with-candidates]"],
     "why": "peer_id 를 읽지 못해도 판정을 이어 간다. 호스트 판정이 거짓이 되어 호스트를 지울 수 있다"},
    {"id": "reclaim-candidates-absent-unreadable", "path": P,
     "old": 'peer.get("candidates", [])', "new": 'peer.get("candidates")',
     "kills": [T + "test_reclaim_read[candidates-attr-absent]"],
     "why": "candidates 속성 없음을 판독 불가로 읽는다. 없음은 빈 것이다 (6.3 조건)"},
    {"id": "reclaim-candidates-unread-ignored", "path": P,
     "old": 'if not isinstance(candidates, list):\n        return SKIP, "candidates_unreadable"',
     "new": 'if False:\n        return SKIP, "candidates_unreadable"',
     "kills": [T + "test_reclaim_read[candidates-not-list]", T + "test_reclaim_read[candidates-null]"],
     "why": "리스트가 아닌 candidates 를 참거짓으로 읽는다. null 이면 후보 없음으로 회수한다"},
    {"id": "reclaim-candidates-unread-reclaims", "path": P,
     "old": 'if not isinstance(candidates, list):\n        return SKIP, "candidates_unreadable"',
     "new": 'if not isinstance(candidates, list):\n        candidates = []',
     "kills": [T + "test_reclaim_read[candidates-not-list]", T + "test_reclaim_read[candidates-null]"],
     "why": "판정 불가를 회수로 바꾼다"},
    {"id": "reclaim-keys-unchecked", "path": P,
     "old": 'if not isinstance(peer.get("virtual_ip"), str) or not isinstance(peer.get("client_nonce"), str):',
     "new": "if False:",
     "kills": [T + "test_reclaim_read[keys-vip-absent]", T + "test_reclaim_read[keys-vip-not-str]",
               T + "test_reclaim_read[keys-nonce-absent]", T + "test_reclaim_read[keys-nonce-not-str]"],
     "why": "판정 순서 6번. 지울 키를 만들 수 없는 피어를 회수 대상으로 넘긴다"},
    {"id": "reclaim-keys-vip-only", "path": P,
     "old": 'if not isinstance(peer.get("virtual_ip"), str) or not isinstance(peer.get("client_nonce"), str):',
     "new": 'if not isinstance(peer.get("virtual_ip"), str):',
     "kills": [T + "test_reclaim_read[keys-nonce-absent]", T + "test_reclaim_read[keys-nonce-not-str]"],
     "why": "client_nonce 도 지울 키다 (NONCE#)"},
    {"id": "reclaim-keys-nonce-only", "path": P,
     "old": 'if not isinstance(peer.get("virtual_ip"), str) or not isinstance(peer.get("client_nonce"), str):',
     "new": 'if not isinstance(peer.get("client_nonce"), str):',
     "kills": [T + "test_reclaim_read[keys-vip-absent]", T + "test_reclaim_read[keys-vip-not-str]"],
     "why": "virtual_ip 도 지울 키다 (VIP#)"},
    {"id": "reclaim-keys-before-grace", "path": P,
     "old": ('    if joined_at_ms + JOIN_REGISTER_GRACE_S * 1000 > now_ms:\n        return KEEP, None\n'
             '    if not isinstance(peer.get("virtual_ip"), str) or not isinstance(peer.get("client_nonce"), str):\n'
             '        return SKIP, "keys_unreadable"\n'),
     "new": ('    if not isinstance(peer.get("virtual_ip"), str) or not isinstance(peer.get("client_nonce"), str):\n'
             '        return SKIP, "keys_unreadable"\n'
             '    if joined_at_ms + JOIN_REGISTER_GRACE_S * 1000 > now_ms:\n        return KEEP, None\n'),
     "kills": [T + "test_reclaim_read[keys-absent-before-grace]"],
     "why": "5번이 6번보다 먼저다. 유예가 남은 피어를 판정 불가로 로그에 남기지 않는다"},
    {"id": "reclaim-int-accepts-bool", "path": P,
     "old": "return isinstance(value, int) and not isinstance(value, bool)", "new": "return isinstance(value, int)",
     "kills": [T + "test_reclaim_read[peer-id-bool]", T + "test_reclaim_read[joined-at-bool]"],
     "why": "bool 을 정수로 읽는다. joined_at_ms=True 가 1ms 로 읽혀 회수된다"},

    # ---------------------------------------------------------------- store 행 (확인 경합 표, 서버 회수 표)
    {"id": "confirm-unconditional-write", "path": P,
     "old": ("            if not confirm_allowed(host, other, view.pairs.get(pair_sk(host_id, target))):\n"
             "                continue\n"
             "            result = self.store.confirm_pair(room_id, host_id, target, self._now_ms(), self._mono_ns(),\n"
             "                                             self._boot_id(), view.room[\"expires_at_ms\"])\n"),
     "new": ("            self.store.client.put_item(TableName=self.store.table, Item=self.store._values({\n"
             "                \"pk\": room_id, \"sk\": pair_sk(host_id, target), \"ready_at_wall_ms\": self._now_ms(),\n"
             "                \"ready_at_mono_ns\": self._mono_ns(), \"ready_boot_id\": self._boot_id(),\n"
             "                \"punch_delay_ms\": PUNCH_DELAY_MS, \"ttl\": storage.ttl_for(view.room[\"expires_at_ms\"])}))\n"
             "            result = storage.CONFIRMED\n"),
     "kills": [race("unconditional-write-impl"), race("h-confirm-then-p"), race("h-twice-same-p")],
     "why": "확인 경합 표 마지막 행. 3번을 조건 없이 쓰는 구현은 후보 없는 쌍에도 PAIR# 를 쓴다"},
    {"id": "confirm-no-precheck", "path": P,
     "old": "if not confirm_allowed(host, other, view.pairs.get(pair_sk(host_id, target))):\n                continue",
     "new": "if False:\n                continue",
     "kills": [race("h-confirm-then-p"), race("h-twice-same-p")],
     "why": "읽은 값으로 3번의 조건을 보지 않고 쓰기부터 시도한다. 조건이 실패해도 쓰기 용량을 쓴다 (6.3)"},
    {"id": "confirm-pair-read-ignored", "path": P,
     "old": "view.pairs.get(pair_sk(host_id, target))", "new": "None",
     "kills": [race("h-twice-same-p")],
     "why": "3번의 '그 쌍의 PAIR# 가 없으면' 을 보지 않는다. 이미 확인된 쌍에 쓰기를 다시 시도한다"},
    {"id": "confirm-before-departed", "path": P,
     "old": ("        self._release_departed(room_id, view, host_id, departed, released, removed)\n"
             "        self._reclaim(room_id, view, host_id, now, {*departed, *released}, released, removed)\n"
             "        confirmed = self._confirm(room_id, view, host, confirm, skip={*departed, *released})\n"),
     "new": ("        confirmed = self._confirm(room_id, view, host, confirm, skip=set())\n"
             "        self._release_departed(room_id, view, host_id, departed, released, removed)\n"
             "        self._reclaim(room_id, view, host_id, now, {*departed, *released}, released, removed)\n"),
     "kills": [race("departed-and-confirm-same-p")],
     "why": "2번이 3번보다 먼저다. 끝난 세션을 준비 완료로 기록하고 pair.ready 를 낸다"},
    {"id": "confirmed-includes-already", "path": P,
     "old": "if result == storage.CONFIRMED:", "new": "if result in (storage.CONFIRMED, storage.ALREADY_CONFIRMED):",
     "kills": [T + "test_confirm_race_concurrent_host_report"],
     "why": "confirmed 는 이번 호출로 PAIR# 를 새로 쓴 상대다. pair.ready 가 쌍마다 한 번이어야 한다"},
    {"id": "pair-ready-not-logged", "path": P,
     "old": "log.emit(\"INFO\", \"pair.ready\",", "new": "(lambda *x, **y: None)(\"INFO\", \"pair.ready\",",
     "kills": [race("p-then-h-confirm"), race("h-twice-same-p")],
     "why": "7.5 pair.ready 를 내지 않는다. 준비 완료 1회 기록을 로그로 볼 수 없다"},
    {"id": "response-stale-for-race", "path": P,
     "old": "        others = [p for pid, p in after.peers.items() if pid != host_id]\n",
     "new": "        after = view\n        others = [p for pid, p in after.peers.items() if pid != host_id]\n",
     "kills": [race("p-then-h-confirm"), race("departed-and-confirm-same-p")],
     "why": "5번 읽기가 쓰기 전의 것이다. 확인한 쌍이 ready: false 로, 회수한 피어가 peers 에 나간다"},
    {"id": "reclaim-released-even-if-not", "path": P,
     "old": "if self.store.reclaim_peer(room_id, target, vip, peer[\"client_nonce\"], now):",
     "new": "if self.store.reclaim_peer(room_id, target, vip, peer[\"client_nonce\"], now) or True:",
     "kills": [reclaim("registers-between-read-and-delete")],
     "why": "삭제 조건이 실패했는데 released 에 담고 peer.released 를 낸다"},
    {"id": "reclaim-skipped-by-ops", "path": P,
     "old": "elif verdict == RECLAIM:", "new": "elif verdict == RECLAIM and False:",
     "kills": [reclaim("join-retry-with-reclaimed-nonce")],
     "why": "서버 회수를 하지 않는다. 회수된 피어의 nonce 가 남아 새 참가가 되지 않는다"},
    {"id": "race-store-reclaim-ignores-candidates", "path": ST,
     "old": "(attribute_not_exists(candidates) OR size(candidates) = :zero) AND joined_at_ms <= :cutoff",
     "new": "(attribute_not_exists(candidates) OR size(candidates) >= :zero) AND joined_at_ms <= :cutoff",
     "kills": [reclaim("registers-between-read-and-delete")],
     "why": "서버 회수 표 registers-between-read-and-delete. 후보가 비었는지를 삭제 조건에 넣지 않는 구현이 이 행에서 걸린다"},
    {"id": "race-store-reclaim-keeps-nonce", "path": ST,
     "old": "            self._delete(nonce_pk(client_nonce), NONCE_SK),\n", "new": "",
     "kills": [reclaim("join-retry-with-reclaimed-nonce")],
     "why": "회수할 때 NONCE# 를 지우지 않는다. 같은 nonce 의 join_room 이 새 참가가 아니게 된다"},
{"id": "confirm-revisits-released", "path": P,
     "old": "            if target in skip:\n                continue  # 2번이 이겼다\n", "new": "",
     "kills": [race("departed-and-confirm-same-p")],
     "why": "2번이 처리한 피어를 3번이 1번의 낡은 값으로 다시 판정한다. 쓰기를 시도한다"},
]
