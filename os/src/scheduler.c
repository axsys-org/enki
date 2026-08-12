#include "enki/actor.h"

er_drive_status er_scheduler_drive(er_scheduler* scheduler, er_actor* root) {
  (void)scheduler;
  (void)root;
  return ER_DRIVE_EXN;
}

er_drive_status er_mt_executor_drive(er_mt_executor* executor, er_actor* root) {
  (void)executor;
  (void)root;
  return ER_DRIVE_EXN;
}
