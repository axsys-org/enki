#ifndef ENKI_WISP_H
#define ENKI_WISP_H

#include <enki/allocator.h>
#include <enki/gc.h>
#include <enki/run.h>
#include <setjmp.h>
#include <stddef.h>

typedef struct wisp_env_entry {
  er_val key_v;
  er_val val_v;
  int mac_f;
  struct wisp_env_entry* next;
} wisp_env_entry;

typedef struct wisp_rt {
  const enki_allocator* loc_a;
  enki_gc* gc;
  jmp_buf errjmp;
  char err_f; // has errjmp been set
  char* msg_c;
  wisp_env_entry* env;
  er_val gc_tmp_v[8192];
  size_t gc_tmp_s;
} wisp_rt;

wisp_rt* wisp_rt_alloc(enki_gc* gc);
void wisp_rt_free(wisp_rt* rt);
void wisp_rt_trace(enki_gc* gc, void* root);

er_val wisp_parse(wisp_rt* rt, char** str);
er_val wisp_macroexpand(wisp_rt* rt, er_val val_v);
er_val wisp_thunk(wisp_rt* rt, er_val val_v);
er_val wisp_eval(wisp_rt* rt, er_val val_v);
char* wisp_print_value(wisp_rt* rt, er_val val_v, size_t* out_s);

#endif
