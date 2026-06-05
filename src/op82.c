#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include "enki/app.h"
#include "enki/error.h"
#include "enki/fd.h"
#include "enki/gc.h"
#include "enki/interp.h"
#include "enki/mailbox.h"
#include "enki/nat.h"
#include "enki/op82.h"
#include "enki/scheduler.h"
#include "enki/value.h"

#define CHILD_HEAP_SIZE ((size_t)1024 * 1024)
#define CHILD_STORE_SIZE ((size_t)1024 * 1024)
#define CHILD_SCRATCH_SIZE ((size_t)1024 * 1024)

static int write_all(int fd, uint8_t* bytes, size_t len) {
    size_t remaining = len;
    size_t off = 0;
    while(remaining > 0) {
        ssize_t r = write(fd, bytes + off, remaining);
        if(r < 0) {
            if(errno == EINTR) continue;
            return -1;
        }
        if(r == 0) {
            errno = EIO;
            return -1;
        }
        remaining -= (size_t)r;
        off += (size_t)r;
    }
    return 0;
}

static int read_all(int fd, uint8_t* buf, size_t len) {
  size_t remaining = len;
  uint8_t* p = buf;
  while(remaining > 0) {
    ssize_t r = read(fd, p, remaining);
    if(r < 0) {
      if(errno == EINTR) continue;
      return -1;
    }
    if(r == 0) break;
    remaining -= (size_t)r;
    p += (size_t)r;
  }
  return 0;
}

void op82_write(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    enki_value hdl = i->stack_v[i->sp - 2];
    if(IS_PTR(hdl)) enki_interp_throw(i, ENKI_ERROR_TYPE, hdl);
    size_t len = 0;
    uint8_t* bytes = enki_nat_to_bytes(i, x, &len);
    int fd = enki_fd_get((size_t)hdl);
    if(fd < 0) enki_interp_throw(i, ENKI_ERROR_IO, hdl);
    int rc = write_all(fd, bytes, len);
    if(rc < 0) enki_interp_throw(i, ENKI_ERROR_IO, x);
    i->sp--;
    i->stack_v[i->sp - 1] = 0;
}

void op82_closefd(enki_interpreter* i) {
    enki_value hdl = i->stack_v[i->sp - 1];
    if(IS_PTR(hdl)) enki_interp_throw(i, ENKI_ERROR_TYPE, hdl);
    int rc = enki_fd_close((size_t)hdl);
    if(rc < 0) enki_interp_throw(i, ENKI_ERROR_IO, hdl);
    i->stack_v[i->sp - 1] = 0;
}

void op82_read(enki_interpreter* i) {
    enki_value hdl = i->stack_v[i->sp - 2];
    enki_value n = i->stack_v[i->sp - 1];
    if(IS_PTR(hdl)) enki_interp_throw(i, ENKI_ERROR_TYPE, hdl);
    if(IS_PTR(n)) enki_interp_throw(i, ENKI_ERROR_TYPE, n);
    n = (size_t)n;
    int fd = enki_fd_get((size_t)hdl);
    if(fd < 0) enki_interp_throw(i, ENKI_ERROR_IO, hdl);
    uint8_t* out = enki_arena_alloc(i->scratch_a, n);
    if(out == NULL) enki_interp_throw(i, ENKI_ERROR_OOM, hdl);
    ssize_t len = read(fd, out, n);
    if(len < 0) enki_interp_throw(i, ENKI_ERROR_IO, hdl);
    enki_value res = enki_bytes_to_nat(i, out, (size_t)len);
    i->sp--;
    i->stack_v[i->sp - 1] = res;
}

void op82_output(enki_interpreter* i) {
  enki_value x = i->stack_v[i->sp - 1];
  size_t len;
  uint8_t* bytes = enki_nat_to_bytes(i, x, &len);
  if(write_all(STDOUT_FILENO, bytes, len) < 0) {
    enki_interp_throw(i, ENKI_ERROR_IO, x);
  }
  i->stack_v[i->sp - 1] = 0;
}

void op82_warn(enki_interpreter* i) {
  enki_value x = i->stack_v[i->sp - 1];
  size_t len;
  uint8_t* bytes = enki_nat_to_bytes(i, x, &len);
  if(write_all(STDERR_FILENO, bytes, len) < 0) {
    enki_interp_throw(i, ENKI_ERROR_IO, x);
  }
  i->stack_v[i->sp - 1] = 0;
}

