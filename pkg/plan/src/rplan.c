#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "axsys/fd.h"
#include "axsys/util.h"
#include "internal.h"
#include "plan/nat.h"

/*
 * op 82: rplan I/O, gated on RPLAN mode (checked in eval.c before
 * dispatch).  Sockets live in a process-global fd handle table; the
 * byte conventions follow the Haskell reference, which the reaver
 * sources rely on:
 *
 *   - Input / ReadFile / Read return "bars": the data bytes followed by
 *     a 0x01 terminator byte (bytesBar), so the consumer can recover
 *     the length (reaver: bytes-size = Dec (Bytes b)).
 *   - Output / Warn / Write take bars and strip the top byte (natBytes).
 *   - Print takes a plain string nat and writes every byte (natStr).
 *
 * Actor ops (Spawn/Send/SendCaps/Recv/CloseHandle) are coordination
 * effects: their bodies below only validate the forced args and rebuild
 * the request spine [OpName, args…]; the machine parks it in
 * t->blocked_on and suspends with PL_RUN_BLOCKED (eval.c op_body).
 * Servicing — mailboxes, handle tables, spawning — belongs to the
 * executor, which deposits the response with pl_thread_deposit.
 */

#define ARG(i) (t->vstack[ab + (i)])

static int rp_write_all(int fd, const uint8_t* bytes, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t r = write(fd, bytes + off, len - off);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (r == 0) {
      errno = EIO;
      return -1;
    }
    off += (size_t)r;
  }
  return 0;
}

static int rp_read_all(int fd, uint8_t* buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t r = read(fd, buf + off, len - off);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (r == 0)
      break;
    off += (size_t)r;
  }
  return 0;
}

/* Materialize a nat's bytes (malloc'd; caller frees).  drop_top drops
 * the highest byte — the reference natBytes, inverse of bytesBar. */
static uint8_t* rp_nat_bytes(pl_val v, bool drop_top, size_t* out_n) {
  size_t n = pl_nat_byte_len(v);
  if (drop_top && n > 0)
    n -= 1;
  uint8_t* b = malloc(n ? n : 1);
  ax_assume(b != NULL, "oom");
  for (size_t i = 0; i < n; i++)
    b[i] = pl_nat_byte_at(v, i);
  *out_n = n;
  return b;
}

/* NUL-terminated path string from a nat (malloc'd). */
static char* rp_nat_path(pl_val v) {
  size_t n = pl_nat_byte_len(v);
  char* p = malloc(n + 1);
  ax_assume(p != NULL, "oom");
  for (size_t i = 0; i < n; i++)
    p[i] = (char)pl_nat_byte_at(v, i);
  p[n] = '\0';
  return p;
}

static char* rp_strdup(const char* s) {
  size_t n = strlen(s);
  char* out = malloc(n + 1);
  ax_assume(out != NULL, "oom");
  memcpy(out, s, n + 1);
  return out;
}

static char* rp_join_path(const char* root, const char* path) {
  size_t rn = strlen(root);
  size_t pn = strlen(path);
  bool slash = rn > 0 && root[rn - 1] == '/';
  char* out = malloc(rn + (slash ? 0 : 1) + pn + 1);
  ax_assume(out != NULL, "oom");
  memcpy(out, root, rn);
  size_t off = rn;
  if (!slash)
    out[off++] = '/';
  memcpy(out + off, path, pn + 1);
  return out;
}

static bool rp_path_under_root(const char* root, const char* path) {
  if (strcmp(root, "/") == 0)
    return true;
  size_t rn = strlen(root);
  return strncmp(path, root, rn) == 0 && (path[rn] == '\0' || path[rn] == '/');
}

static bool rp_resolve_read_path(pl_thread* t, const char* arg_path,
                                 char** out_path) {
  if (t->rplan_file_root_c == NULL || t->rplan_file_root_c[0] == '\0') {
    *out_path = rp_strdup(arg_path);
    return true;
  }

  char* root = realpath(t->rplan_file_root_c, NULL);
  if (root == NULL)
    return false;

  char* joined = rp_join_path(root, arg_path);
  char* resolved = realpath(joined, NULL);
  free(joined);
  if (resolved == NULL || !rp_path_under_root(root, resolved)) {
    free(root);
    free(resolved);
    return false;
  }

  free(root);
  *out_path = resolved;
  return true;
}

