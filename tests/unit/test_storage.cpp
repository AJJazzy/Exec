// Unit tests: script library, drafts and settings (real files in a temporary directory).
#include "nexus_test.hpp"

#include "nexus/library/draft_store.hpp"
#include "nexus/library/script_library.hpp"
#include "nexus/settings/settings.hpp"

#include <algorithm>
#include <filesystem>

using namespace nexus;
namespace fs = std::filesystem;

namespace {
std::string freshDir(const std::string& name) {
    std::string d = nexus_test::tempDir() + "/" + name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}
} // namespace

TEST_CASE(library_create_read_write_rename_duplicate) {
    ScriptLibrary lib(freshDir("lib1"));
    REQUIRE(lib.initialize().ok());
    auto folder = lib.createFolder("", "Tools", true);
    REQUIRE(folder.ok());
    auto script = lib.createScript(folder.value(), "hello", "print('hi')\n");
    REQUIRE(script.ok());
    CHECK_EQ(script.value(), std::string("Tools/hello.luau"));
    CHECK_EQ(lib.read(script.value()).value(), std::string("print('hi')\n"));
    REQUIRE(lib.write(script.value(), "print('changed')").ok());
    CHECK_EQ(lib.read(script.value()).value(), std::string("print('changed')"));
    auto second = lib.createScript(folder.value(), "hello", "");
    REQUIRE(second.ok());
    CHECK_EQ(second.value(), std::string("Tools/hello 2.luau")); // unique naming
    auto renamed = lib.rename(script.value(), "greeting");
    REQUIRE(renamed.ok());
    CHECK_EQ(renamed.value(), std::string("Tools/greeting.luau"));
    CHECK(!lib.rename(renamed.value(), "hello 2.luau").ok()); // exists
    auto dup = lib.duplicate(renamed.value());
    REQUIRE(dup.ok());
    CHECK_EQ(lib.read(dup.value()).value(), std::string("print('changed')"));
    auto tree = lib.tree();
    REQUIRE(tree.children.size() == 1);
    CHECK(tree.children[0].isProject);
    CHECK_EQ(tree.children[0].children.size(), size_t(3));
}

TEST_CASE(library_favorites_recent_and_metadata_follow_renames) {
    std::string dir = freshDir("lib2");
    {
        ScriptLibrary lib(dir);
        REQUIRE(lib.initialize().ok());
        auto s = lib.createScript("", "a", "x").value();
        REQUIRE(lib.setFavorite(s, true).ok());
        lib.markOpened(s);
        auto r = lib.rename(s, "b");
        REQUIRE(r.ok());
        CHECK_EQ(lib.favorites(), std::vector<std::string>{"b.luau"});
        CHECK_EQ(lib.recent().front(), std::string("b.luau"));
    }
    ScriptLibrary reopened(dir); // metadata persisted
    REQUIRE(reopened.initialize().ok());
    CHECK_EQ(reopened.favorites(), std::vector<std::string>{"b.luau"});
    CHECK(reopened.tree().children.front().favorite);
}

TEST_CASE(library_delete_moves_to_trash_and_move_works) {
    ScriptLibrary lib(freshDir("lib3"));
    REQUIRE(lib.initialize().ok());
    auto f = lib.createFolder("", "Dest").value();
    auto s = lib.createScript("", "temp", "data").value();
    auto moved = lib.move(s, f);
    REQUIRE(moved.ok());
    CHECK_EQ(moved.value(), std::string("Dest/temp.luau"));
    CHECK(!lib.move(f, f).ok()); // into itself
    auto trashed = lib.remove(moved.value());
    REQUIRE(trashed.ok());
    CHECK(fs::exists(trashed.value()));
    CHECK(!lib.read(moved.value()).ok());
    CHECK(!lib.remove("").ok()); // root
}

TEST_CASE(library_rejects_path_traversal_and_bad_names) {
    ScriptLibrary lib(freshDir("lib4"));
    REQUIRE(lib.initialize().ok());
    CHECK(!lib.read("../outside.luau").ok());
    CHECK(!lib.read("/etc/passwd").ok());
    CHECK(!lib.read(".nexus-library.json").ok());
    CHECK(!lib.write("a/../../x.luau", "x").ok());
    CHECK(!lib.createScript("", "bad/name", "").ok());
    CHECK(!lib.createScript("", ".hidden", "").ok());
    CHECK(!lib.createFolder("", "..").ok());
    CHECK(!lib.write("notes.exe", "x").ok());
    // A symlink pointing outside the library is refused.
    fs::create_directory_symlink("/tmp", lib.root() / "escape");
    CHECK(!lib.read("escape/anything.luau").ok());
}

