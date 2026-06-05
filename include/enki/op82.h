#pragma once

#include "enki/interp.h"

void op82_write(enki_interpreter* i);
void op82_closefd(enki_interpreter* i);
void op82_read(enki_interpreter* i);
void op82_output(enki_interpreter* i);
void op82_warn(enki_interpreter* i);
void op82_input(enki_interpreter* i);
void op82_now(enki_interpreter* i);
void op82_read_file(enki_interpreter* i);
void op82_stamp(enki_interpreter* i);
void op82_listen(enki_interpreter* i);
void op82_accept(enki_interpreter* i);
void op82_spawn(enki_interpreter* i);
void op82_send(enki_interpreter* i);
void op82_send_caps(enki_interpreter* i);
void op82_rcv(enki_interpreter* i);
void op82_close_handle(enki_interpreter* i);
