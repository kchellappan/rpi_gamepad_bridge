#pragma once
// Real-time hygiene. Worth doing, but keep it in proportion: these shave microseconds off
// a problem whose floor is milliseconds of USB polling on both ends. They are garnish,
// not the meal. Every one of them warns rather than aborts on failure, because running
// without SCHED_FIFO is fine and refusing to run is not.

#include <cstdint>
#include <string>

namespace gpb {

void lock_memory();                       // mlockall, so we never page-fault mid-loop
bool set_realtime_priority(int prio);     // SCHED_FIFO
bool pin_to_cpu(int cpu);

uint64_t now_mono_ns();
uint64_t now_real_ns();

}  // namespace gpb