/* bytesBar: the data bytes followed by a 0x01 terminator. */
static pl_val rp_bar(pl_thread* t, const uint8_t* b, size_t n) {
  uint8_t* bar = malloc(n + 1);
  ax_assume(bar != NULL, "oom");
  memcpy(bar, b, n);
  bar[n] = 0x01;
  pl_val out = pl_nat_from_bytes(t, bar, n + 1);
  free(bar);
  return out;
}

/* Pattern `N x`: the reference rplan errors on non-nat where it
 * pattern-matches a nat constructor. */
static pl_val rp_want_nat(pl_thread* t, pl_val v) {
  if (!pl_is_nat(v))
    pl_raise_msg(t, "unknown actor/net op");
  return v;
}

/* ── Console ───────────────────────────────────────────────────────────── */

pl_val pl_op82_input(pl_thread* t, size_t ab) {
  uint64_t n = pl_nat_u64_clamp(pl_nat_coerce(ARG(0)));
  if (n > (1u << 26))
    n = 1u << 26;
  uint8_t* buf = malloc(n ? n : 1);
  ax_assume(buf != NULL, "oom");
  ssize_t r;
  do {
    r = read(STDIN_FILENO, buf, n);
  } while (r < 0 && errno == EINTR);
  if (r < 0) {
    free(buf);
    pl_raise_msg(t, "Input: read failed");
  }
  pl_val out = rp_bar(t, buf, (size_t)r);
  free(buf);
  return out;
}

static pl_val rp_output_fd(pl_thread* t, size_t ab, int fd) {
  size_t n;
  uint8_t* b = rp_nat_bytes(pl_nat_coerce(ARG(0)), true, &n);
  int rc = rp_write_all(fd, b, n);
  free(b);
  if (rc < 0)
    pl_raise_msg(t, "Output: write failed");
  return 0;
}

pl_val pl_op82_output(pl_thread* t, size_t ab) {
  return rp_output_fd(t, ab, STDOUT_FILENO);
}

pl_val pl_op82_warn(pl_thread* t, size_t ab) {
  return rp_output_fd(t, ab, STDERR_FILENO);
}

pl_val pl_op82_print(pl_thread* t, size_t ab) {
  size_t n;
  uint8_t* b = rp_nat_bytes(rp_want_nat(t, ARG(0)), false, &n);
  int rc = rp_write_all(STDOUT_FILENO, b, n);
  free(b);
  if (rc < 0)
    pl_raise_msg(t, "Print: write failed");
  return 0;
}

/* ── Files ─────────────────────────────────────────────────────────────── */

pl_val pl_op82_read_file(pl_thread* t, size_t ab) {
  char* arg_path = rp_nat_path(rp_want_nat(t, ARG(0)));
  char* path = NULL;
  if (!rp_resolve_read_path(t, arg_path, &path)) {
    free(arg_path);
    return 0;
  }
  free(arg_path);
  int fd = open(path, O_RDONLY);
  free(path);
  if (fd < 0)
    return 0;
  struct stat st;
  if (fstat(fd, &st) < 0 || st.st_size < 0) {
    close(fd);
    return 0;
  }
  size_t size = (size_t)st.st_size;
  uint8_t* buf = malloc(size ? size : 1);
  ax_assume(buf != NULL, "oom");
  if (rp_read_all(fd, buf, size) < 0) {
    close(fd);
    free(buf);
    return 0;
  }
  close(fd);
  pl_val out = rp_bar(t, buf, size);
  free(buf);
  return out;
}

pl_val pl_op82_stamp(pl_thread* t, size_t ab) {
  char* path = rp_nat_path(rp_want_nat(t, ARG(0)));
  struct stat st;
  int rc = stat(path, &st);
  free(path);
  if (rc < 0)
    return 0;
  return (pl_val)(uint64_t)st.st_mtime;
}

pl_val pl_op82_now(pl_thread* t, size_t ab) {
  AX_UNUSED(ab);
  time_t now = time(NULL);
  if (now == (time_t)-1)
    pl_raise_msg(t, "Now: clock failed");
  return (pl_val)(uint64_t)now;
}

/* ── Sockets / descriptors (handles via the global fd table) ───────────── */

pl_val pl_op82_closefd(pl_thread* t, size_t ab) {
  uint64_t h = pl_nat_u64_clamp(rp_want_nat(t, ARG(0)));
  if (ax_fd_close((size_t)h) < 0)
    pl_raise_msg(t, "CloseFd: bad handle");
  return 0;
}

