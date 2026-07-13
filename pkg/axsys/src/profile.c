#include "axsys/profile.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct ax_profile_json_state {
  pthread_mutex_t mu;
  FILE* file;
  struct timespec start;
  uint64_t pid;
  bool first;
  bool failed;
} ax_profile_json_state;

static ax_profile_json_state ax_json = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
};
static _Atomic bool ax_json_enabled = false;

static bool ax_json_write(const void* data, size_t n) {
  if (ax_json.failed)
    return false;
  if (fwrite(data, 1, n, ax_json.file) != n) {
    ax_json.failed = true;
    return false;
  }
  return true;
}

static bool ax_json_char(int ch) {
  if (ax_json.failed)
    return false;
  if (fputc(ch, ax_json.file) == EOF) {
    ax_json.failed = true;
    return false;
  }
  return true;
}

static void ax_json_string(const uint8_t* s, size_t n) {
  static const char hex[] = "0123456789abcdef";
  (void)ax_json_char('"');
  for (size_t i = 0; i < n && !ax_json.failed; i++) {
    uint8_t ch = s[i];
    switch (ch) {
    case '"':
      (void)ax_json_write("\\\"", 2);
      break;
    case '\\':
      (void)ax_json_write("\\\\", 2);
      break;
    case '/':
      (void)ax_json_write("\\/", 2);
      break;
    case '\b':
      (void)ax_json_write("\\b", 2);
      break;
    case '\f':
      (void)ax_json_write("\\f", 2);
      break;
    case '\n':
      (void)ax_json_write("\\n", 2);
      break;
    case '\r':
      (void)ax_json_write("\\r", 2);
      break;
    case '\t':
      (void)ax_json_write("\\t", 2);
      break;
    default:
      if (ch >= 0x20 && ch < 0x7f) {
        (void)ax_json_char(ch);
      } else {
        char esc[6] = {'\\', 'u', '0', '0', hex[ch >> 4], hex[ch & 0xf]};
        (void)ax_json_write(esc, sizeof(esc));
      }
      break;
    }
  }
  (void)ax_json_char('"');
}

static uint64_t ax_json_now_us(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    ax_json.failed = true;
    return 0;
  }
  time_t sec = now.tv_sec - ax_json.start.tv_sec;
  long nsec = now.tv_nsec - ax_json.start.tv_nsec;
  if (nsec < 0) {
    sec--;
    nsec += 1000000000L;
  }
  if (sec < 0)
    return 0;
  return (uint64_t)sec * UINT64_C(1000000) + (uint64_t)nsec / 1000;
}

static bool ax_json_event_prefix(void) {
  if (!ax_json.first && !ax_json_char(','))
    return false;
  ax_json.first = false;
  return true;
}

bool ax_profile_json_start(const char* path) {
  if (path == NULL || path[0] == '\0')
    return false;
  pthread_mutex_lock(&ax_json.mu);
  if (ax_json.file != NULL) {
    pthread_mutex_unlock(&ax_json.mu);
    return false;
  }

  FILE* file = fopen(path, "wb");
  if (file == NULL) {
    pthread_mutex_unlock(&ax_json.mu);
    return false;
  }
  ax_json.file = file;
  ax_json.pid = (uint64_t)getpid();
  ax_json.first = true;
  ax_json.failed = clock_gettime(CLOCK_MONOTONIC, &ax_json.start) != 0;
  if (!ax_json.failed)
    (void)ax_json_write("{\"traceEvents\":[", strlen("{\"traceEvents\":["));
  if (ax_json.failed) {
    (void)fclose(ax_json.file);
    ax_json.file = NULL;
    pthread_mutex_unlock(&ax_json.mu);
    return false;
  }
  atomic_store_explicit(&ax_json_enabled, true, memory_order_release);
  pthread_mutex_unlock(&ax_json.mu);
  return true;
}

bool ax_profile_json_enabled(void) {
  return atomic_load_explicit(&ax_json_enabled, memory_order_acquire);
}

void ax_profile_json_thread_name(uint64_t tid, const char* name,
                                 size_t name_n) {
  if (!ax_profile_json_enabled())
    return;
  pthread_mutex_lock(&ax_json.mu);
  if (ax_json.file != NULL && ax_json_event_prefix()) {
    uint64_t ts = ax_json_now_us();
    int wrote = fprintf(ax_json.file,
                        "{\"cat\":\"__metadata\",\"name\":\"thread_name\","
                        "\"ph\":\"M\",\"ts\":%" PRIu64 ",\"pid\":%" PRIu64
                        ",\"tid\":%" PRIu64 ",\"args\":{\"name\":",
                        ts, ax_json.pid, tid);
    if (wrote < 0)
      ax_json.failed = true;
    ax_json_string((const uint8_t*)name, name_n);
    (void)ax_json_write("}}", 2);
  }
  pthread_mutex_unlock(&ax_json.mu);
}

static void ax_profile_json_zone(char phase, uint64_t tid, uint64_t zone,
                                 const uint8_t* name, size_t name_n) {
  if (!ax_profile_json_enabled())
    return;
  pthread_mutex_lock(&ax_json.mu);
  if (ax_json.file != NULL && ax_json_event_prefix()) {
    uint64_t ts = ax_json_now_us();
    static const char prefix[] = "{\"cat\":\"splan.zone\",\"name\":";
    (void)ax_json_write(prefix, sizeof(prefix) - 1);
    ax_json_string(name, name_n);
    int wrote = fprintf(ax_json.file,
                        ",\"ph\":\"%c\",\"ts\":%" PRIu64 ",\"pid\":%" PRIu64
                        ",\"tid\":%" PRIu64 ",\"args\":{\"zone\":%" PRIu64 "}}",
                        phase, ts, ax_json.pid, tid, zone);
    if (wrote < 0)
      ax_json.failed = true;
  }
  pthread_mutex_unlock(&ax_json.mu);
}

void ax_profile_json_zone_begin(uint64_t tid, uint64_t zone,
                                const uint8_t* name, size_t name_n) {
  ax_profile_json_zone('B', tid, zone, name, name_n);
}

void ax_profile_json_zone_end(uint64_t tid, uint64_t zone, const uint8_t* name,
                              size_t name_n) {
  ax_profile_json_zone('E', tid, zone, name, name_n);
}

bool ax_profile_json_finish(void) {
  atomic_store_explicit(&ax_json_enabled, false, memory_order_release);
  pthread_mutex_lock(&ax_json.mu);
  if (ax_json.file == NULL) {
    pthread_mutex_unlock(&ax_json.mu);
    return true;
  }

  (void)ax_json_write("],\"displayTimeUnit\":\"ms\"}\n",
                      strlen("],\"displayTimeUnit\":\"ms\"}\n"));
  bool ok = !ax_json.failed;
  if (fclose(ax_json.file) != 0)
    ok = false;
  ax_json.file = NULL;
  ax_json.failed = false;
  ax_json.first = true;
  pthread_mutex_unlock(&ax_json.mu);
  return ok;
}
