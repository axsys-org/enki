#include "test_interp.h"
#include "enki/app.h"
#include "enki/error.h"
#include "enki/fd.h"
#include "enki/interp.h"
#include "enki/mailbox.h"
#include "enki/nat.h"
#include "enki/op82.h"
#include "enki/scheduler.h"
#include "enki/value.h"

#include <criterion/criterion.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static enki_interpreter* fixture_interp;

static void setup(void)
{
    fixture_interp = enki_test_interp_create(1024 * 1024, 0);
    cr_assert_not_null(fixture_interp);
}

static void teardown(void)
{
    enki_test_interp_destroy(fixture_interp);
    fixture_interp = NULL;
}

static int catch_op(void (*op)(enki_interpreter*))
{
    fixture_interp->has_error_jmp = true;
    if(setjmp(fixture_interp->error_jmp) == 0) {
        op(fixture_interp);
        fixture_interp->has_error_jmp = false;
        return ENKI_ERROR_OK;
    }
    fixture_interp->has_error_jmp = false;
    return fixture_interp->error_code;
}

static void init_actor(enki_actor* actor)
{
    memset(actor, 0, sizeof(*actor));
    actor->i = fixture_interp;
    actor->state = ENKI_ACTOR_RUNNABLE;
    actor->next_handle = 1;
    actor->handles[0] = actor;
    enki_mailbox_init(fixture_interp, &actor->inbox);
}

static void reset_fd_table(void)
{
    for(size_t k = 3; k < FD_MAX; k++) {
        if(enki_global_fd_table.slots[k].live) {
            (void)close(enki_global_fd_table.slots[k].fd);
        }
    }
    memset(&enki_global_fd_table, 0, sizeof(enki_global_fd_table));
    enki_global_fd_table.next = 3;
    enki_global_fd_table.slots[0] = (enki_fd_slot){.live = true, .fd = 0, .refs = 1};
    enki_global_fd_table.slots[1] = (enki_fd_slot){.live = true, .fd = 1, .refs = 1};
    enki_global_fd_table.slots[2] = (enki_fd_slot){.live = true, .fd = 2, .refs = 1};
}

TestSuite(op82_runtime, .init = setup, .fini = teardown);

Test(op82_runtime, fd_table_add_get_retain_release_and_invalid_edges)
{
    reset_fd_table();
    int fd = dup(STDOUT_FILENO);
    cr_assert_geq(fd, 0);

    size_t h = enki_fd_add(fd);
    cr_assert_eq(h, 3);
    cr_assert_eq(enki_fd_get(h), fd);
    cr_assert_eq(enki_global_fd_table.slots[h].refs, 1);

    cr_assert_eq(enki_fd_retain(h), 0);
    cr_assert_eq(enki_global_fd_table.slots[h].refs, 2);
    cr_assert_eq(enki_fd_release(h), 0);
    cr_assert_eq(enki_global_fd_table.slots[h].refs, 1);
    cr_assert_eq(enki_fd_release(h), 0);
    cr_assert_eq(enki_fd_get(h), -1);
    cr_assert_eq(enki_fd_release(h), -1);
    cr_assert_eq(enki_fd_get(FD_MAX), -1);
    cr_assert_eq(enki_fd_close(FD_MAX), -1);
}

Test(op82_runtime, mailbox_fifo_copy_empty_and_caps_bounds)
{
    enki_mailbox inbox;
    enki_mailbox_init(fixture_interp, &inbox);
    enki_actor cap_a;
    enki_actor cap_b;
    init_actor(&cap_a);
    init_actor(&cap_b);

    enki_message one = {.payload = 11, .n_caps = 1, .caps = {&cap_a}};
    enki_message two = {.payload = 22, .n_caps = 1, .caps = {&cap_b}};
    enki_mailbox_push(&inbox, &one);
    one.payload = 99;
    one.caps[0] = NULL;
    enki_mailbox_push(&inbox, &two);

    cr_assert_eq(inbox.inbound, 2);
    enki_message got = enki_mailbox_pop(&inbox);
    cr_assert_eq(got.payload, 11);
    cr_assert_eq(got.n_caps, 1);
    cr_assert_eq(got.caps[0], &cap_a);
    got = enki_mailbox_pop(&inbox);
    cr_assert_eq(got.payload, 22);
    cr_assert_eq(got.caps[0], &cap_b);
    cr_assert_eq(inbox.inbound, 0);
    cr_assert_null(inbox.head);
    cr_assert_null(inbox.tail);

    got = enki_mailbox_pop(&inbox);
    cr_assert_eq(got.payload, 0);
    cr_assert_eq(got.n_caps, 0);

    enki_message bad = {.payload = 1, .n_caps = CAPS_MAX + 1};
    fixture_interp->has_error_jmp = true;
    if(setjmp(fixture_interp->error_jmp) == 0) {
        enki_mailbox_push(&inbox, &bad);
        cr_assert_fail("mailbox push should reject too many caps");
    }
    cr_assert_eq(fixture_interp->error_code, ENKI_ERROR_BOUNDS);
    fixture_interp->has_error_jmp = false;
}

