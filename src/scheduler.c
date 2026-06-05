

#include "enki/interp.h"
#include "enki/scheduler.h"

void enki_schedueler_enqueue(enki_schedueler* s, enki_actor* act) {
    if(s == NULL || act == NULL) return;
    if(act->state != ENKI_ACTOR_RUNNABLE) return;
    if(act->queued) return;
    act->queued = true;
    act->next = NULL;
    if(s->tail == NULL) {
        s->head = act;
        s->tail = act;
    }
    else {
        s->tail->next = act;
        s->tail = act;
    }
}

void enki_schedueler_run(enki_schedueler* s) {
    while(s->head != NULL) {
        enki_actor* act = s->head;
        s->head = act->next;
        if(s->head == NULL) s->tail = NULL;
        act->next = NULL;
        act->queued = false;
        if(act->state != ENKI_ACTOR_RUNNABLE) continue;
        size_t steps = 0;
        while(steps < ENKI_SCHEDULER_TIMESLICE) {
            if(act->state != ENKI_ACTOR_RUNNABLE) {
                break;
            }
            if(act->i == NULL) {
                act->state = ENKI_ACTOR_DONE;
                break;
            }
            if(act->i->halted) {
                act->state = ENKI_ACTOR_DONE;
                break;
            }
            enki_interp_step(act->i);
            steps += 1;
        }
        if(act->state == ENKI_ACTOR_RUNNABLE) {
            enki_schedueler_enqueue(s, act);
        }
    }
}
