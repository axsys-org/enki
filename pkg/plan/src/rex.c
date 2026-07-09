#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "plan/build.h"
#include "plan/nat.h"

/*
 * ParseRex / PrintRex (op 66) — the Rex notation-subset floor.
 *
 * The normative full Rex grammar lives in the reaver reference
 * (Rex.hs / PrintRex.hs, op-66 ParseRex in Plan.hs); this is the
 * radical-simplicity subset covering the notation-primitive forms of
 * Notation-Primitive-EBNF-Golden NE1/NE2 — the cassette-chain authoring
 * surface: `@head [name] {:key value} [=]` lines with two-space
 * indentation, attr continuation lines, quoted strings, bracket values,
 * and `;` comments.  It grows toward the full grammar against the
 * upstream golden fixtures; consumers see the same row discipline either
 * way.
 *
 * AST rows (head-0 apps; lists are head-0 rows, empty = 0):
 *
 *   form  = (0 "form" span head name attrs kids)
 *   span  = (0 line col off len)            1-based line/col
 *   attr  = (0 key value style)             style: 0 raw, 1 quoted
 *   attrs = (0 attr...)                     kids = (0 form...)
 *
 * head keeps its '@'; keys keep their ':'; values are exact lexeme
 * bytes (quotes stripped for style 1).  ParseRex returns the row of
 * top-level forms, or 0 on any parse failure (no partial repair —
 * NE1a: fix the source, don't add tree-repair heuristics).
 */

#define ARG(i) (t->vstack[ab + (i)])

/* ── Row building ──────────────────────────────────────────────────────── */

/* Build a head-0 row from vstack[base..vsp), pop to base.  Empty -> 0. */
static pl_val rx_row(pl_thread* t, size_t base) {
  uint32_t n = (uint32_t)(t->vsp - base);
  if (n == 0)
    return 0;
  pl_gc_reserve(t, PL_APP_CELLS(n));
  PL_GC_FORBID(t);
  pl_val row = pl_mk_app_from(t, 0, n, &t->vstack[base]);
  PL_GC_ALLOW(t);
  t->vsp = base;
  return row;
}

static pl_val rx_str(pl_thread* t, const uint8_t* b, size_t n) {
  return pl_nat_from_bytes(t, b, n);
}

/* ── Source lines ──────────────────────────────────────────────────────── */

typedef struct rx_line {
  size_t off;      /* content start (after indent) */
  size_t end;      /* content end (after comment/space strip) */
  uint32_t indent; /* leading spaces */
  uint32_t lineno; /* 1-based */
} rx_line;

typedef struct rx_src {
  const uint8_t* b;
  size_t n;
  rx_line* lines;
  size_t nlines;
} rx_src;

/* Split into non-blank lines; strip `;` comments (outside quotes and
 * brackets) and trailing spaces.  Tabs refuse. */
static bool rx_split(rx_src* s) {
  size_t cap = 64, nl = 0;
  rx_line* ls = malloc(cap * sizeof(rx_line));
  if (ls == NULL)
    return false;
  size_t i = 0;
  uint32_t lineno = 0;
  while (i < s->n) {
    lineno++;
    size_t start = i;
    while (i < s->n && s->b[i] != '\n')
      i++;
    size_t end = i;
    if (i < s->n)
      i++; /* eat newline */
    size_t p = start;
    while (p < end && s->b[p] == ' ')
      p++;
    if (p < end && s->b[p] == '\t') {
      free(ls);
      return false;
    }
    /* comment strip */
    bool in_str = false;
    int depth = 0;
    size_t cend = end;
    for (size_t q = p; q < end; q++) {
      uint8_t c = s->b[q];
      if (in_str) {
        if (c == '"')
          in_str = false;
      } else if (c == '"') {
        in_str = true;
      } else if (c == '[') {
        depth++;
      } else if (c == ']') {
        depth--;
      } else if (c == ';' && depth == 0) {
        cend = q;
        break;
      }
    }
    while (cend > p && s->b[cend - 1] == ' ')
      cend--;
    if (cend == p)
      continue; /* blank or comment-only */
    if (nl == cap) {
      cap *= 2;
      rx_line* grown = realloc(ls, cap * sizeof(rx_line));
      if (grown == NULL) {
        free(ls);
        return false;
      }
      ls = grown;
    }
    ls[nl] = (rx_line){.off = p,
                       .end = cend,
                       .indent = (uint32_t)(p - start),
                       .lineno = lineno};
    nl++;
  }
  s->lines = ls;
  s->nlines = nl;
  return true;
}

