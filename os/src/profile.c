#include "axsys/profile.h"

bool ax_profile_json_start(const char* path) { (void)path; return false; }
bool ax_profile_json_enabled(void) { return false; }
void ax_profile_json_thread_name(uint64_t tid, const char* name, size_t n) {
  (void)tid; (void)name; (void)n;
}
void ax_profile_json_zone_begin(uint64_t tid, uint64_t zone,
                                const uint8_t* name, size_t n) {
  (void)tid; (void)zone; (void)name; (void)n;
}
void ax_profile_json_zone_end(uint64_t tid, uint64_t zone,
                              const uint8_t* name, size_t n) {
  (void)tid; (void)zone; (void)name; (void)n;
}
void ax_profile_json_span_begin(uint64_t tid, uint64_t span,
                                const uint8_t* category, size_t category_n,
                                const uint8_t* name, size_t name_n) {
  (void)tid; (void)span; (void)category; (void)category_n; (void)name; (void)name_n;
}
void ax_profile_json_span_end(uint64_t tid, uint64_t span,
                              const uint8_t* category, size_t category_n,
                              const uint8_t* name, size_t name_n) {
  (void)tid; (void)span; (void)category; (void)category_n; (void)name; (void)name_n;
}
bool ax_profile_json_finish(void) { return true; }
