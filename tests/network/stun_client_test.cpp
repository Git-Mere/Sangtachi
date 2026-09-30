#include "sangtachi/network/stun_client.hpp"

#include "sangtachi/counters.hpp"
#include "sangtachi/network/endpoint.hpp"
#include "sangtachi/network/stun.hpp"
#include "sangtachi/protocol_constants.hpp"
#include "sangtachi/timer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// 시험 케이스 이름은 ASCII 로만 적는다. 이유는 network/wsa_test.cpp 머리에 있다.
//
// **시계도 소켓도 주입한다.** 실제로 500ms 를 기다리는 케이스를 쓰지 않는다. 시각은
// 인자이고, 타이머 집합은 이 파일이 직접 돌린다. 한 바퀴가 즉시 끝나는 루프와 같다
// (concurrency.md 3장 루프 한 바퀴).

using sangtachi::Counter;
using sangtachi::Counters;
using sangtachi::Millis;
using sangtachi::TimerSet;
using sangtachi::TimerTick;
using sangtachi::network::Endpoint;
using sangtachi::network::kStunDeadlineMs;
using sangtachi::network::kStunHeaderSize;
using sangtachi::network::peek_transaction_id;
using sangtachi::network::resolve_stun_servers;
using sangtachi::network::StunServerName;
using sangtachi::network::StunClient;
using sangtachi::network::StunPhase;
using sangtachi::network::StunServer;
using sangtachi::network::stun_stage_cap_ms;
using sangtachi::network::TransactionId;

namespace {

// TEST-NET-3 (RFC 5737). 실제로 보내지 않으므로 주소는 표시용이다.
Endpoint server_endpoint(std::uint8_t index) {
    return Endpoint(0xCB007100u + index, 3478);
}

std::vector<StunServer> make_servers(std::size_t count) {
    std::vector<StunServer> servers;
    for (std::size_t i = 0; i < count; ++i) {
        servers.push_back(StunServer{"stun" + std::to_string(i) + ".example:3478",
                                     server_endpoint(static_cast<std::uint8_t>(i))});
    }
    return servers;
}

struct SentDatagram {
    Endpoint to;
    std::vector<std::byte> bytes;
    Millis at = 0;
};

void write_u16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::byte>(value & 0xFFu));
}

void write_u32(std::vector<std::byte>& out, std::uint32_t value) {
    out.push_back(static_cast<std::byte>((value >> 24) & 0xFFu));
    out.push_back(static_cast<std::byte>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::byte>(value & 0xFFu));
}

void write_header(std::vector<std::byte>& out, std::uint16_t type, std::uint16_t length,
                  const TransactionId& id) {
    write_u16(out, type);
    write_u16(out, length);
    write_u32(out, sangtachi::protocol::kStunCookie);
    for (const std::byte b : id) {
        out.push_back(b);
    }
}

// Binding Success Response 한 통. XOR-MAPPED-ADDRESS 하나를 싣는다 (RFC 5389).
std::vector<std::byte> success_response(const TransactionId& id, const Endpoint& mapped) {
    std::vector<std::byte> out;
    write_header(out, 0x0101, 12, id);
    write_u16(out, 0x0020);  // XOR-MAPPED-ADDRESS
    write_u16(out, 8);
    out.push_back(std::byte{0x00});  // 예약
    out.push_back(std::byte{0x01});  // family IPv4
    write_u16(out, static_cast<std::uint16_t>(
                       mapped.port() ^ ((sangtachi::protocol::kStunCookie >> 16) & 0xFFFFu)));
    write_u32(out, mapped.address() ^ sangtachi::protocol::kStunCookie);
    return out;
}

// Binding Error Response 한 통. ERROR-CODE 하나를 싣는다.
std::vector<std::byte> error_response(const TransactionId& id, std::uint8_t error_class,
                                      std::uint8_t number) {
    std::vector<std::byte> out;
    write_header(out, 0x0111, 8, id);
    write_u16(out, 0x0009);  // ERROR-CODE
    write_u16(out, 4);
    out.push_back(std::byte{0x00});
    out.push_back(std::byte{0x00});
    out.push_back(static_cast<std::byte>(error_class));
    out.push_back(static_cast<std::byte>(number));
    return out;
}