Test(op82_runtime, op82_write_and_read_use_shared_fd_table)
{
    reset_fd_table();
    int p[2];
    cr_assert_eq(pipe(p), 0);
    size_t write_h = enki_fd_add(p[1]);
    cr_assert_neq(write_h, ENKI_FD_INVALID);

    uint8_t bytes[] = {'o', 'k'};
    fixture_interp->stack_v[0] = (enki_value)write_h;
    fixture_interp->stack_v[1] = enki_bytes_to_nat(fixture_interp, bytes, sizeof(bytes));
    fixture_interp->sp = 2;
    op82_write(fixture_interp);
    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 0);

    uint8_t got[2] = {0};
    cr_assert_eq(read(p[0], got, sizeof(got)), (ssize_t)sizeof(got));
    cr_assert_arr_eq(got, bytes, sizeof(bytes));
    cr_assert_eq(enki_fd_close(write_h), 0);
    close(p[0]);

    cr_assert_eq(pipe(p), 0);
    uint8_t in[] = {'v', 'm', '!'};
    cr_assert_eq(write(p[1], in, sizeof(in)), (ssize_t)sizeof(in));
    close(p[1]);
    size_t read_h = enki_fd_add(p[0]);
    cr_assert_neq(read_h, ENKI_FD_INVALID);

    fixture_interp->stack_v[0] = (enki_value)read_h;
    fixture_interp->stack_v[1] = sizeof(in);
    fixture_interp->sp = 2;
    op82_read(fixture_interp);
    cr_assert_eq(fixture_interp->sp, 1);

    size_t len = 0;
    uint8_t* out = enki_nat_to_bytes(fixture_interp, fixture_interp->stack_v[0], &len);
    cr_assert_eq(len, sizeof(in));
    cr_assert_arr_eq(out, in, sizeof(in));
    cr_assert_eq(enki_fd_close(read_h), 0);
}

Test(op82_runtime, op82_closefd_closes_registered_descriptor)
{
    reset_fd_table();
    int fd = dup(STDOUT_FILENO);
    cr_assert_geq(fd, 0);
    size_t h = enki_fd_add(fd);
    cr_assert_neq(h, ENKI_FD_INVALID);

    fixture_interp->stack_v[0] = (enki_value)h;
    fixture_interp->sp = 1;
    op82_closefd(fixture_interp);

    cr_assert_eq(fixture_interp->stack_v[0], 0);
    cr_assert_eq(enki_fd_get(h), -1);

    fixture_interp->stack_v[0] = (enki_value)h;
    fixture_interp->sp = 1;
    cr_assert_eq(catch_op(op82_closefd), ENKI_ERROR_IO);

    fixture_interp->stack_v[0] = PTR_TO_ENKI(&fd);
    fixture_interp->sp = 1;
    cr_assert_eq(catch_op(op82_closefd), ENKI_ERROR_TYPE);
}

