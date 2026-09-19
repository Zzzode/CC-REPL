#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

import cc.constants.paths;

namespace fs = std::filesystem;

namespace {

/// Create a temporary HOME with the given config-directory names present, and
/// point $HOME at it for the duration of the test. Restores both $HOME and
/// $LOOM_CONFIG_DIR on destruction, including on early return.
class TempHome {
public:
    TempHome() {
        root_ = fs::temp_directory_path() /
                ("loom-paths-" + std::to_string(::getpid()) + "-" +
                 std::to_string(counter_++));
        fs::create_directories(root_);
        if (const char* prev = std::getenv("HOME")) prev_home_ = prev;
        if (const char* prev = std::getenv("LOOM_CONFIG_DIR")) prev_config_ = prev;
        ::unsetenv("LOOM_CONFIG_DIR");
        ::setenv("HOME", root_.c_str(), 1);
    }

    ~TempHome() {
        if (prev_home_) ::setenv("HOME", prev_home_->c_str(), 1);
        else ::unsetenv("HOME");
        if (prev_config_) ::setenv("LOOM_CONFIG_DIR", prev_config_->c_str(), 1);
        else ::unsetenv("LOOM_CONFIG_DIR");
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    TempHome(const TempHome&) = delete;
    TempHome& operator=(const TempHome&) = delete;

    /// Create the directory (and a memory file in it, when given).
    void make_config_dir(const std::string& name,
                         const std::string& memory_file = {}) {
        auto dir = root_ / name;
        fs::create_directories(dir);
        if (!memory_file.empty()) {
            std::ofstream(dir / memory_file) << "content for " << name << "\n";
        }
    }

    [[nodiscard]] fs::path at(const std::string& name) const {
        return root_ / name;
    }

private:
    fs::path root_;
    std::optional<std::string> prev_home_;
    std::optional<std::string> prev_config_;
    static inline int counter_ = 0;
};

}  // namespace

// ---------------------------------------------------------------------------
// Config-directory cascade: .loom > .agents > .claude
// ---------------------------------------------------------------------------

TEST(PathsConfigHome, PrefersDotLoomWhenPresent) {
    TempHome home;
    home.make_config_dir(".loom");
    home.make_config_dir(".agents");
    home.make_config_dir(".claude");
    EXPECT_EQ(cc::constants::paths::config_home_read(), home.at(".loom"));
}

TEST(PathsConfigHome, FallsToDotAgentsWhenDotLoomAbsent) {
    TempHome home;
    home.make_config_dir(".agents");
    home.make_config_dir(".claude");
    EXPECT_EQ(cc::constants::paths::config_home_read(), home.at(".agents"));
}

TEST(PathsConfigHome, FallsToDotClaudeAsTheOldestRung) {
    TempHome home;
    home.make_config_dir(".claude");
    EXPECT_EQ(cc::constants::paths::config_home_read(), home.at(".claude"));
}

// When nothing exists we name the preferred directory rather than inheriting
// someone else's. Picking an existing-but-unrelated dir here would make the
// product obey a config it did not create.
TEST(PathsConfigHome, NamesThePreferredDirWhenNothingExists) {
    TempHome home;
    EXPECT_EQ(cc::constants::paths::config_home_read(), home.at(".loom"));
}

TEST(PathsConfigHome, ExplicitEnvOverrideWinsOverEveryCandidate) {
    TempHome home;
    home.make_config_dir(".loom");
    home.make_config_dir(".claude");
    auto elsewhere = home.at("somewhere-else");
    ::setenv("LOOM_CONFIG_DIR", elsewhere.c_str(), 1);
    EXPECT_EQ(cc::constants::paths::config_home_read(), elsewhere);
}

// existing_config_homes() returns ALL present dirs, unlike config_home()
// which picks one -- skill discovery reads from every location.
TEST(PathsConfigHome, ExistingHomesReturnsEveryCandidateInPriorityOrder) {
    TempHome home;
    home.make_config_dir(".claude");
    home.make_config_dir(".loom");
    const auto found = cc::constants::paths::existing_config_homes();
    ASSERT_EQ(found.size(), 2u);
    EXPECT_EQ(found[0], home.at(".loom"));
    EXPECT_EQ(found[1], home.at(".claude"));
}

TEST(PathsConfigHome, ExistingHomesRespectsTheExplicitOverrideAlone) {
    TempHome home;
    home.make_config_dir(".loom");
    auto elsewhere = home.at("pinned");
    fs::create_directories(elsewhere);
    ::setenv("LOOM_CONFIG_DIR", elsewhere.c_str(), 1);
    const auto found = cc::constants::paths::existing_config_homes();
    ASSERT_EQ(found.size(), 1u) << "an explicit override means exactly one home";
    EXPECT_EQ(found[0], elsewhere);
}

// ---------------------------------------------------------------------------
// Memory-file cascade: LOOM.md > AGENTS.md > CLAUDE.md, PER DIRECTORY
// ---------------------------------------------------------------------------

TEST(PathsMemoryFile, MemoryFileInPrefersLoomMd) {
    TempHome home;
    home.make_config_dir(".loom");
    auto dir = home.at(".loom");
    std::ofstream(dir / "AGENTS.md") << "agents";
    std::ofstream(dir / "CLAUDE.md") << "claude";
    std::ofstream(dir / "LOOM.md") << "loom";
    auto found = cc::constants::paths::memory_file_in(dir);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->filename(), "LOOM.md")
        << "all three exist, so the highest-priority name wins";
}

