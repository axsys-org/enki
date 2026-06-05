#pragma once

#include <stddef.h>
#include <stdint.h>

#include "enki/error.h"
#include "enki/gc.h"
#include "enki/value.h"

typedef struct enki_plan {
  enki_gc* gc;
  enki_error error;
  enki_value error_v;
  uint64_t whnf_eval_s;
  uint64_t apply_s;
} enki_plan;

void enki_plan_init(enki_plan* plan, enki_gc* gc);
void enki_plan_clear_error(enki_plan* plan);

enki_error enki_plan_eval_whnf(enki_plan* plan, enki_value val_v,
                               enki_value* out_v);
enki_error enki_plan_eval_nf(enki_plan* plan, enki_value val_v,
                             enki_value* out_v);
enki_error enki_plan_apply(enki_plan* plan, enki_value fn_v, size_t n_args_s,
                           const enki_value* args_v, enki_value* out_v);
enki_error enki_plan_nat(enki_plan* plan, enki_value val_v, enki_value* out_v);
enki_error enki_plan_arity(enki_plan* plan, enki_value val_v, size_t* out_s);