Test(op82_runtime, op82_output_and_warn_write_to_standard_streams)
{
    int out_pipe[2];
    int err_pipe[2];
    cr_assert_eq(pipe(out_pipe), 0);
    cr_assert_eq(pipe(err_pipe), 0);
    int saved_out = dup(STDOUT_FILENO);
    int saved_err = dup(STDERR_FILENO);
    cr_assert_geq(saved_out, 0);
    cr_assert_geq(saved_err, 0);

    uint8_t out_bytes[] = {'o', 'u', 't'};
    uint8_t err_bytes[] = {'e', 'r', 'r'};
    cr_assert_eq(dup2(out_pipe[1], STDOUT_FILENO), STDOUT_FILENO);
    fixture_interp->stack_v[0] = enki_bytes_to_nat(fixture_interp, out_bytes, sizeof(out_bytes));
    fixture_interp->sp = 1;
    op82_output(fixture_interp);
    cr_assert_eq(fixture_interp->stack_v[0], 0);
    cr_assert_eq(dup2(saved_out, STDOUT_FILENO), STDOUT_FILENO);
    close(out_pipe[1]);

    cr_assert_eq(dup2(err_pipe[1], STDERR_FILENO), STDERR_FILENO);
    fixture_interp->stack_v[0] = enki_bytes_to_nat(fixture_interp, err_bytes, sizeof(err_bytes));
    fixture_interp->sp = 1;
    op82_warn(fixture_interp);
    cr_assert_eq(fixture_interp->stack_v[0], 0);
    cr_assert_eq(dup2(saved_err, STDERR_FILENO), STDERR_FILENO);
    close(err_pipe[1]);

    uint8_t got[3] = {0};
    cr_assert_eq(read(out_pipe[0], got, sizeof(got)), (ssize_t)sizeof(got));
    cr_assert_arr_eq(got, out_bytes, sizeof(out_bytes));
    memset(got, 0, sizeof(got));
    cr_assert_eq(read(err_pipe[0], got, sizeof(got)), (ssize_t)sizeof(got));
    cr_assert_arr_eq(got, err_bytes, sizeof(err_bytes));

    close(out_pipe[0]);
    close(err_pipe[0]);
    close(saved_out);
    close(saved_err);
}

Test(op82_runtime, op82_input_reads_from_stdin)
{
    int p[2];
    cr_assert_eq(pipe(p), 0);
    int saved_in = dup(STDIN_FILENO);
    cr_assert_geq(saved_in, 0);

    uint8_t bytes[] = {'i', 'n'};
    cr_assert_eq(write(p[1], bytes, sizeof(bytes)), (ssize_t)sizeof(bytes));
    close(p[1]);
    cr_assert_eq(dup2(p[0], STDIN_FILENO), STDIN_FILENO);

    fixture_interp->stack_v[0] = sizeof(bytes);
    fixture_interp->sp = 1;
    op82_input(fixture_interp);

    cr_assert_eq(dup2(saved_in, STDIN_FILENO), STDIN_FILENO);
    close(saved_in);
    close(p[0]);

    size_t len = 0;
    uint8_t* out = enki_nat_to_bytes(fixture_interp, fixture_interp->stack_v[0], &len);
    cr_assert_eq(len, sizeof(bytes));
    cr_assert_arr_eq(out, bytes, sizeof(bytes));

    fixture_interp->stack_v[0] = PTR_TO_ENKI(&p);
    fixture_interp->sp = 1;
    cr_assert_eq(catch_op(op82_input), ENKI_ERROR_TYPE);
}

Test(op82_runtime, op82_now_returns_nonzero_timeish_nat)
{
    fixture_interp->stack_v[0] = 123;
    fixture_interp->sp = 1;
    op82_now(fixture_interp);
    cr_assert_gt(fixture_interp->stack_v[0], 0);
}

Test(op82_runtime, op82_read_file_and_stamp_return_file_contents_and_mtime)
{
    char path[] = "/tmp/enki-op82-file-XXXXXX";
    int fd = mkstemp(path);
    cr_assert_geq(fd, 0);
    uint8_t bytes[] = {'f', 'i', 'l', 'e'};
    cr_assert_eq(write(fd, bytes, sizeof(bytes)), (ssize_t)sizeof(bytes));
    close(fd);

    fixture_interp->stack_v[0] = enki_bytes_to_nat(fixture_interp, (uint8_t*)path, strlen(path));
    fixture_interp->sp = 1;
    op82_read_file(fixture_interp);
    size_t len = 0;
    uint8_t* out = enki_nat_to_bytes(fixture_interp, fixture_interp->stack_v[0], &len);
    cr_assert_eq(len, sizeof(bytes));
    cr_assert_arr_eq(out, bytes, sizeof(bytes));

    fixture_interp->stack_v[0] = enki_bytes_to_nat(fixture_interp, (uint8_t*)path, strlen(path));
    fixture_interp->sp = 1;
    op82_stamp(fixture_interp);
    cr_assert_gt(fixture_interp->stack_v[0], 0);

    (void)unlink(path);

    uint8_t missing[] = "/tmp/enki-op82-missing";
    fixture_interp->stack_v[0] = enki_bytes_to_nat(fixture_interp, missing, sizeof(missing) - 1);
    fixture_interp->sp = 1;
    op82_read_file(fixture_interp);
    cr_assert_eq(fixture_interp->stack_v[0], 0);

    fixture_interp->stack_v[0] = enki_bytes_to_nat(fixture_interp, missing, sizeof(missing) - 1);
    fixture_interp->sp = 1;
    op82_stamp(fixture_interp);
    cr_assert_eq(fixture_interp->stack_v[0], 0);
}