TEST_CASE(library_search_names_and_contents) {
    ScriptLibrary lib(freshDir("lib5"));
    REQUIRE(lib.initialize().ok());
    lib.createScript("", "AutoFarm", "-- nothing\nlocal target = 1\n").value();
    lib.createScript("", "Other", "print('needle here')\n").value();
    auto byName = lib.search("autofarm");
    REQUIRE(byName.size() == 1);
    CHECK(byName[0].nameMatch);
    auto byContent = lib.search("NEEDLE");
    REQUIRE(byContent.size() == 1);
    CHECK_EQ(byContent[0].line, uint32_t(1));
    CHECK(lib.search("   ").empty());
}

TEST_CASE(library_import_preserves_bytes_and_never_executes) {
    ScriptLibrary lib(freshDir("lib6"));
    REQUIRE(lib.initialize().ok());
    std::string external = nexus_test::tempDir() + "/external.lua";
    std::string bytes = "print('crlf')\r\n-- invalid utf8: \xFF\xFE\r\nwhile true do end\r\n";
    REQUIRE(writeFileAtomic(external, bytes).ok());
    auto imported = lib.importFile(external, "");
    REQUIRE(imported.ok());
    CHECK(lib.read(imported.value()).value() == bytes); // byte-exact, nothing ran
    std::string exported = nexus_test::tempDir() + "/exported.lua";
    REQUIRE(lib.exportItem(imported.value(), exported).ok());
    CHECK(readFile(exported).value() == bytes);
    std::string exe = nexus_test::tempDir() + "/tool.sh";
    REQUIRE(writeFileAtomic(exe, "#!/bin/sh").ok());
    CHECK(!lib.importFile(exe, "").ok()); // wrong type
    std::string big = nexus_test::tempDir() + "/big.luau";
    REQUIRE(writeFileAtomic(big, std::string(ScriptLibrary::kMaxScriptBytes + 1, 'x')).ok());
    CHECK(!lib.importFile(big, "").ok()); // too large
    CHECK(!lib.importFile(nexus_test::tempDir() + "/missing.luau", "").ok());
}

TEST_CASE(drafts_roundtrip_and_skip_corrupt_files) {
    std::string dir = freshDir("drafts");
    DraftStore store(dir);
    Draft d;
    d.id = "tab-1";
    d.title = "Untitled";
    d.content = "print('unsaved')\n";
    d.libraryPath = "Tools/x.luau";
    REQUIRE(store.save(d).ok());
    d.id = "tab-2";
    REQUIRE(store.save(d).ok());
    REQUIRE(writeFileAtomic(dir + "/broken.json", "{not json").ok());
    auto list = store.list();
    CHECK_EQ(list.size(), size_t(2));
    CHECK_EQ(list.front().content, std::string("print('unsaved')\n"));
    CHECK(!store.save(Draft{"../escape", "", "", "", {}}).ok());
    REQUIRE(store.remove("tab-1").ok());
    CHECK_EQ(store.list().size(), size_t(1));
}

TEST_CASE(settings_roundtrip_and_validation) {
    std::string dir = freshDir("settings");
    SettingsStore store(dir + "/settings.json");
    Settings defaults = store.load(); // missing file -> defaults
    CHECK_EQ(defaults.defaultTarget, std::string("luau-isolated"));
    CHECK(!defaults.allowRobloxVersionCheck); // network is opt-in
    CHECK(!defaults.studioIntegrationEnabled);
    Settings s;
    s.appearance = "dark";
    s.timeoutMs = 5000;
    s.accentColor = "#00AAFF";
    REQUIRE(store.save(s).ok());
    Settings loaded = store.load();
    CHECK_EQ(loaded.appearance, std::string("dark"));
    CHECK_EQ(loaded.timeoutMs, int64_t(5000));
    CHECK_EQ(loaded.limits().timeout.count(), int64_t(5000));

    auto hostile = json::parse(R"({"appearance":"neon","timeoutMs":-5,"memoryLimitMB":999999,"accentColor":"red","defaultTarget":"roblox-player","indentWidth":100})").value();
    Settings clamped = Settings::fromJson(hostile);
    CHECK_EQ(clamped.appearance, std::string("system"));
    CHECK_EQ(clamped.timeoutMs, int64_t(100));
    CHECK_EQ(clamped.memoryLimitMB, uint64_t(8192));
    CHECK_EQ(clamped.accentColor, std::string("#7C5CFF"));
    CHECK_EQ(clamped.defaultTarget, std::string("luau-isolated")); // the Player is never a valid default
    CHECK_EQ(clamped.indentWidth, 8);
    REQUIRE(writeFileAtomic(dir + "/settings.json", "garbage").ok());
    CHECK_EQ(store.load().appearance, std::string("system"));
}
