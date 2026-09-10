#include "gpb/rt.hpp"

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>

#include <cstdio>

namespace gpb {

void lock_memory() {
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    std::fprintf(stderr, "[rt] mlockall failed (continuing; expect occasional page faults)\n");
  }
}

bool set_realtime_priority(int prio) {
  if (prio <= 0) return true;
  sched_param p{};
  p.sched_priority = prio;
  if (sched_setscheduler(0, SCHED_FIFO, &p) != 0) {
    std::fprintf(stderr, "[rt] SCHED_FIFO prio %d failed (need CAP_SYS_NICE?); continuing\n", prio);
    return false;
  }
  return true;
}

bool pin_to_cpu(int cpu) {
  if (cpu < 0) return true;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
    std::fprintf(stderr, "[rt] pinning to cpu %d failed; continuing\n", cpu);
    return false;
  }
  return true;
}

uint64_t now_mono_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

uint64_t now_real_ns() {
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

}  // namespace gpb