Test(op82_runtime, op82_listen_and_accept_register_socket_fds)
{
    reset_fd_table();
    fixture_interp->stack_v[0] = 0;
    fixture_interp->sp = 1;
    op82_listen(fixture_interp);
    size_t listen_h = (size_t)fixture_interp->stack_v[0];
    int listen_fd = enki_fd_get(listen_h);
    cr_assert_geq(listen_fd, 0);
    cr_assert_eq(enki_fd_close(listen_h), 0);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    cr_assert_geq(listen_fd, 0);
    int yes = 1;
    cr_assert_eq(setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)), 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    cr_assert_eq(bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)), 0);
    cr_assert_eq(listen(listen_fd, 1), 0);
    socklen_t addr_len = sizeof(addr);
    cr_assert_eq(getsockname(listen_fd, (struct sockaddr*)&addr, &addr_len), 0);
    listen_h = enki_fd_add(listen_fd);
    cr_assert_neq(listen_h, ENKI_FD_INVALID);

    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    cr_assert_geq(client_fd, 0);
    cr_assert_eq(connect(client_fd, (struct sockaddr*)&addr, sizeof(addr)), 0);

    fixture_interp->stack_v[0] = (enki_value)listen_h;
    fixture_interp->sp = 1;
    op82_accept(fixture_interp);
    size_t accepted_h = (size_t)fixture_interp->stack_v[0];
    cr_assert_geq(enki_fd_get(accepted_h), 0);

    cr_assert_eq(enki_fd_close(accepted_h), 0);
    cr_assert_eq(enki_fd_close(listen_h), 0);
    close(client_fd);
}

Test(op82_runtime, scheduler_enqueue_edges_and_run_drains_done_cases)
{
    enki_schedueler s = {0};
    enki_actor a;
    enki_actor b;
    init_actor(&a);
    init_actor(&b);

    enki_schedueler_enqueue(NULL, &a);
    enki_schedueler_enqueue(&s, NULL);
    cr_assert_null(s.head);

    b.state = ENKI_ACTOR_BLOCKED;
    enki_schedueler_enqueue(&s, &b);
    cr_assert_null(s.head);

    enki_schedueler_enqueue(&s, &a);
    enki_schedueler_enqueue(&s, &a);
    cr_assert_eq(s.head, &a);
    cr_assert_eq(s.tail, &a);
    cr_assert_null(a.next);

    b.state = ENKI_ACTOR_RUNNABLE;
    enki_schedueler_enqueue(&s, &b);
    cr_assert_eq(s.head, &a);
    cr_assert_eq(s.tail, &b);
    cr_assert_eq(a.next, &b);

    a.i = NULL;
    b.i = fixture_interp;
    fixture_interp->halted = true;
    enki_schedueler_run(&s);
    cr_assert_null(s.head);
    cr_assert_null(s.tail);
    cr_assert_eq(a.state, ENKI_ACTOR_DONE);
    cr_assert_eq(b.state, ENKI_ACTOR_DONE);
    cr_assert(!a.queued);
    cr_assert(!b.queued);
}

Test(op82_runtime, send_rejects_missing_runtime_and_bad_handles)
{
    enki_actor sender;
    enki_schedueler s = {0};
    init_actor(&sender);

    fixture_interp->actor = NULL;
    fixture_interp->scheduler = &s;
    fixture_interp->stack_v[0] = 1;
    fixture_interp->stack_v[1] = 42;
    fixture_interp->sp = 2;
    cr_assert_eq(catch_op(op82_send), ENKI_ERROR_TYPE);

    fixture_interp->actor = &sender;
    fixture_interp->scheduler = NULL;
    fixture_interp->stack_v[0] = 1;
    fixture_interp->stack_v[1] = 42;
    fixture_interp->sp = 2;
    cr_assert_eq(catch_op(op82_send), ENKI_ERROR_TYPE);

    fixture_interp->scheduler = &s;
    fixture_interp->stack_v[0] = PTR_TO_ENKI(&sender);
    fixture_interp->stack_v[1] = 42;
    fixture_interp->sp = 2;
    cr_assert_eq(catch_op(op82_send), ENKI_ERROR_TYPE);

    fixture_interp->stack_v[0] = HANDLES_MAX;
    fixture_interp->stack_v[1] = 42;
    fixture_interp->sp = 2;
    cr_assert_eq(catch_op(op82_send), ENKI_ERROR_BOUNDS);

    fixture_interp->stack_v[0] = 1;
    fixture_interp->stack_v[1] = 42;
    fixture_interp->sp = 2;
    cr_assert_eq(catch_op(op82_send), ENKI_ERROR_BOUNDS);
}

