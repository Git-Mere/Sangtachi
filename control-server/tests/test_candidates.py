"""control_plane.md 4.4 register_candidate 의 위생 케이스 표와 거부 정책 표.

CANDIDATE_CASES 가 4.4 위생 케이스 표의 원본이고 POLICY_CASES 가 거부 정책 표의 원본이다.
문서는 이 파일을 가리킨다. 변이는 tests/mutants/candidates.py 다.

위생 거부 행("그 후보만 폐기")은 정상 후보 GOOD 하나를 앞에 같이 보낸다. 혼자 보내면 저장할 것이
0개라 마지막 행(요청 전체 bad_request)이 되어 그 행이 지키는 것을 볼 수 없다. 형 위반 행도
GOOD 을 같이 보내 "개별 후보 문제가 아니라 요청 문제" 임을 본다.
"""

import pytest

from controlplane import candidates as cmod
from controlplane.constants import MAX_CANDIDATES
from controlplane.errors import OpError


def C(ip="10.0.0.5", port=51000, kind="local", drop=()):
    c = {"ip": ip, "port": port, "kind": kind}
    for k in drop:
        del c[k]
    return c


GOOD = C("192.168.0.10", 51000, "local")


def distinct_good(n):
    return [C(f"10.0.0.{i + 1}", 51000, "local") for i in range(n)]


BAD = "bad_request"