/* ── Tokens ────────────────────────────────────────────────────────────── */

typedef struct rx_tok {
  size_t off; /* lexeme start (quotes stripped for style 1) */
  size_t len;
  uint8_t style; /* 0 raw, 1 quoted */
  bool ok;
} rx_tok;

static rx_tok rx_next(const rx_src* s, size_t* p, size_t end) {
  rx_tok none = {0, 0, 0, false};
  while (*p < end && s->b[*p] == ' ')
    (*p)++;
  if (*p >= end)
    return none;
  size_t q = *p;
  if (s->b[q] == '"') {
    size_t r = q + 1;
    while (r < end && s->b[r] != '"')
      r++;
    if (r >= end)
      return none; /* unterminated */
    *p = r + 1;
    return (rx_tok){.off = q + 1, .len = r - (q + 1), .style = 1, .ok = true};
  }
  if (s->b[q] == '[') {
    int depth = 0;
    size_t r = q;
    while (r < end) {
      if (s->b[r] == '[')
        depth++;
      else if (s->b[r] == ']' && --depth == 0)
        break;
      r++;
    }
    if (r >= end)
      return none; /* unbalanced */
    *p = r + 1;
    return (rx_tok){.off = q, .len = r + 1 - q, .style = 0, .ok = true};
  }
  size_t r = q;
  while (r < end && s->b[r] != ' ')
    r++;
  *p = r;
  return (rx_tok){.off = q, .len = r - q, .style = 0, .ok = true};
}

/* ── Parser ────────────────────────────────────────────────────────────── */

/* Parse the attrs on a line starting at *p; each is `:key value`.
 * Pushes attr rows.  Returns false on malformed input; sets *saw_eq if
 * the line ends with a lone `=`. */
static bool rx_attrs(pl_thread* t, const rx_src* s, size_t* p, size_t end,
                     bool* saw_eq) {
  for (;;) {
    size_t save = *p;
    rx_tok k = rx_next(s, p, end);
    if (!k.ok)
      return true;
    if (k.style == 0 && k.len == 1 && s->b[k.off] == '=') {
      size_t after = *p;
      if (rx_next(s, &after, end).ok)
        return false; /* `=` must be last */
      *saw_eq = true;
      return true;
    }
    if (k.style != 0 || s->b[k.off] != ':') {
      *p = save;
      return false;
    }
    rx_tok v = rx_next(s, p, end);
    if (!v.ok)
      return false;
    size_t base = t->vsp;
    pl_vpush(t, rx_str(t, s->b + k.off, k.len));
    pl_vpush(t, rx_str(t, s->b + v.off, v.len));
    pl_vpush(t, v.style);
    pl_val attr = rx_row(t, base);
    pl_vpush(t, attr);
  }
}

static pl_val rx_form(pl_thread* t, const rx_src* s, size_t* idx,
                      uint32_t indent);

/* Parse forms at `indent` starting at *idx; pushes them; returns false
 * on failure. */
static bool rx_forms(pl_thread* t, const rx_src* s, size_t* idx,
                     uint32_t indent) {
  while (*idx < s->nlines && s->lines[*idx].indent == indent) {
    if (s->b[s->lines[*idx].off] != '@')
      return false;
    pl_val f = rx_form(t, s, idx, indent);
    if (f == 0)
      return false;
    pl_vpush(t, f);
  }
  if (*idx < s->nlines && s->lines[*idx].indent > indent)
    return false; /* over-indented orphan */
  return true;
}