Test(op82_runtime, send_enqueues_payload_and_wakes_blocked_actor)
{
    enki_actor sender;
    enki_actor target;
    enki_schedueler s = {0};
    init_actor(&sender);
    init_actor(&target);
    target.state = ENKI_ACTOR_BLOCKED;
    sender.handles[1] = &target;
    sender.next_handle = 2;
    fixture_interp->actor = &sender;
    fixture_interp->scheduler = &s;
    fixture_interp->stack_v[0] = 1;
    fixture_interp->stack_v[1] = 42;
    fixture_interp->sp = 2;

    op82_send(fixture_interp);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 0);
    cr_assert_eq(target.inbox.inbound, 1);
    cr_assert_eq(target.state, ENKI_ACTOR_RUNNABLE);
    cr_assert_eq(s.head, &target);
    enki_message msg = enki_mailbox_pop(&target.inbox);
    cr_assert_eq(msg.payload, 42);
    cr_assert_eq(msg.n_caps, 0);
}

Test(op82_runtime, send_caps_rejects_bad_caps_rows_and_bad_cap_handles)
{
    enki_actor sender;
    enki_actor target;
    enki_schedueler s = {0};
    init_actor(&sender);
    init_actor(&target);
    sender.handles[1] = &target;
    sender.next_handle = 2;
    fixture_interp->actor = &sender;
    fixture_interp->scheduler = &s;

    fixture_interp->stack_v[0] = 1;
    fixture_interp->stack_v[1] = 88;
    fixture_interp->stack_v[2] = 0;
    fixture_interp->sp = 3;
    cr_assert_eq(catch_op(op82_send_caps), ENKI_ERROR_TYPE);

    enki_value bad_cap_args[1] = {PTR_TO_ENKI(&sender)};
    enki_value bad_caps = enki_alloc_row(fixture_interp->gc, 0, 1, bad_cap_args);
    fixture_interp->stack_v[0] = 1;
    fixture_interp->stack_v[1] = 88;
    fixture_interp->stack_v[2] = bad_caps;
    fixture_interp->sp = 3;
    cr_assert_eq(catch_op(op82_send_caps), ENKI_ERROR_TYPE);

    enki_value null_cap_args[1] = {2};
    enki_value null_caps = enki_alloc_row(fixture_interp->gc, 0, 1, null_cap_args);
    fixture_interp->stack_v[0] = 1;
    fixture_interp->stack_v[1] = 88;
    fixture_interp->stack_v[2] = null_caps;
    fixture_interp->sp = 3;
    cr_assert_eq(catch_op(op82_send_caps), ENKI_ERROR_BOUNDS);
}

Test(op82_runtime, send_caps_resolves_capabilities_from_sender_table)
{
    enki_actor sender;
    enki_actor target;
    enki_actor cap;
    enki_schedueler s = {0};
    init_actor(&sender);
    init_actor(&target);
    init_actor(&cap);
    sender.handles[1] = &target;
    sender.handles[2] = &cap;
    sender.next_handle = 3;
    fixture_interp->actor = &sender;
    fixture_interp->scheduler = &s;

    enki_value cap_args[2] = {0, 2};
    enki_value caps = enki_alloc_row(fixture_interp->gc, 0, 2, cap_args);
    fixture_interp->stack_v[0] = 1;
    fixture_interp->stack_v[1] = 88;
    fixture_interp->stack_v[2] = caps;
    fixture_interp->sp = 3;

    op82_send_caps(fixture_interp);

    cr_assert_eq(fixture_interp->sp, 1);
    cr_assert_eq(fixture_interp->stack_v[0], 0);
    cr_assert_eq(target.inbox.inbound, 1);
    enki_message msg = enki_mailbox_pop(&target.inbox);
    cr_assert_eq(msg.payload, 88);
    cr_assert_eq(msg.n_caps, 2);
    cr_assert_eq(msg.caps[0], &sender);
    cr_assert_eq(msg.caps[1], &cap);
}