TEST(PathsMemoryFile, MemoryFileInFallsThroughToAgentsThenClaude) {
    TempHome home;
    auto dir = home.at("proj");
    fs::create_directories(dir);

    std::ofstream(dir / "CLAUDE.md") << "claude";
    auto found = cc::constants::paths::memory_file_in(dir);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->filename(), "CLAUDE.md");

    std::ofstream(dir / "AGENTS.md") << "agents";
    found = cc::constants::paths::memory_file_in(dir);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->filename(), "AGENTS.md")
        << "AGENTS.md outranks CLAUDE.md once it appears";

    std::ofstream(dir / "LOOM.md") << "loom";
    found = cc::constants::paths::memory_file_in(dir);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->filename(), "LOOM.md");
}

TEST(PathsMemoryFile, MemoryFileInReturnsNulloptForADirectoryWithNone) {
    TempHome home;
    auto dir = home.at("empty");
    fs::create_directories(dir);
    EXPECT_FALSE(cc::constants::paths::memory_file_in(dir).has_value());
}

// The load-bearing semantic: the cascade is applied PER DIRECTORY while
// walking up, so proximity beats name priority. A near CLAUDE.md must win
// over a far LOOM.md -- otherwise a file five levels up would override the
// one sitting next to the code being edited.
TEST(PathsMemoryFile, NearestDirectoryWinsEvenOnAWeakerFileName) {
    TempHome home;
    auto root = home.at("repo");
    auto nested = root / "src" / "deep";
    fs::create_directories(nested);

    std::ofstream(root / "LOOM.md") << "far but preferred";
    std::ofstream(root / "src" / "deep" / "CLAUDE.md") << "near but last-resort";

    auto found = cc::constants::paths::find_memory_file(nested);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, nested / "CLAUDE.md")
        << "proximity must beat name priority";
}

TEST(PathsMemoryFile, WalksUpUntilItFindsAnyMemoryFile) {
    TempHome home;
    auto root = home.at("repo");
    auto nested = root / "a" / "b";
    fs::create_directories(nested);
    std::ofstream(root / "AGENTS.md") << "at the root";

    auto found = cc::constants::paths::find_memory_file(nested);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, root / "AGENTS.md");
}

TEST(PathsMemoryFile, FindReturnsNulloptWhenTheWholeTreeHasNone) {
    TempHome home;
    auto nested = home.at("bare") / "x";
    fs::create_directories(nested);
    EXPECT_FALSE(cc::constants::paths::find_memory_file(nested).has_value());
}

// project_memory_path reports where memory WOULD go, so it must name the
// preferred file when the directory is empty -- quietly naming CLAUDE.md
// would have /init write the old brand name into new projects.
TEST(PathsMemoryFile, ProjectPathNamesThePreferredFileWhenNoneExists) {
    TempHome home;
    auto proj = home.at("fresh");
    fs::create_directories(proj);
    EXPECT_EQ(cc::constants::paths::project_memory_path(proj),
              proj / "LOOM.md");
}

TEST(PathsMemoryFile, ProjectPathReportsTheExistingFileWhenOneDoes) {
    TempHome home;
    auto proj = home.at("legacy");
    fs::create_directories(proj);
    std::ofstream(proj / "CLAUDE.md") << "existing";
    EXPECT_EQ(cc::constants::paths::project_memory_path(proj),
              proj / "CLAUDE.md");
}

TEST(PathsMemoryFile, UserMemoryPathFindsALegacyMemoryInDotClaude) {
    TempHome home;
    home.make_config_dir(".claude", "CLAUDE.md");
    EXPECT_EQ(cc::constants::paths::user_memory_path(),
              home.at(".claude") / "CLAUDE.md")
        << "a user's pre-rename memory must still be found";
}

