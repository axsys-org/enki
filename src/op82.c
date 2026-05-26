
Input  -> STDIN_FILENO
Output -> STDOUT_FILENO
Print  -> STDOUT_FILENO
Warn   -> STDERR_FILENO


int get_fd(enki_value hdl) {


}

op82_write(enki_interpreter* i) {
    enki_value x = i->stack_v[i->sp - 1];
    size_t len;
    uint8_t* bytes = enki_nat_to_bytes(i, x, &len);
    enki_value hdl = i->stack_v[i->sp - 2];
    int fd = get_fd(i, hdl);
    size_t remaining = len;
    size_t off = 0;
    while(remaining > 0) {
        ssize_t r = write(fd, bytes + off, remaining);
        if(r <= 0) {
            if(errno == EINTR) continue;
            x = i->stack_v[i->sp - 1];
            enki_interp_throw(i, ENKI_ERROR_IO, x);
        }
        off += (size_t)r;
        remaining -= (size_t)r;
    }
    i->sp--;
    i->stack_v[i->sp - 1] = 0;
}


write(STDOUT_FILENO, bytes, len);
write(STDERR_FILENO, bytes, len);
time(NULL);
struct stat st;
fstat(fd, &st);

close(fd);

int sock = socket(AF_INET, SOCK_STREAM, 0);
int yes = 1;
setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
struct sockaddr_in addr = {0};
addr.sin_family = AF_INET;
addr.sin_addr.s_addr = htonl(INADDR_ANY);
addr.sin_port = htons(port);
bind(sock, (struct sockaddr*)&addr, sizeof(addr));
listen(sock, 128);

int conn = accept(listen_sock, NULL, NULL);
ssize_t n = write(fd, buf, len);
while (off < len) {
    ssize_t n = write(fd, bytes + off, len - off);
    if (n <= 0) handle_error;
    off += n;
}

Print(nat_string):
  bytes = nat_to_bytes(nat_string)
  write stdout bytes
  flush stdout
  return 0

Warn(bytes_nat):
  bytes = nat_to_bytes(bytes_nat)
  write stderr bytes
  flush stderr
  return 0


ReadFile(path_nat):
  path = nat_to_string(path_nat)
  bytes = read_entire_file("src/" + path)

  if file does not exist:
    return 0

  return bytes_to_bar_nat(bytes)


CloseFd(handle):
  return close_fd_handle(handle)


Listen(port):
  sock = socket_tcp()
  set sock reuseaddr
  bind sock to 0.0.0.0:port
  listen sock backlog 128

  handle = alloc_fd(sock)
  return handle

Accept(listen_handle):
  listen_sock = get_fd(listen_handle)
  conn_sock = accept(listen_sock)

  conn_handle = alloc_fd(conn_sock)
  return conn_handle

Read(fd_handle, n):
  fd = get_fd(fd_handle)
  bytes = read up to n bytes from fd

  if bytes length == 0:
    return 0

  return bytes_to_bar_nat(bytes)

Write(fd_handle, bytes_nat):
  fd = get_fd(fd_handle)
  bytes = nat_to_bytes(bytes_nat)

  write all bytes to fd
  return 0

Output(bytes_nat):
  bytes = nat_to_bytes(bytes_nat)
  write stdout bytes
  flush stdout
  return 0





















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


global fd_table:
  next_fd_handle = 1
  handle -> os_fd

get_fd(h):
  if h not in fd_table:
    throw invalid fd
  return fd_table[h]

  close_fd_handle(h):
  os_fd = get_fd(h)
  close(os_fd)
  remove fd_table[h]
  return 0






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
