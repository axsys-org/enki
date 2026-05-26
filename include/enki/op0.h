#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "enki/app.h"
#include "enki/interp.h"
#include "enki/law.h"
#include "enki/pin.h"
#include "enki/value.h"
#include "enki/vector.h"

void op0_pin(enki_interpreter* i);
void op0_law(enki_interpreter* i);
void op0_elim(enki_interpreter* i);