pl_val pl_op82_listen(pl_thread* t, size_t ab) {
  uint64_t port = pl_nat_u64_clamp(rp_want_nat(t, ARG(0)));
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    pl_raise_msg(t, "Listen: socket failed");
  int yes = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
    close(fd);
    pl_raise_msg(t, "Listen: setsockopt failed");
  }
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)port);
  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
      listen(fd, 128) < 0) {
    close(fd);
    pl_raise_msg(t, "Listen: bind/listen failed");
  }
  size_t h = ax_fd_add(fd);
  if (h == AX_FD_INVALID) {
    close(fd);
    pl_raise_msg(t, "Listen: fd table full");
  }
  return (pl_val)h;
}

pl_val pl_op82_accept(pl_thread* t, size_t ab) {
  uint64_t h = pl_nat_u64_clamp(rp_want_nat(t, ARG(0)));
  int listen_fd = ax_fd_get((size_t)h);
  if (listen_fd < 0)
    pl_raise_msg(t, "Accept: bad handle");
  int client_fd = accept(listen_fd, NULL, NULL);
  if (client_fd < 0)
    pl_raise_msg(t, "Accept: accept failed");
  size_t ch = ax_fd_add(client_fd);
  if (ch == AX_FD_INVALID) {
    close(client_fd);
    pl_raise_msg(t, "Accept: fd table full");
  }
  return (pl_val)ch;
}

pl_val pl_op82_read(pl_thread* t, size_t ab) {
  uint64_t h = pl_nat_u64_clamp(rp_want_nat(t, ARG(0)));
  uint64_t n = pl_nat_u64_clamp(rp_want_nat(t, ARG(1)));
  if (n > (1u << 26))
    n = 1u << 26;
  int fd = ax_fd_get((size_t)h);
  if (fd < 0)
    pl_raise_msg(t, "Read: bad handle");
  uint8_t* buf = malloc(n ? n : 1);
  ax_assume(buf != NULL, "oom");
  ssize_t r;
  do {
    r = read(fd, buf, n);
  } while (r < 0 && errno == EINTR);
  if (r < 0) {
    free(buf);
    pl_raise_msg(t, "Read: read failed");
  }
  if (r == 0) {
    free(buf);
    return 0; /* reference: empty read is N 0, not an empty bar */
  }
  pl_val out = rp_bar(t, buf, (size_t)r);
  free(buf);
  return out;
}

pl_val pl_op82_write(pl_thread* t, size_t ab) {
  uint64_t h = pl_nat_u64_clamp(rp_want_nat(t, ARG(0)));
  rp_want_nat(t, ARG(1));
  int fd = ax_fd_get((size_t)h);
  if (fd < 0)
    pl_raise_msg(t, "Write: bad handle");
  size_t n;
  uint8_t* b = rp_nat_bytes(ARG(1), true, &n);
  int rc = rp_write_all(fd, b, n);
  free(b);
  if (rc < 0)
    pl_raise_msg(t, "Write: write failed");
  /* the reference closes the socket after a write */
  (void)ax_fd_close((size_t)h);
  return 0;
}

/* ── Actors: coordination effects (see header comment) ─────────────────── */

/* Rebuild the request spine [OpName, args…] from the unapp'd op row on
 * the value stack (name at ab-1, argc args from ab).  Strict args are
 * already forced; the rest stay lazy until the executor services them. */
static pl_val rp_request(pl_thread* t, size_t ab, uint32_t argc) {
  pl_gc_reserve(t, PL_APP_CELLS(argc));
  PL_GC_FORBID(t);
  pl_val r = pl_mk_app_from(t, t->vstack[ab - 1], argc, &t->vstack[ab]);
  PL_GC_ALLOW(t);
  return r;
}

pl_val pl_op82_spawn(pl_thread* t, size_t ab) {
  return rp_request(t, ab, 1); /* fn is taken unevaluated */
}

pl_val pl_op82_send(pl_thread* t, size_t ab) {
  rp_want_nat(t, ARG(0)); /* handle; the message stays lazy */
  return rp_request(t, ab, 2);
}

pl_val pl_op82_send_caps(pl_thread* t, size_t ab) {
  rp_want_nat(t, ARG(0));
  return rp_request(t, ab, 3);
}

pl_val pl_op82_recv(pl_thread* t, size_t ab) {
  if (ARG(0) != 0) /* the reference accepts only the literal nat 0 */
    pl_raise_msg(t, "unknown actor/net op");
  return rp_request(t, ab, 1);
}

pl_val pl_op82_close_handle(pl_thread* t, size_t ab) {
  rp_want_nat(t, ARG(0));
  return rp_request(t, ab, 1);
}