void op82_input(enki_interpreter* i) {
  enki_value n = i->stack_v[i->sp - 1];
  if(IS_PTR(n)) enki_interp_throw(i, ENKI_ERROR_TYPE, n);
  n = (size_t)n;
  uint8_t* out = enki_arena_alloc(i->scratch_a, n);
  if(out == NULL) enki_interp_throw(i, ENKI_ERROR_OOM, 0);
  ssize_t len = read(STDIN_FILENO, out, n);
  if(len < 0) enki_interp_throw(i, ENKI_ERROR_IO, 0);
  enki_value res = enki_bytes_to_nat(i, out, (size_t)len);
  i->stack_v[i->sp - 1] = res;
}

void op82_now(enki_interpreter* i) {
  (void)i->stack_v[i->sp - 1];
  time_t now = time(NULL);
  if(now == (time_t)-1) enki_interp_throw(i, ENKI_ERROR_IO, 0);
  i->stack_v[i->sp - 1] = (enki_value)now;
}

void op82_read_file(enki_interpreter* i) {
  enki_value p = i->stack_v[i->sp - 1];
  size_t path_len = 0;
  uint8_t* path_bytes = enki_nat_to_bytes(i, p, &path_len);
  char* path = enki_arena_alloc(i->scratch_a, path_len + 1);
  if(path == NULL) enki_interp_throw(i, ENKI_ERROR_IO, p);
  memcpy(path, path_bytes, path_len);
  path[path_len] = '\0';
  int fd = open(path, O_RDONLY);
  if(fd < 0) {
    i->stack_v[i->sp - 1] = 0;
    return;
  }
  struct stat st;
  if(fstat(fd, &st) < 0 || st.st_size < 0) {
    close(fd);
    i->stack_v[i->sp - 1] = 0;
    return;
  }
  size_t size = (size_t)st.st_size;
  uint8_t* buf = enki_arena_alloc(i->scratch_a, size);
  if(size > 0 && buf == 0) {
    close(fd);
    enki_interp_throw(i, ENKI_ERROR_OOM, p);
  }
  if(read_all(fd, buf, size) < 0) {
    close(fd);
    i->stack_v[i->sp - 1] = 0;
    return;
  }
  close(fd);
  i->stack_v[i->sp - 1] = enki_bytes_to_nat(i, buf, size);

}

void op82_stamp(enki_interpreter* i) {
  enki_value p = i->stack_v[i->sp - 1];
  size_t path_len = 0;
  uint8_t* path_bytes = enki_nat_to_bytes(i, p, &path_len);
  char* path = enki_arena_alloc(i->scratch_a, path_len + 1);
  if(path == NULL) enki_interp_throw(i, ENKI_ERROR_IO, p);
  memcpy(path, path_bytes, path_len);
  path[path_len] = '\0';
  struct stat st;
  if(stat(path, &st) < 0) {
    i->stack_v[i->sp - 1] = 0;
    return;
  }
  i->stack_v[i->sp - 1] = (enki_value)st.st_mtime;
}

void op82_listen(enki_interpreter* i) {
  enki_value p = i->stack_v[i->sp - 1];
  if(IS_PTR(p)) enki_interp_throw(i, ENKI_ERROR_TYPE, p);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if(fd < 0) {
    enki_interp_throw(i, ENKI_ERROR_IO, p);
  }
  int yes = 1;
  if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
    close(fd);
    enki_interp_throw(i, ENKI_ERROR_IO, p);
  }
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port  = htons((uint16_t)p);
  if(bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(fd);
    enki_interp_throw(i, ENKI_ERROR_IO, p);
  }
  if(listen(fd, 128) < 0) {
    close(fd);
    enki_interp_throw(i, ENKI_ERROR_IO, p);
  }
  size_t h = enki_fd_add(fd);
  if(h == ENKI_FD_INVALID) {
    close(fd);
    enki_interp_throw(i, ENKI_ERROR_IO, p);
  }
  i->stack_v[i->sp - 1] = (enki_value)h;
}