// 성공 응답인데 family 가 IPv4 가 아니다 (protocol.md 13장 주소군).
std::vector<std::byte> ipv6_family_response(const TransactionId& id) {
    std::vector<std::byte> out;
    write_header(out, 0x0101, 12, id);
    write_u16(out, 0x0020);
    write_u16(out, 8);
    out.push_back(std::byte{0x00});
    out.push_back(std::byte{0x02});  // IPv6
    write_u16(out, 0);
    write_u32(out, 0);
    return out;
}

// 루프 한 자리를 대신한다. 시각과 난수를 우리가 정하고, 송신을 가로채고, 타이머를
// 직접 돌린다.
//
// **난수도 주입한다.** 진짜 CSPRNG 를 쓰면 "두 트랜잭션 ID 가 겹치지 않는다" 가 확률
// 명제가 되고, 실패 경로를 시험할 수 없다. 여기서는 호출 순서를 그대로 첫 바이트에
// 적는다. n 번째 요청의 ID 는 0xA0+n 으로 시작하고 나머지는 0 이다.
class Harness {
public:
    explicit Harness(std::size_t server_count, std::size_t random_failure_at = 0) {
        client_ = std::make_unique<StunClient>(
            make_servers(server_count),
            [this](const Endpoint& to, std::span<const std::byte> payload) {
                sent.push_back(SentDatagram{
                    to, std::vector<std::byte>(payload.begin(), payload.end()), now});
                return send_ok;
            },
            timers, counters,
            [this, random_failure_at](std::span<std::byte> out) {
                ++random_calls;
                if (random_failure_at != 0 && random_calls == random_failure_at) {
                    return false;
                }
                for (std::byte& b : out) {
                    b = std::byte{0x00};
                }
                if (!out.empty()) {
                    out[0] = static_cast<std::byte>(0xA0u + random_calls);
                }
                return true;
            });
    }

    // n 번째(1부터) 요청에 쓰인 트랜잭션 ID. 위 난수원이 내는 값이다.
    static TransactionId nth_transaction(std::size_t n) {
        TransactionId id{};
        id[0] = static_cast<std::byte>(0xA0u + n);
        return id;
    }

    StunClient& client() { return *client_; }

    // 목표 시각까지 타이머를 차례로 돌린다. 만료 시각을 그대로 now 로 쓴다.
    void advance_to(Millis target) {
        while (true) {
            const auto deadline = timers.earliest_deadline();
            if (!deadline || *deadline > target) {
                break;
            }
            now = *deadline;
            timers.run_expired(now, [this](const TimerTick& tick) {
                if (StunClient::owns_timer(tick.name)) {
                    client_->on_timer(tick.name, now);
                }
            });
        }
        now = target;
    }

    void deliver(std::span<const std::byte> payload) {
        client_->on_datagram(server_endpoint(0), payload, now);
    }

    // index 번째로 나간 요청의 트랜잭션 ID.
    TransactionId transaction_of(std::size_t index) const {
        REQUIRE(index < sent.size());
        const auto id = peek_transaction_id(sent[index].bytes);
        REQUIRE(id.has_value());
        return *id;
    }

    // 그 엔드포인트로 나간 요청 수.
    std::size_t count_to(const Endpoint& to) const {
        std::size_t count = 0;
        for (const auto& item : sent) {
            if (item.to == to) {
                ++count;
            }
        }
        return count;
    }

    std::vector<Millis> times_to(const Endpoint& to) const {
        std::vector<Millis> times;
        for (const auto& item : sent) {
            if (item.to == to) {
                times.push_back(item.at);
            }
        }
        return times;
    }

    TimerSet timers;
    Counters counters;
    std::vector<SentDatagram> sent;
    Millis now = 0;
    bool send_ok = true;
    std::size_t random_calls = 0;

private:
    std::unique_ptr<StunClient> client_;
};

const Endpoint kMapped(0xC6336407u, 51000);  // 198.51.100.7:51000

}  // namespace

