#pragma once

#include <cstdint>
#include <string>

namespace bound {

// The system clock, as Unix seconds.
std::int64_t unix_now();

// "YYYY-MM-DD HH:MM:SS" in UTC, for logs and the explorer.
std::string format_utc(std::int64_t unix_seconds);

}  // namespace bound
