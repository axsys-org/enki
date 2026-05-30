#define FD_MAX 1024 
#define ENKI_FD_INVALID FD_MAX

typedef struct {
  bool live;
  int fd;
} fd_slot;

typedef struct {
  size_t next;
  fd_slot slots[FD_MAX];
} fd_table;

fd_table* t;

t->slots[0] = (fd_slot){.live = true, .fd = 0};
t->slots[1] = (fd_slot){.live = true, .fd = 1};
t->slots[2] = (fd_slot){.live = true, .fd = 2};
t->next = 3;

size_t enki_fd_add(int fd) {
    if(t->next < FD_MAX) {
        size_t hdl = t->next++;
        t->slots[hdl] = (fd_slot){.live = true, .fd = fd}; 
        return hdl;
    }
    for(size_t k = 0; k < FD_MAX; k++) {  
        fd_slot* slot = &t->slots[k];
        if(!slot->live) { 
            slot->fd = fd;
            slot->live = true;
            return k;
        }
    }
    return ENKI_FD_INVALID;
}

int enki_fd_get(size_t hdl) {
    if(hdl >= FD_MAX) return -1;
    fd_slot* slot = &t->slots[hdl];
    if(!slot->live) return -1;
    return slot->fd;
}

int enki_fd_close(size_t hdl) {
    if(hdl >= FD_MAX) return -1;
    fd_slot* slot = &t->slots[hdl];
    if(!slot->live) return -1;
    int rc = close(slot->fd);
    slot->fd = -1;
    slot->live = false;
    return rc;
}

