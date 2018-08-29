#include "uprofiler.h"
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#elif defined(_POSIX_VERSION)
#include <pthread.h>
#endif

static thread_local uprofile_record * tl_uprofile_record_list = NULL;

void uprofile_register(uprofile_record * r, const char* name) {
  if (r->name) return;
  r->name = name;
  r->next = tl_uprofile_record_list;
  tl_uprofile_record_list = r;
}

int64_t uprofile_tick(void) {
#ifdef _WIN32
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
#endif
}

double uprofile_get_time(void) {
#ifdef _WIN32
  LARGE_INTEGER f, t;
  QueryPerformanceCounter(&t);
  QueryPerformanceFrequency(&f);
  return t.QuadPart / (double)f.QuadPart;
#endif
}

static double tick2second(int64_t t) {
#ifdef _WIN32
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  return t / (double)f.QuadPart;
#endif
}

static int get_tid(void) {
#ifdef _WIN32
  return GetCurrentThreadId();
#elif defined(_POSIX_VERSION)
  return pthread_self();
#else
  return 0;
#endif
}

void uprofile_profile_noop(void) {
  double t0 = uprofile_get_time(), t1;
  for(t1 = t0; t1 - t0 < 1; t1 = uprofile_get_time()) {
    for(int i = 0; i < 10000; i++) {
      PROFILE_START("noop");
      PROFILE_END;
    }
  }
  printf("uprofile_profile_noop:time(ms):%.3f\n", 1000 * (t1 - t0));
  t0 = t1;
  for(t1 = t0; t1 - t0 < 1; t1 = uprofile_get_time()) {
    for(int i = 0; i < 10000; i++) {
      PROFILE_START_TL("noop_tl");
      PROFILE_END;
    }
  }
  printf("profile_noop_tl:time(ms):%.3f\n", 1000 * (t1 - t0));
}

void uprofile_write_log(const char* fname) {

}

void uprofile_write_log_std(int std_stream) {
  FILE* fp = std_stream == 1? stdout : stderr;
  fprintf(fp, "=============== Start of uProfile report tid: %d ===================\n", get_tid());
  fprintf(fp, "count\ttime_tot(ms)\ttime_call(us)\tmismatch\tname\n");
  for(uprofile_record * pr = tl_uprofile_record_list; pr; pr = pr->next) {
    double time = tick2second(pr->time);
    fprintf(fp, "%lld\t%.1f\t%.3f\t%lld\t%s\n", pr->end_count, time * 1000,
            time * 1E6 / pr->end_count, pr->start_count - pr->end_count, pr->name);
  }
  fprintf(fp, "================ End of uProfile report tid: %d ====================\n", get_tid());
}
