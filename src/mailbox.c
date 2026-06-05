#include <stdlib.h>
#include <string.h>

#include "enki/error.h"
#include "enki/interp.h"
#include "enki/mailbox.h"

void enki_mailbox_init(enki_interpreter* i, enki_mailbox* inbox) {
    inbox->inbound = 0;
    inbox->head = NULL;
    inbox->tail = NULL;
    inbox->i = i;
}

void enki_mailbox_push(enki_mailbox* inbox, enki_message* msg) {
    if(inbox == NULL || inbox->i == NULL) abort();
    enki_interpreter* i = inbox->i;
    enki_message* node = (enki_message*)i->our_a.alloc(i->our_a.ctx, sizeof(enki_message));
    if(node == NULL) enki_interp_throw(i, ENKI_ERROR_OOM, 0);
    node->payload = msg->payload;
    node->n_caps = msg->n_caps;
    if(msg->n_caps > CAPS_MAX) enki_interp_throw(i, ENKI_ERROR_BOUNDS, msg->n_caps);
    memcpy(node->caps, msg->caps, msg->n_caps * sizeof(enki_actor*));
    node->next = NULL;
    if(inbox->tail == NULL) {
        inbox->head = node;
        inbox->tail = node;
    }
    else {
        inbox->tail->next = node;
        inbox->tail = node;
    }
    inbox->inbound += 1;
}

enki_message enki_mailbox_pop(enki_mailbox* inbox) {
   if(inbox == NULL || inbox->i == NULL) abort();
    enki_interpreter* i = inbox->i;
    enki_message empty = {0};
    if(inbox->head == NULL) return empty;
    enki_message* node = inbox->head;
    inbox->head = node->next;
    if(inbox->head == NULL) {
        inbox->tail = NULL;
    }
    inbox->inbound -= 1;
    enki_message msg = {0};
    msg.payload = node->payload;
    msg.n_caps = node->n_caps;
    memcpy(msg.caps, node->caps, node->n_caps * sizeof(enki_actor*));
    i->our_a.free(i->our_a.ctx, node);
    return msg;
}
