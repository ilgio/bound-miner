#include "util/version.h"

#include "bound_build_info.h"  // generated in the build directory: BOUND_BUILD_COMMIT

#include <format>

#ifndef BOUND_VERSION
#error "CMakeLists.txt defines BOUND_VERSION from project(bound VERSION ...)"
#endif

namespace bound {

std::string_view version() { return BOUND_VERSION; }

std::string_view build_commit() { return BOUND_BUILD_COMMIT; }

std::string version_line(std::string_view program) {
    std::string line = std::format("{} {}", program, version());
    if (!build_commit().empty()) line += std::format(" (commit {})", build_commit());
    return line;
}

std::string version_text(std::string_view program) {
    return std::format("{}\nCopyright (c) 2026 BOUND. MIT license.\nSource: {}\n", version_line(program), kProjectUrl);
}

std::string user_agent(std::string_view program) { return std::format("/{}:{}/", program, version()); }

bool asks_for_version(int argc, char** argv) {
    if (argc != 2) return false;
    const std::string_view arg = argv[1];
    return arg == "--version" || arg == "-version" || arg == "-v";
}

}  // namespace bound