void op82_accept(enki_interpreter* i) {
  enki_value hdl = i->stack_v[i->sp - 1];
  if(IS_PTR(hdl)) enki_interp_throw(i, ENKI_ERROR_TYPE, hdl);
  int listen_fd = enki_fd_get((size_t)hdl);
  if(listen_fd < 0) {
    enki_interp_throw(i, ENKI_ERROR_IO, hdl);
  }
  int client_fd = accept(listen_fd, NULL, NULL);
  if(client_fd < 0) {
    enki_interp_throw(i, ENKI_ERROR_IO, hdl);
  }
  size_t h = enki_fd_add(client_fd);
  if(h == ENKI_FD_INVALID) {
    close(client_fd);
    enki_interp_throw(i, ENKI_ERROR_IO, hdl);
  }
  i->stack_v[i->sp - 1] = (enki_value)h;
}

void op82_spawn(enki_interpreter* i) {
  enki_value fn = i->stack_v[i->sp - 1];
  enki_actor* curr = i->actor;
  if(curr == NULL) enki_interp_throw(i, ENKI_ERROR_TYPE, 0);
  if(i->scheduler == NULL) enki_interp_throw(i, ENKI_ERROR_TYPE, 0);
  if(curr->next_handle >= HANDLES_MAX) enki_interp_throw(i, ENKI_ERROR_OOM, 0);
  enki_actor* child = i->our_a.alloc(i->our_a.ctx, sizeof(*child));
  if(child == NULL) enki_interp_throw(i, ENKI_ERROR_OOM, 0);
  memset(child, 0, sizeof(*child));
  enki_interpreter* child_i = enki_interp_create(
    &i->our_a,
    CHILD_HEAP_SIZE,
    "./snap",
    CHILD_STORE_SIZE,
    CHILD_SCRATCH_SIZE
  );
  child_i->actor = child;
  child_i->scheduler = i->scheduler;
  fn = enki_gc_import(child_i, fn);
  child->i = child_i;
  enki_mailbox_init(child_i, &child->inbox);
  child->handles[0] = child;
  child->queued = false;
  child->next = NULL;
  child->next_handle = 1;
  child->state = ENKI_ACTOR_RUNNABLE;
  enki_value arg0 = 0;
  enki_interp_enter_call(child_i, fn, 1, &arg0);
  size_t hdl = curr->next_handle++;
  curr->handles[hdl] = child;
  enki_schedueler_enqueue(i->scheduler, child);
  i->stack_v[i->sp - 1] = (enki_value)hdl;
}

void op82_send(enki_interpreter* i) {
  enki_value hdl = i->stack_v[i->sp - 2];
  enki_value pay = i->stack_v[i->sp - 1];
  enki_actor* curr = i->actor;
  if(curr == NULL) enki_interp_throw(i, ENKI_ERROR_TYPE, 0);
  if(i->scheduler == NULL) enki_interp_throw(i, ENKI_ERROR_TYPE, 0);
  if(IS_PTR(hdl)) enki_interp_throw(i, ENKI_ERROR_TYPE, hdl);
  if(hdl >= HANDLES_MAX) enki_interp_throw(i, ENKI_ERROR_BOUNDS, hdl);
  enki_actor* target = curr->handles[(size_t)hdl];
  if(target == NULL) enki_interp_throw(i, ENKI_ERROR_BOUNDS, hdl);
  pay = enki_gc_import(target->i, pay);
  enki_message msg = {
    .payload = pay, 
    .caps = {0}, 
    .n_caps = 0,
    .next = NULL,
  };
  enki_mailbox_push(&target->inbox, &msg);
  if(target->state == ENKI_ACTOR_BLOCKED) {
    target->state = ENKI_ACTOR_RUNNABLE;
    enki_schedueler_enqueue(i->scheduler, target);
  }
  i->sp--;
  i->stack_v[i->sp - 1] = 0;
}

