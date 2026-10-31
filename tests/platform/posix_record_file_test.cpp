#include "sangtachi/platform/record_file.hpp"
#include "sangtachi/platform/stdout.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// POSIX 기록 파일과 표준 출력 (ADR 0035, architecture.md 9장, 3.5).
// 시험 케이스 이름은 ASCII 로만 적는다.

using sangtachi::platform::OutputHandle;
using sangtachi::platform::RecordFile;

namespace {

std::filesystem::path exe_dir() {
    return std::filesystem::read_symlink("/proc/self/exe").parent_path();
}

std::string unique_name(std::string_view tag) {
    std::ostringstream out;
    out << "posix-test-" << tag << "-" << ::getpid() << "-"
        << sangtachi::platform::wall_clock_unix_ms() << ".log";
    return out.str();
}

std::string read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

// stdout.hpp 의 핸들은 fd + 1 이다 (ADR 0035 결정 3).
OutputHandle handle_of(int fd) {
    return reinterpret_cast<OutputHandle>(static_cast<std::uintptr_t>(fd) + 1);
}

}  // namespace

TEST_CASE("posix record: a log file is created next to the executable once", "[platform][record][posix]") {
    const std::string name = unique_name("log");
    auto opened = sangtachi::platform::open_log_file(name);
    INFO("op " << opened.failed_op << " code " << opened.error);
    REQUIRE(opened.ok());
    REQUIRE(opened.file.write("first\n"));
    REQUIRE(opened.file.write("second\n"));

    const std::filesystem::path path = exe_dir() / "logs" / name;
    REQUIRE(read_all(path) == "first\nsecond\n");

    // 같은 이름은 다시 만들지 않는다 (9장).
    const auto again = sangtachi::platform::open_log_file(name);
    REQUIRE_FALSE(again.ok());
    REQUIRE(again.failed_op == "open");
    REQUIRE(again.error == static_cast<std::uint32_t>(EEXIST));

    opened.file = RecordFile();
    std::filesystem::remove(path);
}

TEST_CASE("posix record: write_whole fails on a null handle", "[platform][record][posix]") {
    REQUIRE_FALSE(sangtachi::platform::write_whole(nullptr, "x"));
    RecordFile closed;
    REQUIRE_FALSE(closed.write("x"));
}

TEST_CASE("posix record: the wall clock is unix milliseconds", "[platform][record][posix]") {
    const std::uint64_t ms = sangtachi::platform::wall_clock_unix_ms();
    const auto now = static_cast<std::uint64_t>(::time(nullptr));
    REQUIRE(ms / 1000 + 2 >= now);
    REQUIRE(ms / 1000 <= now + 2);
    REQUIRE(sangtachi::platform::current_process_id() == static_cast<std::uint32_t>(::getpid()));
}

TEST_CASE("posix stdout: a pipe gets the bytes and one LF", "[platform][stdout][posix]") {
    std::array<int, 2> fds{};
    REQUIRE(::pipe(fds.data()) == 0);
    const OutputHandle out = handle_of(fds[1]);
    REQUIRE_FALSE(sangtachi::platform::is_console(out));
    REQUIRE(sangtachi::platform::write_line(out, "\xED\x95\x9C ROOM"));
    REQUIRE(sangtachi::platform::write_text(out, "> "));
    ::close(fds[1]);

    std::string got;
    std::array<char, 64> buffer{};
    ssize_t n = 0;
    while ((n = ::read(fds[0], buffer.data(), buffer.size())) > 0) {
        got.append(buffer.data(), static_cast<std::size_t>(n));
    }
    ::close(fds[0]);
    REQUIRE(got == "\xED\x95\x9C ROOM\n> ");
}

TEST_CASE("posix stdout: null and closed handles fail", "[platform][stdout][posix]") {
    REQUIRE_FALSE(sangtachi::platform::write_line(nullptr, "x"));
    REQUIRE_FALSE(sangtachi::platform::is_console(nullptr));
    REQUIRE(sangtachi::platform::stdout_handle() != nullptr);
}
