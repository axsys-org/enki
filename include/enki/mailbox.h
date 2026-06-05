#pragma once

#include <stddef.h>

#include "enki/value.h"

#define CAPS_MAX 16

typedef struct enki_actor enki_actor;
typedef struct enki_interpreter enki_interpreter;

typedef struct enki_message {
  enki_value payload;
  enki_actor* caps[CAPS_MAX];
  size_t n_caps;
  struct enki_message* next;
} enki_message;

typedef struct {
    enki_message* head;
    enki_message* tail;
    size_t inbound;
    enki_interpreter* i;
} enki_mailbox;

void enki_mailbox_init(enki_interpreter* i, enki_mailbox* inbox);
void enki_mailbox_push(enki_mailbox* inbox, enki_message* msg);
enki_message enki_mailbox_pop(enki_mailbox* inbox);