# 출처: control_plane.md 4.4 register_candidate 의 위생 케이스 표. 문서 한 행이 여기 한 원소다.
# "A / B / C" 로 적힌 행은 대안마다 한 원소다.
#   send      보내는 candidates 값
#   expect    BAD 이면 요청 전체 bad_request. 아니면 (accepted, rejected)
#   stored    expect 가 BAD 가 아닐 때 저장돼야 하는 (ip, port, kind) 목록. 순서까지 본다. None 이면 보지 않는다
#   result    문서의 결과 열
#   note      문서의 근거 열
CANDIDATE_CASES = [
    dict(id="local-ok", send=[C("192.168.0.10", 51000, "local")], expect=(1, 0),
         stored=[("192.168.0.10", 51000, "local")], result="저장. `accepted`+1", note="정상 로컬 후보"),
    dict(id="reflexive-ok", send=[C("203.0.113.7", 51000, "reflexive")], expect=(1, 0),
         stored=[("203.0.113.7", 51000, "reflexive")], result="저장. `accepted`+1", note="정상 반사 후보"),
    dict(id="broadcast", send=[GOOD, C("255.255.255.255", 51000, "local")], expect=(1, 1),
         stored=[("192.168.0.10", 51000, "local")], result="그 후보만 폐기. `rejected`+1", note="10.1 브로드캐스트"),
    dict(id="multicast-224", send=[GOOD, C("224.0.0.1", 51000, "local")], expect=(1, 1),
         stored=[("192.168.0.10", 51000, "local")], result="그 후보만 폐기. `rejected`+1",
         note="10.1 멀티캐스트 `224.0.0.0/4`"),
    dict(id="multicast-239", send=[GOOD, C("239.255.255.250", 1900, "local")], expect=(1, 1),
         stored=[("192.168.0.10", 51000, "local")], result="그 후보만 폐기. `rejected`+1",
         note="10.1 멀티캐스트 `224.0.0.0/4`"),
    dict(id="unspecified", send=[GOOD, C("0.0.0.0", 51000, "local")], expect=(1, 1),
         stored=[("192.168.0.10", 51000, "local")], result="그 후보만 폐기. `rejected`+1", note="10.1 미지정"),
    dict(id="loopback", send=[GOOD, C("127.0.0.1", 51000, "local")], expect=(1, 1),
         stored=[("192.168.0.10", 51000, "local")], result="그 후보만 폐기. `rejected`+1",
         note="10.1 루프백. 수신 출발지 위생(7장)과 달리 후보 목록은 루프백을 거부한다"),
    dict(id="port-1", send=[C("10.0.0.5", 1, "local")], expect=(1, 0), stored=[("10.0.0.5", 1, "local")],
         result="저장. `accepted`+1",
         note="포트 범위의 양 끝은 정상이다. 상한을 `>=` 로 잘못 쓴 구현이 이 행에서 걸린다"),
    dict(id="port-65535", send=[C("10.0.0.5", 65535, "local")], expect=(1, 0), stored=[("10.0.0.5", 65535, "local")],
         result="저장. `accepted`+1",
         note="포트 범위의 양 끝은 정상이다. 상한을 `>=` 로 잘못 쓴 구현이 이 행에서 걸린다"),
    dict(id="port-0", send=[GOOD, C("10.0.0.5", 0, "local")], expect=(1, 1), stored=[("192.168.0.10", 51000, "local")],
         result="그 후보만 폐기. `rejected`+1", note="10.1 포트 0. 위생 규칙이 정한 값이라 형식 위반이 아니다"),
    dict(id="port-65536", send=[GOOD, C("10.0.0.5", 65536, "local")], expect=BAD, stored=None,
         result="요청 전체 `bad_request`", note="`port` 가 1~65535 의 JSON 정수가 아니다. 형 위반이다"),
    dict(id="port-minus-1", send=[GOOD, C("10.0.0.5", -1)], expect=BAD, stored=None,
         result="요청 전체 `bad_request`", note="`port` 가 1~65535 의 JSON 정수가 아니다. 형 위반이다"),
    dict(id="port-string", send=[GOOD, C("10.0.0.5", "51000")], expect=BAD, stored=None,
         result="요청 전체 `bad_request`", note="`port` 가 1~65535 의 JSON 정수가 아니다. 형 위반이다"),
    dict(id="ip-three-octets", send=[GOOD, C("10.0.0")], expect=BAD, stored=None,
         result="요청 전체 `bad_request`", note="`ip` 가 IPv4 점 십진 4옥텟이 아니다. 형 위반이다"),
    dict(id="ip-five-octets", send=[GOOD, C("10.0.0.5.1")], expect=BAD, stored=None,
         result="요청 전체 `bad_request`", note="`ip` 가 IPv4 점 십진 4옥텟이 아니다. 형 위반이다"),
    dict(id="ip-v6", send=[GOOD, C("::1")], expect=BAD, stored=None,
         result="요청 전체 `bad_request`", note="`ip` 가 IPv4 점 십진 4옥텟이 아니다. 형 위반이다"),
    dict(id="ip-leading-zero", send=[GOOD, C("10.000.0.5")], expect=BAD, stored=None,
         result="요청 전체 `bad_request`",
         note="`ip` 가 IPv4 점 십진 4옥텟이 아니다. 형 위반이다. 선행 0 거부 (7.1)"),
    dict(id="kind-other", send=[GOOD, C(kind="relay")], expect=BAD, stored=None,
         result="요청 전체 `bad_request`", note="`kind` 가 local/reflexive 가 아님. 형 위반이다"),
    dict(id="missing-ip", send=[GOOD, C(drop=("ip",))], expect=BAD, stored=None,
         result="요청 전체 `bad_request`", note="`ip` 가 없음. 형 위반이다"),
    dict(id="missing-port", send=[GOOD, C(drop=("port",))], expect=BAD, stored=None,
         result="요청 전체 `bad_request`", note="`port` 가 없음. 형 위반이다"),
    dict(id="missing-kind", send=[GOOD, C(drop=("kind",))], expect=BAD, stored=None,
         result="요청 전체 `bad_request`", note="`kind` 가 없음. 형 위반이다"),
    dict(id="duplicate", send=[C("10.0.0.5", 51000, "local"), C("10.0.0.5", 51000, "reflexive")],
         expect=(1, 0), stored=[("10.0.0.5", 51000, "local")], result="하나만 저장. `accepted`+1, `rejected` 는 그대로",
         note="10.1 중복 제거. `kind` 가 달라도 같은 엔드포인트다. 중복은 위생 거부가 아니므로 세지 않는다. "
              "따라서 `accepted + rejected` 가 보낸 개수보다 작을 수 있다. 먼저 온 것(kind local)을 남긴다"),
    dict(id="nine", send=distinct_good(9), expect=BAD, stored=None, result="요청 전체 `bad_request`",
         note="클라이언트가 8개를 넘겨 보내는 것은 형식 위반이다. 10.1 의 \"초과분은 버린다\" 는 수신한 목록에 "
              "대한 클라이언트 규칙이고, 서버는 보내는 쪽에 상한을 강제한다"),
    dict(id="eight", send=distinct_good(8), expect=(8, 0), stored=[(f"10.0.0.{i + 1}", 51000, "local") for i in range(8)],
         result="저장. `accepted`=8", note="상한 안쪽이다"),
    dict(id="empty", send=[], expect=BAD, stored=None, result="요청 전체 `bad_request`",
         note="후보가 없으면 등록할 것이 없다"),
    dict(id="all-rejected", send=[C("127.0.0.1", 51000, "local")], expect=BAD, stored=None,
         result="요청 전체 `bad_request`", note="저장할 후보가 0개다. 거부 정책"),
]

