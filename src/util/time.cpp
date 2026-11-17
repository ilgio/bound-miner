#include "util/time.h"

#include <chrono>
#include <format>

namespace bound {

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// With plain integers rather than a chrono format spec: MSVC 14.38 has an internal compiler error
// on translation units that use two different ones.
std::string format_utc(std::int64_t unix_seconds) {
    const std::chrono::sys_seconds t{std::chrono::seconds{unix_seconds}};
    const auto day = std::chrono::floor<std::chrono::days>(t);
    const std::chrono::year_month_day date{day};
    const std::chrono::hh_mm_ss time{t - day};
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", static_cast<int>(date.year()),
                       static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()), time.hours().count(),
                       time.minutes().count(), time.seconds().count());
}

}  // namespace bound