Test(op82_runtime, recv_empty_rejects_non_direct_op82_rewind)
{
    enki_actor actor;
    init_actor(&actor);
    fixture_interp->actor = &actor;
    uint8_t bc[] = {OP_OP66};
    fixture_interp->bc_b = bc;
    fixture_interp->pc = 1;
    fixture_interp->sp = 0;

    cr_assert_eq(catch_op(op82_rcv), ENKI_ERROR_BAD_TAG);
    cr_assert_eq(actor.state, ENKI_ACTOR_RUNNABLE);
    cr_assert_eq(fixture_interp->pc, 1);
}

Test(op82_runtime, recv_blocks_and_rewinds_direct_op82)
{
    enki_actor actor;
    init_actor(&actor);
    fixture_interp->actor = &actor;
    uint8_t bc[] = {OP_OP82, OP82_RCV};
    fixture_interp->bc_b = bc;
    fixture_interp->pc = 2;
    fixture_interp->sp = 0;

    op82_rcv(fixture_interp);

    cr_assert_eq(actor.state, ENKI_ACTOR_BLOCKED);
    cr_assert_eq(fixture_interp->pc, 0);
    cr_assert_eq(fixture_interp->sp, 0);
}

Test(op82_runtime, recv_returns_payload_and_fresh_local_cap_handles)
{
    enki_actor receiver;
    enki_actor cap;
    init_actor(&receiver);
    init_actor(&cap);
    fixture_interp->actor = &receiver;
    enki_message msg = {.payload = 77, .n_caps = 1, .caps = {&cap}};
    enki_mailbox_push(&receiver.inbox, &msg);

    op82_rcv(fixture_interp);

    cr_assert_eq(receiver.inbox.inbound, 0);
    cr_assert_eq(receiver.handles[1], &cap);
    cr_assert_eq(receiver.next_handle, 2);
    cr_assert_eq(fixture_interp->sp, 1);
    enki_app* result = ENKI_AS(enki_app, fixture_interp->stack_v[0]);
    cr_assert_eq(result->n_args_s, 2);
    cr_assert_eq(result->args_v[0], 77);
    enki_app* caps = ENKI_AS(enki_app, result->args_v[1]);
    cr_assert_eq(caps->n_args_s, 1);
    cr_assert_eq(caps->args_v[0], 1);
}

Test(op82_runtime, close_handle_rejects_reserved_closed_and_bad_types)
{
    enki_actor actor;
    enki_actor target;
    init_actor(&actor);
    init_actor(&target);
    actor.handles[1] = &target;
    actor.next_handle = 2;
    fixture_interp->actor = &actor;

    fixture_interp->stack_v[0] = 1;
    fixture_interp->sp = 1;
    cr_assert_eq(catch_op(op82_close_handle), ENKI_ERROR_OK);
    cr_assert_null(actor.handles[1]);
    cr_assert_eq(fixture_interp->stack_v[0], 0);

    fixture_interp->stack_v[0] = 0;
    fixture_interp->sp = 1;
    cr_assert_eq(catch_op(op82_close_handle), ENKI_ERROR_IO);

    fixture_interp->stack_v[0] = 1;
    fixture_interp->sp = 1;
    cr_assert_eq(catch_op(op82_close_handle), ENKI_ERROR_BOUNDS);

    fixture_interp->stack_v[0] = PTR_TO_ENKI(&target);
    fixture_interp->sp = 1;
    cr_assert_eq(catch_op(op82_close_handle), ENKI_ERROR_TYPE);
}

Test(op82_runtime, spawn_requires_actor_scheduler_and_handle_capacity)
{
    enki_actor actor;
    enki_schedueler s = {0};

    fixture_interp->actor = NULL;
    fixture_interp->scheduler = &s;
    fixture_interp->stack_v[0] = 0;
    fixture_interp->sp = 1;
    cr_assert_eq(catch_op(op82_spawn), ENKI_ERROR_TYPE);

    init_actor(&actor);
    fixture_interp->actor = &actor;
    fixture_interp->scheduler = NULL;
    fixture_interp->stack_v[0] = 0;
    fixture_interp->sp = 1;
    cr_assert_eq(catch_op(op82_spawn), ENKI_ERROR_TYPE);

    fixture_interp->scheduler = &s;
    actor.next_handle = HANDLES_MAX;
    fixture_interp->stack_v[0] = 0;
    fixture_interp->sp = 1;
    cr_assert_eq(catch_op(op82_spawn), ENKI_ERROR_OOM);
}
