
#ifndef ENKI_WISP_H
#define ENKI_WISP_H

#include <enki/arena.h>
#include <enki/allocator.h>
#include <enki/value.h>
#include <enki/gc.h>
#include <enki/bst.h>
#include <setjmp.h>

typedef struct _wisp_env_entry {
  // TODO: fix should
  int mac_f;
  enki_value val_v;
} wisp_env_entry;

typedef struct _wisp_env {
  char* key;
  wisp_env_entry* value;
} wisp_env;

typedef struct _wisp_rt {
  enki_interpreter* i;
  enki_gc* gc;
  const enki_allocator* loc_a;
  jmp_buf errjmp;
  char err_f; // has errjmp been set
  char* msg_c;
  enki_bst_tree* env;
} wisp_rt;


wisp_rt* wisp_rt_alloc(const enki_allocator* sys_a);
void wisp_rt_free(const enki_allocator* sys_a, wisp_rt* rt);

enki_value wisp_parse(wisp_rt* rt, char** str);
enki_value wisp_eval(wisp_rt* rt, enki_value val_v);


#endif