int write_all(int fd, uint8_t* bytes, size_t len) {
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

int read_all(int fd, uint8_t* buf, size_t len) {
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
    if(out == NULL || n < 0) enki_interp_throw(i, ENKI_ERROR_OOM, hdl);
    ssize_t len = read(fd, out, n);
    if(len < 0) enki_interp_throw(i, ENKI_ERROR_IO, hdl);
    enki_value res = enki_bytes_to_nat(i, out, (size_t)len);
    i->sp--;
    i->stack_v[i->sp - 2] = res;
}

void op82_output(enki_interpreter* i) {
  enki_value x = i->stack_v[i->sp - 1];
  size_t len;
  uint8_t* bytes = enki_nat_to_bytes(i, x, &len);
  if(write_all(STDOUT_FILENO, bytes, len) < 0) {
    enki_interp_throw(i, ENKI_ERROR_IO, hdl);
  }
  i->stack_v[i->sp - 1] = 0;
}

void op82_warn(enki_interpreter* i) {
  enki_value x = i->stack_v[i->sp - 1];
  size_t len;
  uint8_t* bytes = enki_nat_to_bytes(i, x, &len);
  if(write_all(STDERR_FILENO, bytes, len) < 0) {
    enki_interp_throw(i, ENKI_ERROR_IO, hdl);
  }
  i->stack_v[i->sp - 1] = 0;
}

void op82_input(enki_interpreter* i) {
  enki_value n = i->stack_v[i->sp - 1];
  if(IS_PTR(n)) enki_interp_throw(i, ENKI_ERROR_TYPE, n);
  n = (size_t)n;
  uint8_t* out = enki_arena_alloc(i->scratch_a, n);
  if(out == NULL || n < 0) enki_interp_throw(i, ENKI_ERROR_OOM, 0);
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
  char* path = enki_arena_alloc(i->scratch_a, path_len);
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
  char* path = enki_arena_alloc(i->scratch_a, path_len);
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
  size_t listen_fd = enki_fd_get((size_t)hdl);
  if(listen_fd < 0) {
    enki_interp_throw(i, ENKI_ERROR_IO, hdl);
  }
  int client_fd = accept(listen_fd, NULL, NULL);
  if(client_fd < 0) {
    enki_interp_throw(i, ENKI_ERROR_IO, hdl);
  }
  size_t h = enki_fd_add(fd);
  if(h == ENKI_FD_INVALID) {
    close(client_fd);
    enki_interp_throw(i, ENKI_ERROR_IO, hdl);
  }
  i->stack_v[i->sp - 1] = (enki_value)h;
}
typedef struct {
  enki_value payload;
  enki_actor* caps[CAPS_MAX];
  size_t n_caps;
} enki_msg;

typedef enum {
  ENKI_ACTOR_RUNNABLE,
  ENKI_ACTOR_BLOCKED,
  ENKI_ACTOR_DONE,
} actor_state;

typedef struct enki_actor {
  enki_interpreter* i;
  ms_queue inbox;
  size_t next_handle;
  struct enki_actor* handles[HANDLES_MAX];
  actor_state state;
} enki_actor;

void op82_spawn(enki_interpreter* i) {
  enki_value fn = i->stack_v[i->sp - 1];
  enki_actor* curr = i->actor;
  if(curr->next_handle >= HANDLES_MAX) {
    enki_interp_throw(i, ENKI_ERROR_OOM, 0);
  } 
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
  child->i = child_i;
  enki_init_mailbox(&child->inbox);
  child->handles[0] = child;
  child->next_handle = 1;
  child->state = ENKI_ACTOR_RUNNABLE;
  enki_value arg0 = 0;
  enki_interp_enter_call(child_i, fn, 1, &arg0);

  size_t hdl = curr->next_handle++;
  curr->handles[hdl] = child;

  enki_scheduler_enqueue(i->scheduler, child);

  i->stack_v[i->sp - 1] = (enki_value)hdl;
}

void op82_send(enki_interpreter* i) {
  enki_value hdl = i->stack_v[i->sp - 2];
  enki_value msg = i->stack_v[i->sp - 1];
  enki_actor* curr = i->actor;
  if(IS_PTR(hdl)) enki_interp_throw(i, ENKI_ERROR_TYPE, hdl);
  if(hdl >= HANDLES_MAX) enki_interp_throw(i, ENKI_ERROR_BOUNDS, hdl);
  enki_actor* target = curr->handles[(size_t)hdl];
  enki_msg_push(target->inbox, msg);
  if(target->state == ENKI_ACTOR_BLOCKED) {
    taget->state = ENKI_ACTOR_RUNNABLE;
    enki_scheduler_enqueue(i->scheduler, target);
  }
  i->sp--;
  i->stack_v[i->sp - 2] = 0;
}

void op82_send_caps() {
Resolve h in sender table to target actor.
Interpret caps_row as a row of numeric handles.
For each numeric handle, look it up in sender table to get enki_actor*.
Put those actor pointers into the outgoing message.
Push message into target inbox.
Return 0.
}
void op82_rcv() {
Block on current actor’s inbox until a message arrives.
Pop { msg, caps }.
For each received enki_actor*, allocate a fresh local handle in the receiver’s table.
Return:

}

Actor =
  mutex
  condition_variable
  inbox_queue

Message =
  payload
  caps

RuntimeState =
  self
  handles
  next_handle


newRuntime(self):
  handles[0] = self
  next_handle = 1

allocHandle(actor):
  h = next_handle
  next_handle += 1
  handles[h] = actor
  return h


child_main(child_interp, fn):
  push fn
  push 0

  apply 1 argument

  run interpreter until done


Spawn(fn):
  child = new Actor
    pthread_mutex_init(child.mutex)
    pthread_cond_init(child.condition_variable)
    child.inbox_queue = empty

    child_runtime = newRuntime(child)
    ctx =
  child_interpreter
  child_actor_runtime
  fn_to_run
    pthread_t tid;
    pthread_create(&tid, NULL, child_main, ctx);
    pthread_detach(tid);

  parent_handle = allocHandle(child)
  return parent_handle


  Send(handle, message):
  target = handles[handle]

  lock target.mutex
    push target.inbox_queue:
      payload = message
      caps = []
    signal target.condition_variable
  unlock target.mutex

  return 0


  SendCaps(handle, message, cap_handles):
  target = handles[handle]

  real_caps = []
  for h in cap_handles:
    real_caps.append(handles[h])

  lock target.mutex
    push target.inbox_queue:
      payload = message
      caps = real_caps
    signal target.condition_variable
  unlock target.mutex

  return 0


Recv(0):
  lock self.mutex
    while self.inbox_queue is empty:
      wait self.condition_variable with self.mutex

    message = pop self.inbox_queue
  unlock self.mutex

  new_handles = []
  for actor in message.caps:
    h = allocHandle(actor)
    new_handles.append(h)

  return [message.payload, new_handles]


  CloseHandle(handle):
  remove handles[handle]
  return 0


