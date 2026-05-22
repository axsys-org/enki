
#ifndef ENKI_WISP_H
#define ENKI_WISP_H

#include <enki/arena.h>
#include <enki/allocator.h>
#include <enki/value.h>
#include <enki/gc.h>
#include <setjmp.h>

typedef struct _wisp_env_entry {
  bool mac_f;
  enki_value val_v;
} wisp_env_entry;

typedef struct _wisp_env {
  char* key;
  wisp_env_entry* value;
} wisp_env;

typedef struct _wisp_rt {
  enki_gc* gc;

  jmp_buf errjmp;
  char err_f; // has errjmp been set
  char* msg_c;
  wisp_env* env;
} wisp_rt;


wisp_rt* wisp_rt_alloc(enki_allocator sys_a);
void wisp_rt_free(enki_allocator sys_a, wisp_rt* rt);

enki_value wisp_parse(wisp_rt* rt, char** str);
enki_value wisp_eval(wisp_rt* rt, enki_value val_v);


#endif
