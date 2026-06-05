#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <setjmp.h>
#include <enki/wisp.h>

wisp_rt* rt = NULL;

static ssize_t repl(char** out_c) {
  size_t cap_s;
  fputs("> ", stdout);
  fflush(stdout);

  ssize_t len_s = getline(out_c, &cap_s, stdin);
  if (len_s < 0) {
    if (feof(stdin))
      return -1;
    if (errno == EINTR) {
      clearerr(stdin);
      return 0;
    }

    perror("getline");
    return -1;
  }
  // if (len_s > 0 && *out_c[len_s - 1] == '\n') // strip the newline
  (*out_c)[--len_s] = '\0';
  return len_s;
}

int main() {
  const enki_allocator* loc_a = enki_allocator_system();
  rt = wisp_rt_alloc(loc_a);

  char* inp_c = NULL;
  ssize_t inp_s;
  rt->err_f = true;
  while (inp_s = repl(&inp_c), inp_s >= 0) {
    // printf("read@(%li): %s\n", inp_s, inp_c);
    char* txt_c = inp_c;
    if (setjmp(rt->errjmp)) {
      if (rt->msg_c == NULL)
        rt->msg_c = "Unknown Error";
      printf("Failed to evaluate: %s\n", rt->msg_c);
      continue;
    } else {
      er_val val_v = wisp_parse(rt, &txt_c);
      er_val eve_v = wisp_eval(rt, val_v);
      char* out_c = wisp_print_value(rt, eve_v, NULL);
      printf(">> %s\n", out_c);
      loc_a->free(loc_a->ctx, out_c);
    }
  }
  return 0;
}
