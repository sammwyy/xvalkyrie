#pragma once
#include <cstdint>

namespace xv::host {
// What this process takes of the machine: its share of all the processors' time since the
// last reading (0 at the first), the memory it holds (its working set) and the memory the
// machine has.
struct ProcessUsage {
    double processor = 0;
    std::uint64_t memory = 0, installed = 0;
};
ProcessUsage process_usage();
} // namespace xv::host