TEST_CASE("stun_client: start queries the first two servers at once", "[stun_client]") {
    // architecture.md 3.5 기동 입력의 서버 선택. 앞 두 서버에 같은 소켓으로 동시에 질의한다.
    Harness h(4);
    REQUIRE(h.client().start(0));
    REQUIRE(h.client().phase() == StunPhase::Running);

    REQUIRE(h.sent.size() == 2);
    REQUIRE(h.sent[0].to == server_endpoint(0));
    REQUIRE(h.sent[1].to == server_endpoint(1));
    REQUIRE(h.sent[0].bytes.size() == kStunHeaderSize);  // 요청은 헤더뿐이다 (13장)

    // 트랜잭션 ID 가 응답을 가른다. 두 요청이 같은 ID 를 쓰면 가르지 못한다.
    REQUIRE(h.transaction_of(0) != h.transaction_of(1));
}

TEST_CASE("stun_client: the retry schedule is 500ms, 1s and 2s", "[stun_client]") {
    // protocol.md 11장 타이머. 값은 간격이라 첫 요청 뒤 500, 1500, 3500 에 다시 보낸다.
    Harness h(2);
    REQUIRE(h.client().start(0));

    h.advance_to(499);
    REQUIRE(h.count_to(server_endpoint(0)) == 1);

    h.advance_to(kStunDeadlineMs - 1);
    const auto times = h.times_to(server_endpoint(0));
    REQUIRE(times == std::vector<Millis>{0, 500, 1500, 3500});  // 첫 요청 + 재시도 3회

    // 마감 안에서 네 번째 재시도는 없다.
    REQUIRE(h.count_to(server_endpoint(1)) == 4);
}

TEST_CASE("stun_client: a response cancels that server's retry and deadline", "[stun_client]") {
    // protocol.md 11장 타이머의 "취소" 열. 응답 수신에 그 서버의 재시도와 마감을 지운다.
    Harness h(2);
    REQUIRE(h.client().start(0));

    h.advance_to(100);
    h.deliver(success_response(h.transaction_of(0), kMapped));

    REQUIRE_FALSE(h.timers.contains("stun.retry.0"));
    REQUIRE_FALSE(h.timers.contains("stun.deadline.0"));
    REQUIRE(h.timers.contains("stun.retry.1"));
    REQUIRE(h.timers.contains("stun.deadline.1"));

    h.advance_to(4999);
    REQUIRE(h.count_to(server_endpoint(0)) == 1);  // 그 서버로는 더 보내지 않았다
    REQUIRE(h.count_to(server_endpoint(1)) == 4);  // 다른 서버의 일정은 그대로다
}

TEST_CASE("stun_client: a deadline swaps in the next server", "[stun_client]") {
    // protocol.md 11장: 한 서버가 마감에 걸리면 목록의 다음 서버로 바꾸고 그 서버의
    // 재시도 일정과 마감이 새로 시작한다.
    Harness h(4);
    REQUIRE(h.client().start(0));

    h.advance_to(kStunDeadlineMs - 1);
    REQUIRE(h.count_to(server_endpoint(2)) == 0);

    h.advance_to(kStunDeadlineMs);
    REQUIRE(h.count_to(server_endpoint(2)) == 1);
    REQUIRE(h.count_to(server_endpoint(3)) == 1);
    REQUIRE(h.client().phase() == StunPhase::Running);

    // 일정이 새로 시작한다. 5000 을 0 으로 본 간격이다.
    h.advance_to(2 * kStunDeadlineMs - 1);
    const auto times = h.times_to(server_endpoint(2));
    REQUIRE(times == std::vector<Millis>{5000, 5500, 6500, 8500});
}

TEST_CASE("stun_client: two different servers make it succeed", "[stun_client]") {
    Harness h(4);
    REQUIRE(h.client().start(0));

    h.advance_to(10);
    h.deliver(success_response(h.transaction_of(0), kMapped));
    REQUIRE(h.client().phase() == StunPhase::Running);  // 하나로는 아직이다

    const auto second = h.transaction_of(1);
    h.deliver(success_response(second, Endpoint(0xC6336407u, 51001)));

    REQUIRE(h.client().phase() == StunPhase::Succeeded);
    REQUIRE(h.client().mappings().size() == 2);
    REQUIRE(h.client().mappings()[0].server == "stun0.example:3478");
    REQUIRE(h.client().mappings()[0].mapped == kMapped);
    REQUIRE(h.client().mappings()[1].mapped.port() == 51001);

    // 끝난 뒤에는 타이머도 송신도 남지 않는다.
    REQUIRE(h.timers.size() == 0);
    const std::size_t sent_before = h.sent.size();
    h.advance_to(20000);
    REQUIRE(h.sent.size() == sent_before);
}