# 출처: control_plane.md 4.4 의 "처리 순서는 셋이다" 문단. 형 검사 -> 위생 거부 -> 중복 제거.
ORDER_CASES = [
    dict(id="duplicate-rejected-twice", send=[GOOD, C("127.0.0.1", 51000), C("127.0.0.1", 51000)],
         expect=(1, 2), stored=[("192.168.0.10", 51000, "local")], result="저장 1. `rejected`=2",
         note="protocol.md 10.1 표의 순서대로 위생 거부를 먼저 하고 남은 것에서 중복을 지운다. "
              "그래서 같은 루프백 둘은 둘 다 위생 거부로 센다. 정상 후보가 없으면 저장 0개 행이 된다"),
    dict(id="duplicate-keeps-first", send=[C("10.0.0.5", 51000, "reflexive"), C("10.0.0.5", 51000, "local")],
         expect=(1, 0), stored=[("10.0.0.5", 51000, "reflexive")], result="하나만 저장",
         note="중복이면 먼저 온 것을 남긴다. kind 도 먼저 온 것이다. duplicate 행과 순서를 뒤집어 "
              "\"늘 local 을 남긴다\" 같은 구현을 가른다"),
    dict(id="unknown-key-dropped", send=[{"ip": "10.0.0.5", "port": 51000, "kind": "local", "note": "x"}],
         expect=(1, 0), stored=[("10.0.0.5", 51000, "local")], result="저장. 모르는 키는 버린다",
         note="후보 원소의 모르는 키는 무시하고 ip/port/kind 만 저장한다. 3.3 본문의 알 수 없는 키 무시와 같은 뜻"),
    dict(id="same-ip-other-port", send=[C("10.0.0.5", 51000), C("10.0.0.5", 51001)], expect=(2, 0),
         stored=[("10.0.0.5", 51000, "local"), ("10.0.0.5", 51001, "local")], result="둘 다 저장",
         note="중복의 기준은 ip:port 다. ip 만 같으면 다른 엔드포인트다"),
    dict(id="type-before-sanitation-kind", send=[GOOD, C("127.0.0.1", 51000, "relay")], expect=BAD, stored=None,
         result="요청 전체 `bad_request`",
         note="위생 거부 대상(루프백)이어도 kind 가 형 위반이면 요청 전체 bad_request 다. 형 검사가 위생보다 먼저다"),
    dict(id="type-before-sanitation-port", send=[GOOD, C("127.0.0.1", 65536)], expect=BAD, stored=None,
         result="요청 전체 `bad_request`",
         note="위생 거부 대상(루프백)이어도 port 가 범위 밖이면 요청 전체 bad_request 다. 형 검사가 위생보다 먼저다"),
]

