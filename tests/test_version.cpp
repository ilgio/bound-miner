// Milestone 13: one version, from CMakeLists.txt, everywhere a program says which it is.
#include "net/peerman.h"
#include "seeder/crawler.h"
#include "util/version.h"

#include <gtest/gtest.h>

#include <regex>
#include <string>

using namespace bound;

TEST(Version, IsMajorMinorPatch) {
    EXPECT_TRUE(std::regex_match(std::string(version()), std::regex(R"(\d+\.\d+\.\d+)"))) << version();
}

TEST(Version, CommitIsHexOrAbsent) {
    // Empty outside a git checkout; otherwise 12 hex digits, "-dirty" if files had changed.
    const std::string commit(build_commit());
    EXPECT_TRUE(commit.empty() || std::regex_match(commit, std::regex(R"([0-9a-f]{12}(-dirty)?)"))) << commit;
}

TEST(Version, VersionLineNamesProgramVersionAndCommit) {
    const std::string line = version_line("boundd");
    EXPECT_TRUE(line.starts_with("boundd " + std::string(version()))) << line;
    if (build_commit().empty()) {
        EXPECT_EQ(line, "boundd " + std::string(version()));
    } else {
        EXPECT_TRUE(line.ends_with("(commit " + std::string(build_commit()) + ")")) << line;
    }
    const std::string text = version_text("bound-cli");
    EXPECT_TRUE(text.starts_with(version_line("bound-cli") + "\n")) << text;
    EXPECT_NE(text.find("MIT license"), std::string::npos);
}

TEST(Version, UserAgentsCarryTheVersion) {
    // BIP 14's form, which explorers and seeders show.
    EXPECT_EQ(user_agent("boundd"), "/boundd:" + std::string(version()) + "/");
    EXPECT_EQ(PeerConfig{}.user_agent, user_agent("boundd"));
    EXPECT_EQ(CrawlerOptions{}.user_agent, user_agent("bound-seeder"));
}

TEST(Version, AsksForVersionOnlyAlone) {
    char program[] = "boundd";
    char flag[] = "--version";
    char short_flag[] = "-v";
    char other[] = "node";
    char* alone[] = {program, flag};
    char* short_alone[] = {program, short_flag};
    char* with_more[] = {program, flag, other};
    char* none[] = {program};
    char* not_it[] = {program, other};
    EXPECT_TRUE(asks_for_version(2, alone));
    EXPECT_TRUE(asks_for_version(2, short_alone));
    EXPECT_FALSE(asks_for_version(3, with_more));
    EXPECT_FALSE(asks_for_version(1, none));
    EXPECT_FALSE(asks_for_version(2, not_it));
}