TEST_CASE("stun_client: one answer alone is a failure", "[stun_client]") {
    // architecture.md 3.5 기동 입력. 응답이 하나뿐이어도 STUN_DISCOVERY_FAILED 다.
    Harness h(2);
    REQUIRE(h.client().start(0));

    h.advance_to(100);
    h.deliver(success_response(h.transaction_of(0), kMapped));

    h.advance_to(kStunDeadlineMs - 1);
    REQUIRE(h.client().phase() == StunPhase::Running);

    h.advance_to(kStunDeadlineMs);
    REQUIRE(h.client().phase() == StunPhase::Failed);
    REQUIRE(h.client().mappings().size() == 1);
}

TEST_CASE("stun_client: the stage cap is 5s times ceil(list / 2)", "[stun_client]") {
    // protocol.md 11장 타이머. 두 서버에 동시에 질의하므로 목록을 반으로 나눈 만큼이다.
    REQUIRE(stun_stage_cap_ms(0) == 0);
    REQUIRE(stun_stage_cap_ms(1) == 5000);
    REQUIRE(stun_stage_cap_ms(2) == 5000);
    REQUIRE(stun_stage_cap_ms(3) == 10000);
    REQUIRE(stun_stage_cap_ms(4) == 10000);  // 기본 목록 4개면 10초다
    REQUIRE(stun_stage_cap_ms(5) == 15000);

    // 아무도 응답하지 않는 목록 4개가 정확히 그 시각에 끝난다.
    Harness h(4);
    REQUIRE(h.client().start(0));
    REQUIRE(h.client().stage_cap_ms() == 10000);

    h.advance_to(9999);
    REQUIRE(h.client().phase() == StunPhase::Running);
    h.advance_to(10000);
    REQUIRE(h.client().phase() == StunPhase::Failed);
    REQUIRE(h.client().mappings().empty());
}

TEST_CASE("stun_client: an early answer keeps two queries in flight", "[stun_client]") {
    // 상한을 지키려면 응답으로 빈 자리도 다시 채워야 한다 (stun_client.hpp 의 표).
    // 마감에서만 채우면 남은 둘을 한 자리로 차례로 써서 끝이 15초가 된다.
    Harness h(4);
    REQUIRE(h.client().start(0));

    h.advance_to(1000);
    h.deliver(success_response(h.transaction_of(0), kMapped));

    // 빈 자리가 그 자리에서 다음 서버를 받는다.
    REQUIRE(h.count_to(server_endpoint(2)) == 1);
    REQUIRE(h.times_to(server_endpoint(2))[0] == 1000);

    h.advance_to(20000);
    REQUIRE(h.client().phase() == StunPhase::Failed);
    REQUIRE(h.sent.back().at <= 10000);  // 상한 밖에서 보낸 요청이 없다
}

TEST_CASE("stun_client: an error response gives up on that server", "[stun_client]") {
    // 거절한 서버에 같은 요청을 다시 보내지 않는다 (stun_client.hpp 의 표).
    Harness h(3);
    REQUIRE(h.client().start(0));

    h.advance_to(100);
    h.deliver(error_response(h.transaction_of(0), 4, 0));  // 400 Bad Request

    REQUIRE(h.client().mappings().empty());
    REQUIRE(h.count_to(server_endpoint(2)) == 1);  // 자리를 다음 서버가 받았다
    REQUIRE(h.times_to(server_endpoint(2))[0] == 100);

    h.advance_to(4999);
    REQUIRE(h.count_to(server_endpoint(0)) == 1);  // 그 서버로는 더 보내지 않는다
}

