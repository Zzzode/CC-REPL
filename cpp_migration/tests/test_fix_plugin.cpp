// Unit tests for the H1/H2 plugin fixes:
//   H1 — plugin_marketplace: known_marketplaces.json config round-trip,
//        cache-only read, fetch-from-directory, add/remove.
//   H2 — plugin_validation: validate_file now performs real checks instead
//        of unconditionally returning success.
//
// NOTE: This file is intentionally NOT wired into tests/CMakeLists.txt — the
// migration rules forbid editing that file. The owning integration agent must
// add an `add_executable(test_fix_plugin test_fix_plugin.cpp)` + GoogleTest
// discovery block (see cmakeNeeds in the migration plan) before ctest can run
// it. Written so that, once linked against cc_utils, it exercises the ported
// logic without touching the network (directory/file sources only).

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <thread>

import cc.utils.json;
import cc.utils.plugin_marketplace;
import cc.utils.plugin_validation;

namespace mp = cc::utils::plugin_marketplace;
namespace pv = cc::utils::plugin_validation;

namespace {

// Scoped temp directory helper — keeps ~/.claude untouched by pointing the
// plugins dir at a unique temp path via CLAUDE_PLUGINS_DIR.
class TempPluginsEnv {
public:
    TempPluginsEnv() {
        auto tmpl = std::filesystem::temp_directory_path() / "cc_fix_plugin_XXXXXX";
        // mkdtemp-safe: create a uniquely named dir manually.
        dir_ = std::filesystem::temp_directory_path()
            / ("cc_fix_plugin_" + std::to_string(std::hash<std::thread::id>{}(
                  std::this_thread::get_id())) + "_"
              + std::to_string(counter_++));
        std::filesystem::create_directories(dir_);
        prev_ = std::getenv("CLAUDE_PLUGINS_DIR");
        set_env("CLAUDE_PLUGINS_DIR", dir_.string());
    }
    ~TempPluginsEnv() {
        if (prev_) set_env("CLAUDE_PLUGINS_DIR", prev_);
        else unset_env("CLAUDE_PLUGINS_DIR");
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    const std::filesystem::path& dir() const { return dir_; }

private:
    static void set_env(const char* k, const std::string& v) {
#ifdef _WIN32
        _putenv_s(k, v.c_str());
#else
        setenv(k, v.c_str(), 1);
#endif
    }
    static void unset_env(const char* k) {
#ifdef _WIN32
        _putenv_s(k, "");
#else
        unsetenv(k);
#endif
    }
    std::filesystem::path dir_;
    const char* prev_ = nullptr;
    static std::size_t counter_;
};

std::size_t TempPluginsEnv::counter_ = 0;

void write_file(const std::filesystem::path& p, const std::string& body) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << body;
}

} // namespace

// H2: validate_file on a valid plugin.json returns success=true with no errors.
TEST(PluginValidationFix, ValidPluginManifestPasses) {
    auto tmp = std::filesystem::temp_directory_path() / "cc_plugin_valid.json";
    write_file(tmp, R"({"name":"my-plugin","version":"1.0.0","description":"x","author":"me"})");
    auto r = pv::validate_file(tmp);
    EXPECT_TRUE(r.success) << "errors: " << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_TRUE(r.errors.empty());
    std::filesystem::remove(tmp);
}

// H2: a plugin.json missing the required 'name' field must now FAIL (previously
// the stub returned success=true unconditionally).
TEST(PluginValidationFix, MissingNameFails) {
    auto tmp = std::filesystem::temp_directory_path() / "cc_plugin_noname.json";
    write_file(tmp, R"({"version":"1.0.0"})");
    auto r = pv::validate_file(tmp);
    EXPECT_FALSE(r.success);
    ASSERT_FALSE(r.errors.empty());
    EXPECT_NE(r.errors[0].message.find("name"), std::string::npos);
    std::filesystem::remove(tmp);
}

// H2: non-existent file surfaces an honest ENOENT-style error, not fake success.
TEST(PluginValidationFix, MissingFileFails) {
    auto r = pv::validate_file("/nonexistent/path/plugin.json");
    EXPECT_FALSE(r.success);
    ASSERT_FALSE(r.errors.empty());
}

