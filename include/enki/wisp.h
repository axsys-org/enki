#ifndef ENKI_WISP_H
#define ENKI_WISP_H

#include <enki/allocator.h>
#include <enki/run.h>
#include <setjmp.h>
#include <stddef.h>

typedef struct _wisp_env_entry {
  er_val key_v;
  er_val val_v;
  int mac_f;
  struct _wisp_env_entry* next;
} wisp_env_entry;

typedef struct _wisp_rt {
  const enki_allocator* loc_a;
  jmp_buf errjmp;
  char err_f; // has errjmp been set
  char* msg_c;
  wisp_env_entry* env;
} wisp_rt;

wisp_rt* wisp_rt_alloc(const enki_allocator* sys_a);
void wisp_rt_free(const enki_allocator* sys_a, wisp_rt* rt);

er_val wisp_parse(wisp_rt* rt, char** str);
er_val wisp_eval(wisp_rt* rt, er_val val_v);
char* wisp_print_value(wisp_rt* rt, er_val val_v, size_t* out_s);

#endif
