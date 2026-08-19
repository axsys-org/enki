#include <stddef.h>

#include "actor_internal.h"

/* The deterministic actor executor lives unchanged in pkg/enki/src/actor.c.
 * Only its optional libcurl backend is absent from the guest. */
void er_http_service(er_scheduler* scheduler, er_actor* actor, uint32_t argc,
                     pl_val* args) {
  (void)scheduler;
  (void)argc;
  (void)args;
  er_crash_msg(actor, "unsupported on enki-os");
}

void er_http_pump(er_scheduler* scheduler) { (void)scheduler; }
bool er_http_idle(er_scheduler* scheduler) {
  (void)scheduler;
  return false;
}
bool er_http_outstanding(const er_scheduler* scheduler) {
  (void)scheduler;
  return false;
}
bool er_http_mt_pump(er_scheduler* scheduler) {
  (void)scheduler;
  return false;
}
void er_http_teardown(er_scheduler* scheduler) { (void)scheduler; }
size_t er_http_inflight_count(const er_scheduler* scheduler) {
  (void)scheduler;
  return 0;
}