static pl_val rx_form(pl_thread* t, const rx_src* s, size_t* idx,
                      uint32_t indent) {
  const rx_line* ln = &s->lines[*idx];
  size_t p = ln->off;
  rx_tok head = rx_next(s, &p, ln->end);
  if (!head.ok || head.style != 0 || s->b[head.off] != '@')
    return 0;

  size_t s0 = t->vsp;
  pl_vpush(t, rx_str(t, s->b + head.off, head.len)); /* s0+0 head */
  pl_vpush(t, 0);                                    /* s0+1 name */

  /* optional bare name before attrs */
  size_t save = p;
  rx_tok nm = rx_next(s, &p, ln->end);
  if (nm.ok && nm.style == 0 && s->b[nm.off] != ':' &&
      !(nm.len == 1 && s->b[nm.off] == '=')) {
    t->vstack[s0 + 1] = rx_str(t, s->b + nm.off, nm.len);
  } else {
    p = save;
  }

  bool has_kids = false;
  size_t abase = t->vsp;
  if (!rx_attrs(t, s, &p, ln->end, &has_kids)) {
    t->vsp = s0;
    return 0;
  }
  (*idx)++;

  if (has_kids) {
    /* attr continuation lines precede child forms */
    while (*idx < s->nlines && s->lines[*idx].indent == indent + 2 &&
           s->b[s->lines[*idx].off] == ':') {
      const rx_line* al = &s->lines[*idx];
      size_t ap = al->off;
      bool stray_eq = false;
      if (!rx_attrs(t, s, &ap, al->end, &stray_eq) || stray_eq ||
          ap < al->end) {
        t->vsp = s0;
        return 0;
      }
      (*idx)++;
    }
  } else if (p < ln->end) {
    t->vsp = s0;
    return 0; /* trailing garbage */
  }
  pl_val attrs = rx_row(t, abase);
  pl_vpush(t, attrs); /* s0+2 */

  size_t kbase = t->vsp;
  if (has_kids) {
    if (!rx_forms(t, s, idx, indent + 2)) {
      t->vsp = s0;
      return 0;
    }
  }
  pl_val kids = rx_row(t, kbase);
  pl_vpush(t, kids); /* s0+3 */

  size_t sbase = t->vsp;
  pl_vpush(t, ln->lineno);
  pl_vpush(t, indent + 1);
  pl_vpush(t, (pl_val)ln->off);
  pl_vpush(t, (pl_val)(ln->end - ln->off));
  pl_val span = rx_row(t, sbase);
  pl_vpush(t, span); /* s0+4 */

  size_t fbase = t->vsp;
  pl_vpush(t, rx_str(t, (const uint8_t*)"form", 4));
  pl_vpush(t, t->vstack[s0 + 4]);
  pl_vpush(t, t->vstack[s0 + 0]);
  pl_vpush(t, t->vstack[s0 + 1]);
  pl_vpush(t, t->vstack[s0 + 2]);
  pl_vpush(t, t->vstack[s0 + 3]);
  pl_val form = rx_row(t, fbase);
  t->vsp = s0;
  return form;
}

pl_val pl_op_parse_rex(pl_thread* t, size_t ab) {
  pl_val src = pl_nat_coerce(ARG(0));
  size_t n = pl_nat_byte_len(src);
  uint8_t* bytes = malloc(n ? n : 1);
  if (bytes == NULL)
    return 0;
  for (size_t i = 0; i < n; i++)
    bytes[i] = pl_nat_byte_at(src, i);
  rx_src s = {.b = bytes, .n = n, .lines = NULL, .nlines = 0};
  if (!rx_split(&s)) {
    free(bytes);
    return 0;
  }
  size_t base = t->vsp;
  size_t idx = 0;
  pl_val out = 0;
  if (rx_forms(t, &s, &idx, 0) && idx == s.nlines && t->vsp > base)
    out = rx_row(t, base);
  else
    t->vsp = base;
  free(s.lines);
  free(bytes);
  return out;
}

/* ── Printer ───────────────────────────────────────────────────────────── */

typedef struct rx_buf {
  uint8_t* p;
  size_t n, cap;
  bool ok;
} rx_buf;

