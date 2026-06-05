
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "enki/mailbox.h"

#define ENKI_SCHEDULER_TIMESLICE 256
#define HANDLES_MAX 1024

typedef struct enki_interpreter enki_interpreter;

typedef enum {
  ENKI_ACTOR_RUNNABLE,
  ENKI_ACTOR_BLOCKED,
  ENKI_ACTOR_DONE,
} actor_state;

typedef struct enki_actor {
  enki_interpreter* i;
  enki_mailbox inbox;
  size_t next_handle;
  struct enki_actor* handles[HANDLES_MAX];
  actor_state state;
  struct enki_actor* next;
  bool queued;
} enki_actor;

typedef struct enki_schedueler {
    enki_actor* head;
    enki_actor* tail; 
} enki_schedueler;

typedef enki_schedueler enki_scheduler;

void enki_schedueler_run(enki_schedueler* s);
void enki_schedueler_enqueue(enki_schedueler* s, enki_actor* act);