// H2: path traversal in a commands entry is flagged (security pre-check).
TEST(PluginValidationFix, PathTraversalFlagged) {
    auto tmp = std::filesystem::temp_directory_path() / "cc_plugin_traversal.json";
    write_file(tmp, R"({"name":"p","commands":["../escape.sh"]})");
    auto r = pv::validate_file(tmp);
    EXPECT_FALSE(r.success);
    bool found = false;
    for (const auto& e : r.errors) {
        if (e.message.find("..") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
    std::filesystem::remove(tmp);
}

// H2: a marketplace.json is detected via the "plugins" array and validated.
TEST(PluginValidationFix, MarketplaceWithDuplicateNamesFails) {
    auto tmp = std::filesystem::temp_directory_path() / "cc_marketplace.json";
    write_file(tmp,
        R"({"name":"mkt","plugins":[{"name":"a","source":"./a"},{"name":"a","source":"./b"}]})");
    auto r = pv::validate_file(tmp);
    EXPECT_FALSE(r.success);
    bool found_dup = false;
    for (const auto& e : r.errors) {
        if (e.message.find("Duplicate") != std::string::npos) found_dup = true;
    }
    EXPECT_TRUE(found_dup);
    std::filesystem::remove(tmp);
}

// H1: known_marketplaces.json round-trips through save + load.
TEST(PluginMarketplaceFix, ConfigRoundTrip) {
    TempPluginsEnv env;
    mp::KnownMarketplacesFile cfg;
    mp::KnownMarketplace km;
    km.source = mp::GitHubMarketplaceSource{"owner/repo", std::nullopt};
    km.cache_path = (env.dir() / "cache").string();
    km.last_updated = "2026-01-01T00:00:00Z";
    cfg["test-mkt"] = km;

    auto saved = mp::save_known_marketplaces_config(cfg);
    ASSERT_TRUE(saved.has_value()) << saved.error();
    auto loaded = mp::load_known_marketplaces_config();
    ASSERT_TRUE(loaded.has_value()) << loaded.error();
    ASSERT_EQ(loaded->size(), 1u);
    ASSERT_TRUE(loaded->contains("test-mkt"));
    EXPECT_EQ(std::get<mp::GitHubMarketplaceSource>(
                  (*loaded)["test-mkt"].source).repo, "owner/repo");
    EXPECT_EQ((*loaded)["test-mkt"].cache_path.value(), (env.dir() / "cache").string());
}

// H1: load on an absent config returns an empty map (fresh-install parity).
TEST(PluginMarketplaceFix, EmptyConfigWhenAbsent) {
    TempPluginsEnv env;
    auto loaded = mp::load_known_marketplaces_config();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->empty());
    EXPECT_TRUE(mp::load_known_marketplaces_config_safe().empty());
}

// H1: remove on a missing marketplace surfaces an honest error.
TEST(PluginMarketplaceFix, RemoveMissingErrors) {
    TempPluginsEnv env;
    auto r = mp::remove_marketplace("does-not-exist");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("not found"), std::string::npos);
}

// H1: fetch a directory-source marketplace by reading the nested manifest.
TEST(PluginMarketplaceFix, FetchDirectorySource) {
    TempPluginsEnv env;
    // Build a directory marketplace with .claude-plugin/marketplace.json.
    auto mkt_dir = env.dir() / "src_marketplace";
    write_file(mkt_dir / ".claude-plugin" / "marketplace.json",
        R"({"name":"dir-mkt","description":"d","plugins":[{"name":"p1","source":"./p1"}]})");

    mp::KnownMarketplacesFile cfg;
    mp::KnownMarketplace km;
    km.source = mp::DirectoryMarketplaceSource{mkt_dir.string()};
    cfg["dir-mkt"] = km;
    auto saved = mp::save_known_marketplaces_config(cfg);
    ASSERT_TRUE(saved.has_value()) << saved.error();

    auto fetched = mp::fetch_marketplace("dir-mkt");
    ASSERT_TRUE(fetched.has_value()) << fetched.error();
    EXPECT_EQ(fetched->name, "dir-mkt");
    ASSERT_EQ(fetched->plugins.size(), 1u);
    EXPECT_EQ(fetched->plugins.count("p1"), 1u);
}

// H1: get_marketplace_cache_only reads back a previously-fetched directory
// source without re-reading the network.
TEST(PluginMarketplaceFix, CacheOnlyRead) {
    TempPluginsEnv env;
    auto mkt_dir = env.dir() / "src_marketplace2";
    write_file(mkt_dir / ".claude-plugin" / "marketplace.json",
        R"({"name":"co-mkt","plugins":[{"name":"p2","source":"./p2"}]})");
    mp::KnownMarketplacesFile cfg;
    mp::KnownMarketplace km;
    km.source = mp::DirectoryMarketplaceSource{mkt_dir.string()};
    km.cache_path = mkt_dir.string();
    cfg["co-mkt"] = km;
    ASSERT_TRUE(mp::save_known_marketplaces_config(cfg).has_value());

    auto cached = mp::get_marketplace_cache_only("co-mkt");
    ASSERT_TRUE(cached.has_value());
    EXPECT_EQ(cached->name, "co-mkt");
}