void op82_send_caps(enki_interpreter* i) {
  enki_value hdl = i->stack_v[i->sp - 3];
  enki_value pay = i->stack_v[i->sp - 2];
  enki_value caps = i->stack_v[i->sp - 1];
  enki_actor* curr = i->actor;
  if(curr == NULL) enki_interp_throw(i, ENKI_ERROR_TYPE, 0);
  if(i->scheduler == NULL) enki_interp_throw(i, ENKI_ERROR_TYPE, 0);
  if(IS_PTR(hdl)) enki_interp_throw(i, ENKI_ERROR_TYPE, hdl);
  if(!IS_PTR(caps)) enki_interp_throw(i, ENKI_ERROR_TYPE, caps);
  enki_value_header* h = ENKI_AS(enki_value_header, caps);
  if(h->kind_b != APP) enki_interp_throw(i, ENKI_ERROR_TYPE, caps);
  enki_app* caps_row = ENKI_AS(enki_app, caps);
  if(hdl >= HANDLES_MAX) enki_interp_throw(i, ENKI_ERROR_BOUNDS, hdl);
  enki_actor* target = curr->handles[(size_t)hdl];
  if(target == NULL) enki_interp_throw(i, ENKI_ERROR_BOUNDS, hdl);
  pay = enki_gc_import(target->i, pay);
  size_t n_caps = caps_row->n_args_s;
  if(n_caps > CAPS_MAX) enki_interp_throw(i, ENKI_ERROR_BOUNDS, caps);
  enki_message msg = {
    .payload = pay, 
    .caps = {0}, 
    .n_caps = n_caps,
    .next = NULL,
  };
  for(size_t k = 0; k < n_caps; k++) {
    enki_value cap_hdl = caps_row->args_v[k];
    if(IS_PTR(cap_hdl)) enki_interp_throw(i, ENKI_ERROR_TYPE, cap_hdl);
    if(cap_hdl >= HANDLES_MAX) enki_interp_throw(i, ENKI_ERROR_BOUNDS, cap_hdl);
    enki_actor* cap_actor = curr->handles[(size_t)cap_hdl];
    if(cap_actor == NULL) enki_interp_throw(i, ENKI_ERROR_BOUNDS, cap_hdl);
    msg.caps[k] = cap_actor;
  }
  enki_mailbox_push(&target->inbox, &msg);
  if(target->state == ENKI_ACTOR_BLOCKED) {
    target->state = ENKI_ACTOR_RUNNABLE;
    enki_schedueler_enqueue(i->scheduler, target);
  }
  i->sp -= 2;
  i->stack_v[i->sp - 1] = 0;
}
void op82_rcv(enki_interpreter* i) {
  enki_actor* curr = i->actor;
  if(curr == NULL) enki_interp_throw(i, ENKI_ERROR_TYPE, 0);
  if(curr->inbox.inbound == 0){
    if(i->pc < 2 || i->bc_b[i->pc - 2] != OP_OP82) {
      enki_interp_throw(i, ENKI_ERROR_BAD_TAG, 0);
    }
    i->pc -= 2;
    curr->state = ENKI_ACTOR_BLOCKED;
    return;
  }
  enki_message msg = enki_mailbox_pop(&curr->inbox);
  size_t n_caps = msg.n_caps;
  if(n_caps > CAPS_MAX) enki_interp_throw(i, ENKI_ERROR_BOUNDS, n_caps);
  enki_value cap_handles[CAPS_MAX];
  for(size_t k = 0; k < n_caps; k++) {
    size_t cap_hdl = curr->next_handle;
    if(cap_hdl >= HANDLES_MAX) enki_interp_throw(i, ENKI_ERROR_BOUNDS, cap_hdl);
    curr->handles[cap_hdl] = msg.caps[k];
    cap_handles[k] = (enki_value)cap_hdl;
    curr->next_handle++;
  }
  enki_value caps_row = enki_alloc_row(i->gc, 0, n_caps, cap_handles);
  enki_value result_args[2] = { msg.payload, caps_row };
  enki_value result = enki_alloc_row(i->gc, 0, 2, result_args);
  i->stack_v[i->sp++] = result;
}
void op82_close_handle(enki_interpreter* i) {
  enki_actor* curr = i->actor;
  if(curr == NULL) enki_interp_throw(i, ENKI_ERROR_TYPE, 0);
  enki_value hdl = i->stack_v[i->sp - 1];
  if(IS_PTR(hdl)) enki_interp_throw(i, ENKI_ERROR_TYPE, hdl);
  if(hdl == (enki_value)0) enki_interp_throw(i, ENKI_ERROR_IO, hdl);
  if(hdl >= HANDLES_MAX) enki_interp_throw(i, ENKI_ERROR_BOUNDS, hdl);
  if(curr->handles[(size_t)hdl] == NULL) enki_interp_throw(i, ENKI_ERROR_BOUNDS, hdl);
  curr->handles[(size_t)hdl] = NULL;
  i->stack_v[i->sp - 1] = (enki_value)0;
}
