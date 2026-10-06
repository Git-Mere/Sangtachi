"""4.4 위생 케이스 표와 거부 정책 표의 변이. 실행기는 tests/mutate.py 다."""

P = "controlplane/candidates.py"
T = "tests/test_candidates.py::test_candidate"
POL = "tests/test_candidates.py::test_policy"


def k(*ids):
    return [f"{T}[{i}]" for i in ids]


def kp(*ids):
    return [f"{POL}[{i}]" for i in ids]


MUTANTS = [
    dict(id="no-broadcast", path=P, old="if octets == (255, 255, 255, 255):", new="if False:",
         kills=k("broadcast") + kp("sanitation-broadcast"), why="브로드캐스트 거부를 지운다"),
    dict(id="multicast-low", path=P, old="if 224 <= first <= 239:", new="if 225 <= first <= 239:",
         kills=k("multicast-224") + kp("sanitation-multicast"), why="멀티캐스트 하한을 한 칸 올린다"),
    dict(id="multicast-high", path=P, old="if 224 <= first <= 239:", new="if 224 <= first <= 238:",
         kills=k("multicast-239"), why="멀티캐스트 상한을 한 칸 내린다"),
    dict(id="no-unspecified", path=P, old="if octets == (0, 0, 0, 0):", new="if False:",
         kills=k("unspecified") + kp("sanitation-unspecified"), why="미지정 주소 거부를 지운다"),
    dict(id="no-loopback", path=P, old="if first == 127:", new="if False:",
         kills=k("loopback", "loopback-127-8", "all-rejected") + kp("sanitation-loopback"),
         why="루프백 거부를 지운다. 전부 거부 행도 루프백 1개로 만든 것이라 같이 떨어진다"),
    dict(id="loopback-host-only", path=P, old="if first == 127:", new="if octets == (127, 0, 0, 1):",
         kills=k("loopback-127-8"), why="10.1 의 127.0.0.0/8 을 127.0.0.1 하나로 좁힌다"),
    dict(id="no-port-zero", path=P, old="if port == 0:", new="if False:",
         kills=k("port-0") + kp("sanitation-port-0"), why="포트 0 위생 거부를 지운다"),
    dict(id="port-zero-is-type", path=P, old="not 0 <= port <= 65535", new="not 1 <= port <= 65535",
         kills=k("port-0") + kp("sanitation-port-0"),
         why="포트 0 을 형 위반(요청 전체 bad_request)으로 바꾼다. 거부 정책의 경계를 어긴다"),
    dict(id="port-high-exclusive", path=P, old="not 0 <= port <= 65535", new="not 0 <= port < 65535",
         kills=k("port-65535"), why="상한을 >= 로 잘못 쓴 구현"),
    dict(id="port-high-open", path=P, old="not 0 <= port <= 65535", new="not 0 <= port <= 65536",
         kills=k("port-65536") + kp("type-port"), why="상한을 한 칸 넓힌다"),
    dict(id="port-low-open", path=P, old="not 0 <= port <= 65535", new="not -1 <= port <= 65535",
         kills=k("port-minus-1"), why="하한을 한 칸 넓힌다"),
    dict(id="port-zero-wide", path=P, old="if port == 0:", new="if port <= 1:",
         kills=k("port-1"), why="포트 하한 끝(1)을 위생 거부로 넓힌다"),
    dict(id="port-no-type", path=P, old="if not _is_json_int(port) or not", new="if not",
         kills=k("port-string", "missing-port", "port-float"),
         why="정수 형 검사를 지운다. 문자열은 비교에서 OpError 가 아닌 예외로 떨어지고 소수는 통과한다"),
    dict(id="port-bool-is-int", path=P, old="return type(value) is int", new="return isinstance(value, int)",
         kills=k("port-bool"), why="JSON true 를 정수 1 로 받는다"),
    dict(id="ip-no-leading-zero-check", path=P, old='if len(part) > 1 and part.startswith("0"):',
         new="if False:", kills=k("ip-leading-zero"), why="선행 0 거부를 지운다"),
    dict(id="ip-three-ok", path=P, old=r"[0-9]{1,3}(?:\.[0-9]{1,3}){3}", new=r"[0-9]{1,3}(?:\.[0-9]{1,3}){2,3}",
         kills=k("ip-three-octets") + kp("type-ip"), why="옥텟 3개를 받는다. 그 결과 4번째 옥텟 접근에서 OpError 가 아닌 예외가 난다"),
    dict(id="ip-prefix-match", path=P, old="_DOTTED.fullmatch(text) is None", new="_DOTTED.match(text) is None",
         kills=k("ip-five-octets"), why="전체 일치를 앞부분 일치로 바꾼다. 10.0.0.5.1 의 앞부분이 맞는다"),
    dict(id="ip-dollar", path=P, old="_DOTTED.fullmatch(text) is None",
         new='__import__("re").match(_DOTTED.pattern + "$", text) is None',
         kills=k("ip-trailing-newline"), why="문서 정규식을 ^...$ 로 그대로 옮긴 구현. $ 가 끝 줄바꿈을 받는다"),
    dict(id="ip-unicode-digit", path=P, old=r"[0-9]{1,3}(?:\.[0-9]{1,3}){3}", new=r"\d{1,3}(?:\.\d{1,3}){3}",
         kills=k("unicode-digits"), why="문서 정규식의 \\d 를 Python 에 그대로 쓴다. 유니코드 숫자를 받는다"),
    dict(id="ip-no-regex", path=P, old="if not isinstance(text, str) or _DOTTED.fullmatch(text) is None:",
         new="if not isinstance(text, str):", kills=k("ip-v6", "ip-three-octets", "ip-five-octets"),
         why="형식 검사를 지운다"),
    dict(id="ip-octet-256", path=P, old="if value > 255:", new="if value > 256:",
         kills=k("octet-256"), why="옥텟 상한을 넓힌다"),
    dict(id="ip-octet-255", path=P, old="if value > 255:", new="if value >= 255:",
         kills=k("octet-255-ok", "broadcast"), why="옥텟 상한을 >= 로 잘못 쓴다. 255 가 형 위반이 된다"),
    dict(id="ip-no-type", path=P, old="if not isinstance(text, str) or _DOTTED", new="if _DOTTED",
         kills=k("missing-ip") + kp("type-missing"), why="ip 문자열 형 검사를 지운다. 없는 ip(None)가 OpError 가 아닌 예외로 떨어진다"),
    dict(id="kind-any", path=P, old="if not isinstance(kind, str) or kind not in KINDS:", new="if False:",
         kills=k("kind-other", "missing-kind") + kp("type-kind"), why="kind 검사를 지운다"),
    dict(id="too-many-off-by-one", path=P, old="if len(raw) > MAX_CANDIDATES:", new="if len(raw) > MAX_CANDIDATES + 1:",
         kills=k("nine") + kp("type-nine"), why="상한을 한 칸 넓힌다"),
    dict(id="too-many-inclusive", path=P, old="if len(raw) > MAX_CANDIDATES:", new="if len(raw) >= MAX_CANDIDATES:",
         kills=k("eight"), why="상한을 >= 로 잘못 쓴다"),
    dict(id="client-truncate", path=P, old='raise OpError("bad_request", "후보가 너무 많다")',
         new="raw = raw[:MAX_CANDIDATES]", kills=k("nine"),
         why="10.1 의 클라이언트 규칙(초과분은 버린다)을 서버에 적용한다"),
    dict(id="zero-stored-ok", path=P, old="if not stored:", new="if False:",
         kills=k("empty", "all-rejected") + kp("type-empty"), why="저장 0개 bad_request 를 지운다"),
    dict(id="no-dedupe", path=P, old="if key in seen:", new="if False:",
         kills=k("duplicate"), why="ip:port 중복 제거를 지운다"),
    dict(id="dedupe-counts-rejected", path=P, old="if key in seen:\n            continue",
         new="if key in seen:\n            rejected += 1\n            continue",
         kills=k("duplicate"), why="중복을 위생 거부로 센다"),
    dict(id="dedupe-by-kind", path=P, old="key = (octets, port)", new='key = (octets, port, item["kind"])',
         kills=k("duplicate"), why="kind 가 다르면 다른 후보로 본다"),
    dict(id="sanitation-is-type", path=P, old="            rejected += 1\n            continue\n        key",
         new='            raise OpError("bad_request", "x")\n        key',
         kills=k("broadcast", "multicast-224", "multicast-239", "unspecified", "loopback", "port-0")
         + kp("sanitation-broadcast", "sanitation-multicast", "sanitation-unspecified", "sanitation-loopback",
              "sanitation-port-0"),
         why="위생 거부를 요청 전체 bad_request 로 바꾼다. 거부 정책의 두 결과를 섞는다"),
    dict(id="bad-ip-skipped", path=P, old='raise OpError("bad_request", "ip 는 점 십진 IPv4 다")', new="continue",
         kills=k("ip-three-octets", "ip-five-octets", "ip-v6", "ip-leading-zero") + kp("type-ip"),
         why="ip 형 위반을 그 후보만 버리는 것으로 바꾼다. 요청 전체 bad_request 규칙을 어긴다"),
    dict(id="bad-kind-skipped", path=P, old='raise OpError("bad_request", "kind 는 local 이나 reflexive 다")',
         new="continue", kills=k("kind-other", "missing-kind") + kp("type-kind"),
         why="kind 형 위반을 그 후보만 버리는 것으로 바꾼다"),
    dict(id="bad-port-skipped", path=P, old='raise OpError("bad_request", "port 는 1~65535 정수다")',
         new="continue", kills=k("port-65536", "port-minus-1", "port-string", "missing-port") + kp("type-port"),
         why="port 형 위반을 그 후보만 버리는 것으로 바꾼다"),
    dict(id="no-list-check", path=P, old="if not isinstance(raw, list):", new="if False:",
         kills=k("candidates-missing"),
         why="배열 형 검사를 지운다. 없는 candidates(None)가 OpError 가 아닌 예외로 떨어진다"),
    dict(id="no-object-check", path=P, old="if not isinstance(item, dict):", new="if False:",
         kills=k("element-not-object"), why="후보 원소의 객체 형 검사를 지운다"),
    dict(id="private-predicate", path=P, old="if first == 127:",
         new='if __import__("ipaddress").ip_address(".".join(map(str, octets))).is_private:',
         kills=k("local-ok", "port-1", "port-65535", "eight"),
         why="7.1 이 금한 ipaddress 술어로 루프백을 가른다. is_private 는 사설 대역도 참이다"),
    dict(id="global-predicate", path=P, old="if 224 <= first <= 239:",
         new='if not __import__("ipaddress").ip_address(".".join(map(str, octets))).is_global:',
         kills=k("local-ok", "reflexive-ok"),
         why="7.1 이 금한 ipaddress 술어로 거부한다. 203.0.113.7 은 문서용 대역이라 is_global 이 거짓이다"),
    dict(id="kinds-no-reflexive", path=P, old='KINDS = ("local", "reflexive")', new='KINDS = ("local",)',
         kills=k("reflexive-ok"), why="kind 허용 목록에서 reflexive 를 뺀다"),
    dict(id="dedupe-before-sanitation", path=P,
         old="""        if _sanitation_reject(octets, port):
            rejected += 1
            continue
        key = (octets, port)
        if key in seen:
            continue
        seen.add(key)
""",
         new="""        key = (octets, port)
        if key in seen:
            continue
        seen.add(key)
        if _sanitation_reject(octets, port):
            rejected += 1
            continue
""",
         kills=k("duplicate-rejected-twice"),
         why="중복 제거를 위생 거부보다 먼저 한다. 같은 루프백 둘이 rejected 1 이 된다 (10.1 순서를 뒤집는다)"),
    dict(id="dedupe-keeps-last", path=P, old="""        if key in seen:
            continue
""",
         new="""        if key in seen:
            stored = [c for c in stored if (c["ip"], c["port"]) != (item["ip"], port)]
""",
         kills=k("duplicate", "duplicate-keeps-first"), why="중복이면 나중 것을 남긴다"),
    dict(id="store-whole-item", path=P, old='stored.append({"ip": item["ip"], "port": port, "kind": item["kind"]})',
         new="stored.append(dict(item))", kills=k("unknown-key-dropped"),
         why="원소를 통째로 저장해 모르는 키까지 남긴다"),
    dict(id="dedupe-by-ip-only", path=P, old="key = (octets, port)", new="key = (octets, None)",
         kills=k("same-ip-other-port"), why="ip 만으로 중복을 지운다. port 가 다르면 다른 엔드포인트다"),
    dict(id="unspecified-whole-0-8", path=P, old="if octets == (0, 0, 0, 0):", new="if first == 0:",
         kills=k("zero-net-not-unspecified"), why="미지정을 0.0.0.0/8 전체로 넓힌다"),
    dict(id="multicast-low-wide", path=P, old="if 224 <= first <= 239:", new="if 223 <= first <= 239:",
         kills=k("below-multicast"), why="멀티캐스트 하한을 한 칸 내린다"),
    dict(id="multicast-to-240", path=P, old="if 224 <= first <= 239:", new="if 224 <= first <= 240:",
         kills=k("above-multicast"), why="멀티캐스트를 240 까지 넓힌다 (240.0.0.0/4 를 거부한다)"),
    dict(id="sanitation-before-type", path=P,
         old="""            raise OpError("bad_request", "ip 는 점 십진 IPv4 다")
""",
         new="""            raise OpError("bad_request", "ip 는 점 십진 IPv4 다")
        if _sanitation_reject(octets, 1):
            out.append((octets, item))
            continue
""",
         kills=k("type-before-sanitation-kind", "type-before-sanitation-port"),
         why="위생 거부 대상을 먼저 걸러 port/kind 형 검사를 건너뛴다. 형 검사가 위생보다 먼저라는 순서를 어긴다"),
]
