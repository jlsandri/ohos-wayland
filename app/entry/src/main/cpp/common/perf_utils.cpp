#include "perf_utils.h"

namespace winehua {

uint64_t PerfNowUs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        PerfClock::now().time_since_epoch()).count());
}

} // namespace winehua
