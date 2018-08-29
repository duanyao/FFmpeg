#ifndef UPROFILER_H
#define UPROFILER_H

#include <stdint.h>
#include <stdlib.h>

#if !defined(thread_local) && !defined(__cplusplus)
  #if defined(__GNUC__)
    #define thread_local __thread
  #elif defined(_MSC_VER)
    #define thread_local __declspec(thread)
  #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uprofile_record {
  const char* name;
  int64_t time;
  int64_t start_count;
  int64_t end_count;
  struct uprofile_record* next;
} uprofile_record;

void uprofile_register(uprofile_record * r, const char* name);
int64_t uprofile_tick(void);

double uprofile_get_time(void);

void uprofile_write_log(const char* fname);
void uprofile_write_log_std(int std_stream);
void uprofile_profile_noop(void);

#define PROFILE_START(pname) \
  static uprofile_record a_uprofile_record = {0, 0, 0, 0}; \
  uprofile_register(&a_uprofile_record, pname); \
  a_uprofile_record.start_count += 1; \
  int64_t uprofile_t0 = uprofile_tick();

#define PROFILE_START_TL(pname) \
  static thread_local uprofile_record a_uprofile_record = {0, 0, 0, 0}; \
  uprofile_register(&a_uprofile_record, pname); \
  a_uprofile_record.start_count += 1; \
  int64_t uprofile_t0 = uprofile_tick();

#define PROFILE_END \
  a_uprofile_record.time += uprofile_tick() - uprofile_t0; \
  a_uprofile_record.end_count++;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* UPROFILER_H */