# 출처: control_plane.md 4.4 의 "서버가 판정하는 대역은 10.1 의 목록 그대로다" 문단.
BAND_CASES = [
    dict(id="loopback-127-8", send=[GOOD, C("127.1.2.3", 51000, "local")], expect=(1, 1),
         stored=[("192.168.0.10", 51000, "local")], result="그 후보만 폐기",
         note="루프백은 `127.0.0.0/8` 전체다 (protocol.md 10.1). 위생 표 행은 127.0.0.1 뿐이다"),
    dict(id="octet-255-ok", send=[C("10.0.0.255")], expect=(1, 0), stored=[("10.0.0.255", 51000, "local")],
         result="저장", note="서버는 피어의 prefix 를 받지 않으므로 directed broadcast 를 판정할 수 없다. 10.0.0.255 는 저장한다. 7.1 옥텟 상한 안쪽이기도 하다"),
    dict(id="zero-net-not-unspecified", send=[C("0.0.0.1")], expect=(1, 0), stored=[("0.0.0.1", 51000, "local")],
         result="저장", note="미지정은 0.0.0.0 하나다. 0.0.0.0/8 의 나머지는 10.1 목록에 없어 저장한다"),
    dict(id="below-multicast", send=[C("223.255.255.255")], expect=(1, 0),
         stored=[("223.255.255.255", 51000, "local")], result="저장", note="멀티캐스트 224.0.0.0/4 의 바로 아래"),
    dict(id="above-multicast", send=[C("240.0.0.1")], expect=(1, 0), stored=[("240.0.0.1", 51000, "local")],
         result="저장", note="240.0.0.0/4 는 10.1 목록에 없어 저장한다. 멀티캐스트 바로 위"),
]

# 문서 표에 없는 행. 각 note 에 출처를 적는다. 표의 행만으로는 지키지 못하는 규칙을 지킨다.
CANDIDATE_EXTRA_CASES = [
    dict(id="octet-256", send=[GOOD, C("10.0.0.256")], expect=BAD, stored=None,
         result="요청 전체 `bad_request`", note="7.1 각 옥텟 0~255"),
    dict(id="unicode-digits", send=[GOOD, C("\u0661\u0660.0.0.5")], expect=BAD, stored=None,
         result="요청 전체 `bad_request`", note="7.1 정규식의 \\d 를 ASCII 로 읽는다. 아랍-인도 숫자 ١٠ 은 int() 가 10 으로 받는다"),
    dict(id="ip-trailing-newline", send=[GOOD, C("10.0.0.5\n")], expect=BAD, stored=None,
         result="요청 전체 `bad_request`", note="7.1 정규식의 $ 는 끝 줄바꿈 앞에서도 맞는다. 문자열 전체가 맞아야 한다"),
    dict(id="port-bool", send=[GOOD, C("10.0.0.5", True)], expect=BAD, stored=None,
         result="요청 전체 `bad_request`", note="3.3 인코딩 \"정수는 JSON number\". JSON true 는 number 가 아니다. Python 에서 bool 은 int 의 하위형이다"),
    dict(id="port-float", send=[GOOD, C("10.0.0.5", 51000.0)], expect=BAD, stored=None,
         result="요청 전체 `bad_request`", note="3.3 인코딩 \"소수점 없음\""),
    dict(id="not-a-list", send={"ip": "10.0.0.5", "port": 51000, "kind": "local"}, expect=BAD, stored=None,
         result="요청 전체 `bad_request`", note="4.4 요청 표 `candidates` 의 형은 배열이다"),
    dict(id="candidates-missing", send=None, expect=BAD, stored=None,
         result="요청 전체 `bad_request`", note="4.4 요청 표 `candidates` 는 필수다. 본문에 없으면 None 이 들어온다"),
    dict(id="element-not-object", send=[GOOD, "10.0.0.5:51000"], expect=BAD, stored=None,
         result="요청 전체 `bad_request`", note="4.4 후보 원소는 ip/port/kind 를 가진 객체다"),
]