TEST_CASE("stun_client: a malformed response counts drop_stun_parse", "[stun_client]") {
    // protocol.md 13장. 검증에 걸린 응답은 drop_stun_parse 로 센다. 이유를 나누지 않는다.
    Harness h(2);
    REQUIRE(h.client().start(0));
    h.advance_to(100);

    // (a) magic cookie 가 틀린 것. 대기 중인 요청을 찾는 단계에서 걸린다.
    auto bad_cookie = success_response(h.transaction_of(0), kMapped);
    bad_cookie[4] = std::byte{0x00};
    h.deliver(bad_cookie);
    REQUIRE(h.counters.value(Counter::DropStunParse) == 1);

    // (b) 잘린 응답.
    auto truncated = success_response(h.transaction_of(0), kMapped);
    truncated.resize(24);
    h.deliver(truncated);
    REQUIRE(h.counters.value(Counter::DropStunParse) == 2);

    // (c) IPv4 가 아닌 family.
    h.deliver(ipv6_family_response(h.transaction_of(0)));
    REQUIRE(h.counters.value(Counter::DropStunParse) == 3);

    // 셋 다 그 서버를 끝내지 않는다. 재시도 일정이 그대로 돈다.
    REQUIRE(h.client().phase() == StunPhase::Running);
    h.advance_to(4999);
    REQUIRE(h.count_to(server_endpoint(0)) == 4);
}

TEST_CASE("stun_client: an unknown transaction id is dropped", "[stun_client]") {
    // roadmap.md Phase 2 검증: 트랜잭션 ID 가 다른 응답은 무시된다.
    Harness h(2);
    REQUIRE(h.client().start(0));
    h.advance_to(100);

    TransactionId other{};
    other[0] = std::byte{0xAB};
    h.deliver(success_response(other, kMapped));

    REQUIRE(h.counters.value(Counter::DropStunParse) == 1);
    REQUIRE(h.client().mappings().empty());
    REQUIRE(h.client().phase() == StunPhase::Running);
    REQUIRE(h.timers.contains("stun.deadline.0"));
}

TEST_CASE("stun_client: a duplicate answer is not counted as a drop", "[stun_client]") {
    // 재시도를 보냈으면 응답이 여러 통 오는 것이 정상이다 (stun_client.hpp 의 표).
    Harness h(4);
    REQUIRE(h.client().start(0));
    h.advance_to(600);  // 재시도가 한 번 나갔다

    const auto first = h.transaction_of(0);
    h.deliver(success_response(first, kMapped));
    h.deliver(success_response(first, kMapped));

    REQUIRE(h.counters.value(Counter::DropStunParse) == 0);
    REQUIRE(h.client().mappings().size() == 1);  // 두 번 세지 않는다
}

TEST_CASE("stun_client: a list shorter than two fails without sending", "[stun_client]") {
    // architecture.md 3.5 기동 입력. 해석에 실패해 목록이 줄어든 경우와 같은 규칙이다.
    Harness one(1);
    REQUIRE_FALSE(one.client().start(0));
    REQUIRE(one.client().phase() == StunPhase::Failed);
    REQUIRE(one.sent.empty());

    Harness none(0);
    REQUIRE_FALSE(none.client().start(0));
    REQUIRE(none.client().phase() == StunPhase::Failed);
    REQUIRE(none.sent.empty());
}

TEST_CASE("stun_client: a failed send keeps the schedule", "[stun_client]") {
    // 한 번의 sendto 실패는 그 서버의 실패가 아니다 (protocol.md 6장 소켓 소유권).
    Harness h(2);
    h.send_ok = false;
    REQUIRE(h.client().start(0));

    h.advance_to(1500);
    REQUIRE(h.count_to(server_endpoint(0)) == 3);  // 실패해도 재시도가 돈다

    h.send_ok = true;
    h.advance_to(3500);
    h.deliver(success_response(h.transaction_of(0), kMapped));
    REQUIRE(h.client().mappings().size() == 1);
}

TEST_CASE("stun_client: it only owns names of its own shape", "[stun_client]") {
    // 접두가 아니라 모양을 본다. `stun.` 으로 시작하는 것을 전부 받으면 나중에 생길 다른
    // stun.* 타이머를 이 객체가 가로챈다 (stun_client.hpp).
    REQUIRE(StunClient::owns_timer("stun.retry.0"));
    REQUIRE(StunClient::owns_timer("stun.deadline.3"));
    REQUIRE(StunClient::owns_timer("stun.retry.4294967295"));

    // 다른 STUN 타이머. 접두만 보면 여기서 걸린다.
    REQUIRE_FALSE(StunClient::owns_timer("stun.probe.0"));
    REQUIRE_FALSE(StunClient::owns_timer("stun.keepalive"));
    REQUIRE_FALSE(StunClient::owns_timer("stun."));

    // 모양이 어긋난 것.
    REQUIRE_FALSE(StunClient::owns_timer("stun.retry."));
    REQUIRE_FALSE(StunClient::owns_timer("stun.retry.x"));
    REQUIRE_FALSE(StunClient::owns_timer("stun.retry.1x"));
    REQUIRE_FALSE(StunClient::owns_timer("stun.retry.-1"));
    REQUIRE_FALSE(StunClient::owns_timer("stun.retry.07"));   // to_string 이 내지 않는 표기
    REQUIRE_FALSE(StunClient::owns_timer("stun.retry.0 "));
    REQUIRE_FALSE(StunClient::owns_timer("stun.retry.12345678901"));  // 자릿수 상한 밖
    REQUIRE_FALSE(StunClient::owns_timer("stun.deadline"));
    REQUIRE_FALSE(StunClient::owns_timer(" stun.retry.0"));

    // 남의 이름.
    REQUIRE_FALSE(StunClient::owns_timer("probe200"));
    REQUIRE_FALSE(StunClient::owns_timer("keepalive"));
    REQUIRE_FALSE(StunClient::owns_timer(""));
}

