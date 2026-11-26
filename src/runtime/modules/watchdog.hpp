#pragma once

namespace xv::runtime::modules {
// XV_WATCHDOG: called at each present; dumps every thread's stack once frames stop for 5 s.
// The first call starts the count, so a title that never presents is seen as well.
void frame_watchdog();
// XV_WATCHDOG, XV_THREAD_DUMP and XV_PROFILE as the environment has them: before the title runs
void start_watchdog();
} // namespace xv::runtime::modules