# 출처: control_plane.md 4.4 의 거부 정책 표. 두 행이고 각 행이 나열한 경우마다 한 원소다.
# 각 원소는 그 경우를 대표하는 CANDIDATE_CASES 의 id 를 가리킨다. 여기서는 결과의 종류만 본다.
#   ok        True 면 응답 ok 에 rejected 가 0 이 아니고, False 면 요청 전체 bad_request
POLICY_CASES = [
    dict(id="sanitation-broadcast", row="위생 거부", case="broadcast", ok=True,
         note="그 후보만 버리고 나머지를 저장한다. `rejected` 를 올리고 응답은 `ok` 다"),
    dict(id="sanitation-multicast", row="위생 거부", case="multicast-224", ok=True, note="위와 같다"),
    dict(id="sanitation-unspecified", row="위생 거부", case="unspecified", ok=True, note="위와 같다"),
    dict(id="sanitation-loopback", row="위생 거부", case="loopback", ok=True, note="위와 같다"),
    dict(id="sanitation-port-0", row="위생 거부", case="port-0", ok=True,
         note="`10.0.0.5:0` 은 형으로는 정수 `0` 이고 규칙이 그 값을 거부하는 것이라 위생 거부다"),
    dict(id="type-ip", row="형 위반", case="ip-three-octets", ok=False,
         note="요청 전체가 `bad_request` 다. 개별 후보 문제가 아니라 요청 문제다"),
    dict(id="type-port", row="형 위반", case="port-65536", ok=False,
         note="`10.0.0.5:65536` 은 범위 밖이라 형 위반이다"),
    dict(id="type-kind", row="형 위반", case="kind-other", ok=False, note="위와 같다"),
    dict(id="type-missing", row="형 위반", case="missing-ip", ok=False, note="없다. 위와 같다"),
    dict(id="type-nine", row="형 위반", case="nine", ok=False, note="목록이 9개다"),
    dict(id="type-empty", row="형 위반", case="empty", ok=False, note="목록이 비었다"),
]

_BY_ID = {c["id"]: c for c in CANDIDATE_CASES}


def _run(send):
    stored, rejected = cmod.sanitize_candidates(send)
    return stored, rejected


@pytest.mark.parametrize("case", CANDIDATE_CASES + ORDER_CASES + BAND_CASES + CANDIDATE_EXTRA_CASES,
                         ids=lambda c: c["id"])
def test_candidate(case):
    if case["expect"] == BAD:
        with pytest.raises(OpError) as exc:
            _run(case["send"])
        assert exc.value.code == BAD
        return
    stored, rejected = _run(case["send"])
    accepted_want, rejected_want = case["expect"]
    assert (len(stored), rejected) == (accepted_want, rejected_want)
    assert [(c["ip"], c["port"], c["kind"]) for c in stored] == case["stored"]
    for c in stored:
        assert set(c) == {"ip", "port", "kind"}


@pytest.mark.parametrize("case", POLICY_CASES, ids=lambda c: c["id"])
def test_policy(case):
    send = _BY_ID[case["case"]]["send"]
    if case["ok"]:
        stored, rejected = _run(send)
        assert stored and rejected > 0
    else:
        with pytest.raises(OpError) as exc:
            _run(send)
        assert exc.value.code == BAD


def test_tables_are_whole():
    # 위생 케이스 표 16행. 대안으로 나뉜 행: 224/239 (2), 1/65535 (2), 65536/-1/"51000" (3),
    # ip 넷 (4), kind 아님/ip·port·kind 없음 (4). 16 + 1 + 1 + 2 + 3 + 3 = 26.
    assert len(CANDIDATE_CASES) == 26
    ids_all = [c["id"] for c in CANDIDATE_CASES + ORDER_CASES + BAND_CASES + CANDIDATE_EXTRA_CASES]
    assert len(ids_all) == len(set(ids_all))
    assert all(p["case"] in _BY_ID for p in POLICY_CASES)
    assert len({c["id"] for c in POLICY_CASES}) == len(POLICY_CASES)


def test_max_candidates_value():
    # 2.6 상수와 protocol.md 3장. 9개/8개 행이 이 값에 기댄다.
    assert MAX_CANDIDATES == 8
