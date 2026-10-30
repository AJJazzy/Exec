// Unit tests: JSON, strings, ring buffer, cancellation, compiler front end.
#include "nexus_test.hpp"

#include "nexus/common/cancellation.hpp"
#include "nexus/common/json.hpp"
#include "nexus/common/ring_buffer.hpp"
#include "nexus/common/strings.hpp"
#include "nexus/compiler/compiler.hpp"
#include "nexus/compiler/source_map.hpp"
#include "nexus/diagnostics/logger.hpp"
#include "nexus/library/script_library.hpp"

#include <thread>

using namespace nexus;

TEST_CASE(json_roundtrip) {
    json::Value v = json::Object{
        {"string", "a\"b\\c\n\t\x01"}, {"int", 42}, {"neg", -7.5}, {"big", 1e300}, {"bool", true}, {"null", nullptr},
        {"array", json::Array{1, "two", json::Object{{"three", 3}}}}, {"unicode", "héllo ✓ 😀"},
    };
    std::string compact = json::dump(v);
    auto parsed = json::parse(compact);
    REQUIRE(parsed.ok());
    CHECK(parsed.value() == v);
    auto pretty = json::parse(json::dump(v, 2));
    REQUIRE(pretty.ok());
    CHECK(pretty.value() == v);
    CHECK_EQ(json::dump(json::Value(42)), std::string("42"));
    CHECK_EQ(json::dump(json::Value(0.5)), std::string("0.5"));
}

TEST_CASE(json_unicode_escapes_and_errors) {
    auto v = json::parse(R"({"a":"\u00e9\ud83d\ude00","b":[1,2,3]})");
    REQUIRE(v.ok());
    CHECK_EQ(v.value().getString("a"), std::string("é😀"));
    CHECK_EQ(v.value().get("b")->asArray().size(), size_t(3));
    for (const char* bad : {"", "{", "[1,]", "{\"a\" 1}", "tru", "\"\\x\"", "\"\\ud800\"", "1 2", "{\"a\":01x}", "\"\x01\""})
        CHECK(!json::parse(bad).ok());
    std::string deep(300, '[');
    deep += std::string(300, ']');
    CHECK(!json::parse(deep).ok()); // depth limit
}

TEST_CASE(strings_utf8_and_format) {
    CHECK(str::isValidUtf8("plain ascii"));
    CHECK(str::isValidUtf8("ünïcödé ✓ 😀"));
    CHECK(!str::isValidUtf8("\xC3\x28"));
    CHECK(!str::isValidUtf8("\xED\xA0\x80")); // surrogate
    CHECK(!str::isValidUtf8("\xC0\xAF"));     // overlong
    CHECK_EQ(str::sanitizeUtf8("a\xFF" "b"), std::string("a\xEF\xBF\xBD" "b"));
    CHECK_EQ(str::splitLines("a\r\nb\nc").size(), size_t(3));
    CHECK_EQ(str::formatBytes(2048), std::string("2.0 KB"));
    CHECK_EQ(str::randomHex(16).size(), size_t(32));
    CHECK(str::randomHex(8) != str::randomHex(8));
    CHECK(str::containsIgnoreCase("Hello World", "WORLD"));
}

TEST_CASE(ring_buffer_is_bounded) {
    RingBuffer<int> rb(3);
    for (int i = 1; i <= 10; ++i) rb.push(i);
    CHECK_EQ(rb.size(), size_t(3));
    CHECK_EQ(rb.droppedCount(), uint64_t(7));
    auto all = rb.since(0);
    REQUIRE(all.size() == 3);
    CHECK_EQ(all.front().second, 8);
    CHECK_EQ(rb.since(9).size(), size_t(1));
    CHECK_EQ(rb.since(10).size(), size_t(0));
    CHECK_EQ(rb.since(0, 2).front().second, 9);
}

TEST_CASE(cancellation_wakes_waiters) {
    CancellationSource src;
    bool callback = false;
    src.token().onCancel([&] { callback = true; });
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        src.cancel();
    });
    auto start = std::chrono::steady_clock::now();
    bool cancelled = src.token().waitUntil(start + std::chrono::seconds(5));
    t.join();
    CHECK(cancelled);
    CHECK(callback);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    bool late = false;
    src.token().onCancel([&] { late = true; }); // already cancelled: runs immediately
    CHECK(late);
}

TEST_CASE(logger_bounded_and_exportable) {
    Logger log(10);
    for (int i = 0; i < 25; ++i) log.info("test", "message " + std::to_string(i), {{"i", std::to_string(i)}});
    log.error("test", "bad \xFF byte");
    auto entries = log.entries();
    CHECK_EQ(entries.size(), size_t(10));
    CHECK(str::isValidUtf8(entries.back().message));
    CHECK_EQ(log.countsByLevel()[LogLevel::Info], uint64_t(25));
    std::string path = nexus_test::tempDir() + "/log.json";
    REQUIRE(log.exportJson(path).ok());
    auto parsed = json::parse(readFile(path).value());
    REQUIRE(parsed.ok());
    CHECK_EQ(parsed.value().get("entries")->asArray().size(), size_t(10));
}

TEST_CASE(compiler_compiles_valid_source) {
    auto out = compileSource("local x = 1\nreturn x + 1\n");
    CHECK(out.ok);
    CHECK(!out.bytecode.empty());
    CHECK(out.diagnostics.empty());
    CHECK(checkSyntax("print('hi')").empty());
}

TEST_CASE(compiler_reports_all_syntax_errors_with_lines) {
    auto out = compileSource("local x = \nprint('ok')\nlocal function f(\nend\n");
    CHECK(!out.ok);
    REQUIRE(!out.diagnostics.empty());
    for (const auto& d : out.diagnostics) {
        CHECK(d.range.line >= 1);
        CHECK_EQ(d.code, std::string("syntax"));
    }
    auto multi = checkSyntax("x = = 1\ny = = 2\n");
    CHECK(multi.size() >= 2);
    CHECK_EQ(multi.front().range.line, uint32_t(1));
}

TEST_CASE(compiler_reports_compile_errors) {
    // Valid syntax that the compiler rejects: more than 200 simultaneously live locals. The values
    // are non-constant so the optimiser cannot fold the locals away.
    std::string src = "local function f()\n";
    for (int i = 0; i < 300; ++i) src += "local v" + std::to_string(i) + " = math.random()\n";
    src += "end\n";
    auto out = compileSource(src);
    CHECK(!out.ok);
    REQUIRE(!out.diagnostics.empty());
    CHECK(out.diagnostics.front().message.find("register") != std::string::npos || out.diagnostics.front().message.find("local") != std::string::npos);
}

TEST_CASE(source_map_utf16_offsets) {
    std::string s = "a\né😀b\n";
    SourceMap m(s);
    CHECK_EQ(m.lineCount(), size_t(3));
    CHECK_EQ(m.offsetOf(1, 0), size_t(2));
    CHECK_EQ(m.utf16Offset(2), size_t(2));
    CHECK_EQ(m.utf16Offset(4), size_t(3));  // after é (2 bytes, 1 unit)
    CHECK_EQ(m.utf16Offset(8), size_t(5));  // after 😀 (4 bytes, 2 units)
    CHECK_EQ(m.lineOf(3), uint32_t(1));
}