static void rx_put(rx_buf* b, const uint8_t* d, size_t n) {
  if (!b->ok)
    return;
  if (b->n + n > b->cap) {
    size_t cap = b->cap ? b->cap * 2 : 256;
    while (cap < b->n + n)
      cap *= 2;
    uint8_t* grown = realloc(b->p, cap);
    if (grown == NULL) {
      b->ok = false;
      return;
    }
    b->p = grown;
    b->cap = cap;
  }
  memcpy(b->p + b->n, d, n);
  b->n += n;
}

static void rx_put_c(rx_buf* b, char c) {
  rx_put(b, (const uint8_t*)&c, 1);
}

static void rx_put_nat(rx_buf* b, pl_val v) {
  size_t n = pl_nat_byte_len(v);
  for (size_t i = 0; i < n; i++) {
    uint8_t c = pl_nat_byte_at(v, i);
    rx_put(b, &c, 1);
  }
}

static pl_val* rx_fields(pl_val v, const char* tag, uint32_t n) {
  pl_cell* p = pl_as(PL_TAG_APP, v);
  if (p == NULL || pl_app_head(p) != 0 || pl_app_n(p) != n)
    return NULL;
  pl_val t0 = pl_app_args(p)[0];
  size_t tn = strlen(tag);
  if (pl_nat_byte_len(t0) != tn)
    return NULL;
  for (size_t i = 0; i < tn; i++)
    if (pl_nat_byte_at(t0, i) != (uint8_t)tag[i])
      return NULL;
  return pl_app_args(p);
}

static bool rx_print_form(rx_buf* b, pl_val form, uint32_t depth) {
  pl_val* f = rx_fields(form, "form", 6);
  if (f == NULL)
    return false;
  for (uint32_t i = 0; i < depth * 2; i++)
    rx_put_c(b, ' ');
  rx_put_nat(b, f[2]);
  if (f[3] != 0) {
    rx_put_c(b, ' ');
    rx_put_nat(b, f[3]);
  }
  if (f[4] != 0) {
    pl_cell* attrs = pl_as(PL_TAG_APP, f[4]);
    if (attrs == NULL || pl_app_head(attrs) != 0)
      return false;
    for (uint32_t i = 0; i < pl_app_n(attrs); i++) {
      pl_cell* a = pl_as(PL_TAG_APP, pl_app_args(attrs)[i]);
      if (a == NULL || pl_app_head(a) != 0 || pl_app_n(a) != 3)
        return false;
      rx_put_c(b, ' ');
      rx_put_nat(b, pl_app_args(a)[0]);
      rx_put_c(b, ' ');
      if (pl_nat_u64_clamp(pl_nat_coerce(pl_app_args(a)[2])) == 1) {
        rx_put_c(b, '"');
        rx_put_nat(b, pl_app_args(a)[1]);
        rx_put_c(b, '"');
      } else {
        rx_put_nat(b, pl_app_args(a)[1]);
      }
    }
  }
  if (f[5] != 0) {
    rx_put(b, (const uint8_t*)" =\n", 3);
    pl_cell* kids = pl_as(PL_TAG_APP, f[5]);
    if (kids == NULL || pl_app_head(kids) != 0)
      return false;
    for (uint32_t i = 0; i < pl_app_n(kids); i++)
      if (!rx_print_form(b, pl_app_args(kids)[i], depth + 1))
        return false;
  } else {
    rx_put_c(b, '\n');
  }
  return b->ok;
}

pl_val pl_op_print_rex(pl_thread* t, size_t ab) {
  pl_val forms = ARG(0);
  pl_cell* p = pl_as(PL_TAG_APP, forms);
  if (p == NULL || pl_app_head(p) != 0)
    return 0;
  rx_buf b = {.p = NULL, .n = 0, .cap = 0, .ok = true};
  for (uint32_t i = 0; i < pl_app_n(p); i++) {
    if (!rx_print_form(&b, pl_app_args(p)[i], 0)) {
      free(b.p);
      return 0;
    }
  }
  pl_val out = b.n > 0 ? pl_nat_from_bytes(t, b.p, b.n) : 0;
  free(b.p);
  return out;
}
