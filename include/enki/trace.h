#pragma once

#include "enki/gc.h"
#include "enki/interp.h"

void enki_trace_interp(enki_interpreter* i);
void enki_trace_value(enki_gc* gc, void* obj);
