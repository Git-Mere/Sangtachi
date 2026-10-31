#include "sangtachi/hash.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

// FIPS 180-4 SHA-256. 서드파티를 들이지 않으려고 직접 쓴다 (ADR 0035 결정 8).

namespace sangtachi {
namespace {

constexpr std::array<std::uint32_t, 64> kRound = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

constexpr std::array<std::uint32_t, 8> kInitial = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};

constexpr std::uint32_t rotr(std::uint32_t x, unsigned n) noexcept {
    return (x >> n) | (x << (32u - n));
}

void compress(std::array<std::uint32_t, 8>& state, const unsigned char* block) noexcept {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t t = 0; t < 16; ++t) {
        w[t] = (std::uint32_t{block[4 * t]} << 24) | (std::uint32_t{block[4 * t + 1]} << 16) |
               (std::uint32_t{block[4 * t + 2]} << 8) | std::uint32_t{block[4 * t + 3]};
    }
    for (std::size_t t = 16; t < 64; ++t) {
        const std::uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
        const std::uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }

    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (std::size_t t = 0; t < 64; ++t) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + ch + kRound[t] + w[t];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

}  // namespace

std::optional<std::string> sha256_hex(std::span<const std::byte> data) {
    std::array<std::uint32_t, 8> state = kInitial;
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());

    const std::size_t full = data.size() - data.size() % 64;
    for (std::size_t offset = 0; offset < full; offset += 64) {
        compress(state, bytes + offset);
    }

    // 남은 바이트 + 0x80 + 0 채움 + 비트 길이 64비트 빅엔디언. 길이 자리가 모자라면 블록이 둘이다.
    std::array<unsigned char, 128> tail{};
    const std::size_t rest = data.size() - full;
    for (std::size_t i = 0; i < rest; ++i) {
        tail[i] = bytes[full + i];
    }
    tail[rest] = 0x80;
    const std::size_t tail_size = rest < 56 ? 64 : 128;
    const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8u;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[tail_size - 1 - i] = static_cast<unsigned char>(bit_length >> (8 * i));
    }
    for (std::size_t offset = 0; offset < tail_size; offset += 64) {
        compress(state, tail.data() + offset);
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const std::uint32_t word : state) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            out.push_back(kHex[(word >> shift) & 0x0Fu]);
        }
    }
    return out;
}

}  // namespace sangtachi
