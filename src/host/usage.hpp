#pragma once
#include <cstdint>

namespace xv::host {
// What this process takes of the machine: its share of all the processors' time since the
// last reading (0 at the first), the memory it holds (its working set) and the memory the
// machine has; the share of the GPU's busiest kind of engine since the last reading, and the
// video memory it holds of the first adapter's own.
struct ProcessUsage {
    double processor = 0, graphics = 0;
    std::uint64_t memory = 0, installed = 0, video_memory = 0, video_installed = 0;
};
ProcessUsage process_usage();
} // namespace xv::host