TEST(PathsMemoryFile, UserMemoryPathFindsTheNewMemoryInDotLoom) {
    TempHome home;
    home.make_config_dir(".loom", "LOOM.md");
    home.make_config_dir(".claude", "CLAUDE.md");
    EXPECT_EQ(cc::constants::paths::user_memory_path(),
              home.at(".loom") / "LOOM.md")
        << ".loom is the higher rung, so its memory wins";
}

TEST(PathsMemoryFile, UserMemoryPathNamesThePreferredFileWhenNoneExists) {
    TempHome home;
    home.make_config_dir(".loom");
    EXPECT_EQ(cc::constants::paths::user_memory_path(),
              home.at(".loom") / "LOOM.md");
}

TEST(PathsMemoryFile, RecognisesOnlyTheThreeCascadeNames) {
    using cc::constants::paths::is_memory_file_name;
    EXPECT_TRUE(is_memory_file_name("LOOM.md"));
    EXPECT_TRUE(is_memory_file_name("AGENTS.md"));
    EXPECT_TRUE(is_memory_file_name("CLAUDE.md"));
    EXPECT_FALSE(is_memory_file_name("README.md"));
    EXPECT_FALSE(is_memory_file_name("loom.md")) << "case-sensitive like the FS";
}

// Guards the ordering intent itself: if someone reorders the candidate
// arrays, this fails rather than silently changing which file is read.
TEST(PathsCascadeOrder, ConfidenceTheDocumentedOrderIsTheImplementedOne) {
    using namespace cc::constants::paths;
    ASSERT_EQ(kConfigDirCandidates.size(), 3u);
    EXPECT_EQ(kConfigDirCandidates[0], ".loom");
    EXPECT_EQ(kConfigDirCandidates[1], ".agents");
    EXPECT_EQ(kConfigDirCandidates[2], ".claude");

    ASSERT_EQ(kMemoryFileCandidates.size(), 3u);
    EXPECT_EQ(kMemoryFileCandidates[0], "LOOM.md");
    EXPECT_EQ(kMemoryFileCandidates[1], "AGENTS.md");
    EXPECT_EQ(kMemoryFileCandidates[2], "CLAUDE.md");
}

// ---------------------------------------------------------------------------
// Read vs write: the cascade is for READING. Writing stays under our own name.
// ---------------------------------------------------------------------------

// The dangerous mistake this guards against: if the write path followed the
// read cascade, a user who has only ~/.claude would get our sessions/,
// plugins/ and state written into another tool's directory. Reading their
// config is intended; writing into their home is not.
TEST(PathsReadWriteSplit, WriteStaysInDotLoomEvenWhenDotClaudeExists) {
    TempHome home;
    home.make_config_dir(".claude");
    EXPECT_EQ(cc::constants::paths::config_home_read(), home.at(".claude"))
        << "read follows the cascade to the legacy dir";
    EXPECT_EQ(cc::constants::paths::config_home_write(), home.at(".loom"))
        << "write must NOT follow it into another tool's directory";
}

TEST(PathsReadWriteSplit, WriteIgnoresDotAgentsToo) {
    TempHome home;
    home.make_config_dir(".agents");
    EXPECT_EQ(cc::constants::paths::config_home_read(), home.at(".agents"));
    EXPECT_EQ(cc::constants::paths::config_home_write(), home.at(".loom"));
}

TEST(PathsReadWriteSplit, WriteUsesDotLoomWhenItExists) {
    TempHome home;
    home.make_config_dir(".loom");
    EXPECT_EQ(cc::constants::paths::config_home_write(), home.at(".loom"));
}

TEST(PathsReadWriteSplit, ExplicitOverrideDirectsBothReadAndWrite) {
    TempHome home;
    home.make_config_dir(".claude");
    auto pinned = home.at("pinned");
    ::setenv("LOOM_CONFIG_DIR", pinned.c_str(), 1);
    EXPECT_EQ(cc::constants::paths::config_home_read(), pinned);
    EXPECT_EQ(cc::constants::paths::config_home_write(), pinned)
        << "an explicit override is a deliberate choice, so it wins for both";
}

// user_memory_path reads across the cascade (finding a legacy memory) but
// must fall back to the WRITE dir when nothing exists, so we do not seed a
// new memory file inside ~/.claude.
TEST(PathsReadWriteSplit, UserMemoryPathPrefersLegacyButSeedsOurOwnDir) {
    TempHome home;
    home.make_config_dir(".claude", "CLAUDE.md");
    EXPECT_EQ(cc::constants::paths::user_memory_path(),
              home.at(".claude") / "CLAUDE.md")
        << "an existing legacy memory is read";

    TempHome empty;
    empty.make_config_dir(".claude");  // exists, but holds no memory file
    EXPECT_EQ(cc::constants::paths::user_memory_path(),
              empty.at(".loom") / "LOOM.md")
        << "with no memory anywhere, name the file under our own dir";
}
