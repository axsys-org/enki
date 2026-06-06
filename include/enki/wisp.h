#ifndef ENKI_WISP_H
#define ENKI_WISP_H

#include <enki/allocator.h>
#include <enki/bytecode.h>
#include <enki/run.h>
#include <stdbool.h>
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
  jmp_buf errjmp;
  char err_f; // has errjmp been set
  char* msg_c;
  wisp_env_entry* env;
  er_law_compiler law_compiler;
  er_val law_compiler_v;
  bool compiling_law_f;
} wisp_rt;

wisp_rt* wisp_rt_alloc(const enki_allocator* loc_a);
void wisp_rt_free(const enki_allocator* loc_a, wisp_rt* rt);

er_val wisp_parse(wisp_rt* rt, char** str);
er_val wisp_macroexpand(wisp_rt* rt, er_val val_v);
er_val wisp_thunk(wisp_rt* rt, er_val val_v);
er_val wisp_eval(wisp_rt* rt, er_val val_v);
bool wisp_rt_set_law_compiler(wisp_rt* rt, er_val compiler_v);
er_val wisp_law_make(wisp_rt* rt, er_val nam_v, er_val bod_v, uint32_t ari_d);
char* wisp_print_value(wisp_rt* rt, er_val val_v, size_t* out_s);

#endif