TEST_CASE("stun_client: an answer that arrives after the deadline is not a drop", "[stun_client]") {
    // 재시도를 세 번 보냈으면 마감 직후에 응답이 오는 것은 정상 경로다. 그것을 폐기로
    // 세면 카운터가 거짓말을 한다 (stun_client.hpp 의 "이미 끝난 트랜잭션의 응답").
    Harness h(4);
    REQUIRE(h.client().start(0));
    const TransactionId first = h.transaction_of(0);

    h.advance_to(kStunDeadlineMs);  // 첫 자리가 마감으로 끝나고 서버 2 로 바뀐다
    REQUIRE(h.count_to(server_endpoint(2)) == 1);
    REQUIRE(h.client().phase() == StunPhase::Running);

    h.deliver(success_response(first, kMapped));
    REQUIRE(h.counters.value(Counter::DropStunParse) == 0);
    REQUIRE(h.client().mappings().empty());  // 끝난 자리의 응답은 쓰지 않는다

    // 단계 종료로 끝난 자리도 같다. 두 응답으로 끝낸 뒤 세 번째가 와도 세지 않는다.
    Harness done(4);
    REQUIRE(done.client().start(0));
    const TransactionId third = done.transaction_of(0);
    done.deliver(success_response(done.transaction_of(0), kMapped));
    const TransactionId live = done.transaction_of(2);  // 성공으로 자리를 받은 서버 2
    done.deliver(success_response(done.transaction_of(1), kMapped));
    REQUIRE(done.client().phase() == StunPhase::Succeeded);
    done.deliver(success_response(live, kMapped));
    done.deliver(success_response(third, kMapped));
    REQUIRE(done.counters.value(Counter::DropStunParse) == 0);
}

TEST_CASE("stun_client: the transaction id picks the server", "[stun_client]") {
    // 난수를 주입했으므로 어느 요청의 ID 인지가 값으로 정해진다. 두 번째 요청의 ID 로
    // 답하면 두 번째 서버의 관측이어야 한다.
    Harness h(4);
    REQUIRE(h.client().start(0));
    REQUIRE(h.transaction_of(0) == Harness::nth_transaction(1));
    REQUIRE(h.transaction_of(1) == Harness::nth_transaction(2));

    h.deliver(success_response(Harness::nth_transaction(2), kMapped));
    REQUIRE(h.client().mappings().size() == 1);
    REQUIRE(h.client().mappings()[0].server == "stun1.example:3478");
}

TEST_CASE("stun_client: a random source failure ends the stage", "[stun_client]") {
    // stun_client.hpp: 트랜잭션 ID 를 뽑지 못하면 보낼 수 있는 요청이 없다. 예측 가능한
    // 값으로 대신하지 않는다.
    Harness first_call(4, 1);
    REQUIRE_FALSE(first_call.client().start(0));
    REQUIRE(first_call.client().phase() == StunPhase::Failed);
    REQUIRE(first_call.sent.empty());

    // 두 번째 자리에서 실패해도 단계 전체가 끝난다. 한 자리만 살려 두지 않는다.
    Harness second_call(4, 2);
    REQUIRE_FALSE(second_call.client().start(0));
    REQUIRE(second_call.client().phase() == StunPhase::Failed);
    REQUIRE(second_call.sent.size() == 1);
    REQUIRE(second_call.timers.size() == 0);  // 남은 타이머가 없다

    // 서버를 바꾸는 자리에서 실패해도 같다.
    Harness on_swap(4, 3);
    REQUIRE(on_swap.client().start(0));
    on_swap.advance_to(kStunDeadlineMs);
    REQUIRE(on_swap.client().phase() == StunPhase::Failed);
    REQUIRE(on_swap.timers.size() == 0);
}

