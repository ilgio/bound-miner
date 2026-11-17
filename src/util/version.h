// Which BOUND this is (milestone 13). The version comes from one place, project(bound VERSION ...)
// in CMakeLists.txt; the commit is recorded at every build (cmake/build_info.cmake).
#pragma once

#include <string>
#include <string_view>

namespace bound {

inline constexpr std::string_view kProjectUrl = "https://github.com/Sonic-Bound/Bound";

// "0.10.0".
std::string_view version();

// The git commit it was built from (12 hex digits, then "-dirty" if the checkout had changes), or
// empty when the source wasn't a git checkout.
std::string_view build_commit();

// What --version prints first: "boundd 0.10.0 (commit 0123456789ab)".
std::string version_line(std::string_view program);

// All of --version: the line above, the license and where the source is.
std::string version_text(std::string_view program);

// How a program introduces itself to peers, in BIP 14's format: "/boundd:0.10.0/".
std::string user_agent(std::string_view program);

// True when the arguments are just --version (or -version, -v).
bool asks_for_version(int argc, char** argv);

}  // namespace bound