TEST_CASE("stun_client: resolving the list drops failures and duplicate endpoints",
          "[stun_client]") {
    // architecture.md 3.5 기동 입력. 해석 -> 중복 제거 -> "두 개 미만" 판정 순서다.
    // **해석을 주입한다.** 실제 DNS 에 기대면 오프라인에서 깨지고, NXDOMAIN 을 가로채는
    // 망에서는 없는 이름이 주소를 받아 온다.
    const Endpoint shared(0xCB007101u, 19302);   // 203.0.113.1:19302
    const Endpoint other(0xCB007102u, 3478);     // 203.0.113.2:3478

    const auto resolver = [&](std::string_view host, std::uint16_t port)
        -> std::optional<Endpoint> {
        if (host == "dead.example") {
            return std::nullopt;  // 해석 실패
        }
        if (host == "one.example" || host == "two.example") {
            return Endpoint(shared.address(), port);  // 두 이름이 한 주소다
        }
        if (host == "other.example") {
            return Endpoint(other.address(), port);
        }
        return std::nullopt;
    };

    SECTION("two names on one address leave one entry, the first one") {
        const StunServerName list[] = {{"one.example", 19302}, {"two.example", 19302}};
        const auto out = resolve_stun_servers(list, resolver);
        REQUIRE(out.size() == 1);
        REQUIRE(out[0].label == "one.example:19302");  // 앞의 것을 남긴다
        REQUIRE(out[0].endpoint == shared);
    }

    SECTION("the same host on another port is another server") {
        const StunServerName list[] = {{"one.example", 19302}, {"one.example", 3478}};
        const auto out = resolve_stun_servers(list, resolver);
        REQUIRE(out.size() == 2);
        REQUIRE(out[0].endpoint.port() == 19302);
        REQUIRE(out[1].endpoint.port() == 3478);
    }

    SECTION("order survives and only the later duplicate goes") {
        const StunServerName list[] = {
            {"one.example", 19302}, {"other.example", 3478}, {"two.example", 19302}};
        const auto out = resolve_stun_servers(list, resolver);
        REQUIRE(out.size() == 2);
        REQUIRE(out[0].label == "one.example:19302");
        REQUIRE(out[1].label == "other.example:3478");
    }

    SECTION("a name that does not resolve is dropped") {
        const StunServerName list[] = {
            {"dead.example", 3478}, {"one.example", 19302}, {"other.example", 3478}};
        const auto out = resolve_stun_servers(list, resolver);
        REQUIRE(out.size() == 2);
        REQUIRE(out[0].label == "one.example:19302");
    }

    SECTION("an empty list stays empty") {
        REQUIRE(resolve_stun_servers({}, resolver).empty());
    }
}

TEST_CASE("stun_client: duplicates are removed before the two-entry check", "[stun_client]") {
    // 순서가 중요하다. 중복을 지우고 나서 "두 개 미만" 을 본다. 항목 수만 보면 둘이지만
    // 목적지는 하나여서 매핑 판정이 성립하지 않는다 (architecture.md 3.5).
    const auto resolver = [](std::string_view, std::uint16_t port) -> std::optional<Endpoint> {
        return Endpoint(0xCB007101u, port);  // 이름과 무관하게 한 주소다
    };
    const StunServerName list[] = {{"one.example", 19302}, {"two.example", 19302}};
    auto servers = resolve_stun_servers(list, resolver);
    REQUIRE(servers.size() == 1);

    TimerSet timers;
    Counters counters;
    std::size_t sends = 0;
    StunClient client(
        std::move(servers),
        [&sends](const Endpoint&, std::span<const std::byte>) {
            ++sends;
            return true;
        },
        timers, counters);

    REQUIRE_FALSE(client.start(0));
    REQUIRE(client.phase() == StunPhase::Failed);
    REQUIRE(sends == 0);  // 한 목적지뿐이라 묻지 않는다
}
