#include "os/loader.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "axsys/allocator.h"
#include "os/platform.h"
#include "plan/nat.h"

typedef struct loader_module {
  pl_val key;
  en_env_entry* environment;
  struct loader_module* next;
} loader_module;

#define TEMP_ENV_CAPACITY 256

struct os_loader {
  const ax_allocator* allocator;
  en_wisp* wisp;
  loader_module* modules;
  en_env_entry* temporary[TEMP_ENV_CAPACITY];
  size_t temporary_count;
};

static void visit_environment(pl_root_visit visit, void* gc,
                              en_env_entry* environment) {
  for (en_env_entry* entry = environment; entry != NULL; entry = entry->next) {
    visit(&entry->key_v, gc);
    visit(&entry->val_v, gc);
  }
}

static void loader_roots(pl_root_visit visit, void* gc, void* context) {
  os_loader* loader = context;
  for (loader_module* module = loader->modules; module != NULL;
       module = module->next) {
    visit(&module->key, gc);
    visit_environment(visit, gc, module->environment);
  }
  for (size_t i = 0; i < loader->temporary_count; i++)
    visit_environment(visit, gc, loader->temporary[i]);
}

static void environment_free(os_loader* loader, en_env_entry* environment) {
  while (environment != NULL) {
    en_env_entry* next = environment->next;
    ax_free(loader->allocator, environment);
    environment = next;
  }
}

static en_env_entry* environment_find(en_env_entry* environment, pl_val key) {
  for (en_env_entry* entry = environment; entry != NULL; entry = entry->next)
    if (pl_nat_eq(entry->key_v, key))
      return entry;
  return NULL;
}

static en_env_entry* environment_clone(os_loader* loader,
                                       en_env_entry* environment) {
  en_env_entry* head = NULL;
  en_env_entry** tail = &head;
  for (en_env_entry* entry = environment; entry != NULL; entry = entry->next) {
    en_env_entry* copy = ax_calloc(loader->allocator, en_env_entry, 1);
    if (copy == NULL) {
      environment_free(loader, head);
      return NULL;
    }
    *copy = (en_env_entry){
        .key_v = entry->key_v,
        .val_v = entry->val_v,
        .mac_f = entry->mac_f,
    };
    *tail = copy;
    tail = &copy->next;
  }
  return head;
}

static bool environment_put(os_loader* loader, en_env_entry** environment,
                            const en_env_entry* source) {
  en_env_entry* old = environment_find(*environment, source->key_v);
  if (old != NULL) {
    old->val_v = source->val_v;
    old->mac_f = source->mac_f;
    return true;
  }
  en_env_entry* copy = ax_calloc(loader->allocator, en_env_entry, 1);
  if (copy == NULL)
    return false;
  *copy = (en_env_entry){
      .key_v = source->key_v,
      .val_v = source->val_v,
      .mac_f = source->mac_f,
      .next = *environment,
  };
  *environment = copy;
  return true;
}

static en_env_entry* environment_merge(os_loader* loader,
                                       en_env_entry* first,
                                       en_env_entry* second) {
  en_env_entry* result = environment_clone(loader, first);
  if (first != NULL && result == NULL)
    return NULL;
  for (en_env_entry* entry = second; entry != NULL; entry = entry->next) {
    if (!environment_put(loader, &result, entry)) {
      environment_free(loader, result);
      return NULL;
    }
  }
  return result;
}

static void temporary_push(os_loader* loader, en_env_entry* environment) {
  if (loader->temporary_count >= TEMP_ENV_CAPACITY)
    en_wisp_fail(loader->wisp, "module environment root overflow");
  loader->temporary[loader->temporary_count++] = environment;
}

static loader_module* module_find(os_loader* loader, pl_val key) {
  for (loader_module* module = loader->modules; module != NULL;
       module = module->next)
    if (pl_nat_eq(module->key, key))
      return module;
  return NULL;
}

static bool valid_module_name(const char* module) {
  if (module == NULL || *module == '\0')
    return false;
  for (const char* c = module; *c != '\0'; c++)
    if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-')
      return false;
  return true;
}

static bool eat_space(char** cursor) {
  for (;;) {
    if (**cursor == ';')
      while (**cursor != '\n' && **cursor != '\0')
        (*cursor)++;
    if (**cursor == ' ' || **cursor == '\n' || **cursor == '\r' || **cursor == '\t') {
      (*cursor)++;
      continue;
    }
    return **cursor == '\0';
  }
}

static char* include_name(os_loader* loader, pl_val form) {
  if (!pl_is_nat(form))
    return NULL;
  size_t size = pl_nat_byte_len(form);
  if (size < 2 || pl_nat_byte_at(form, 0) != '@')
    return NULL;
  char* name = ax_calloc(loader->allocator, char, size);
  if (name == NULL)
    en_wisp_fail(loader->wisp, "oom");
  for (size_t i = 1; i < size; i++) {
    char c = (char)pl_nat_byte_at(form, i);
    if (!isalnum((unsigned char)c) && c != '_' && c != '-') {
      ax_free(loader->allocator, name);
      return NULL;
    }
    name[i - 1] = c;
  }
  return name;
}

static char* module_text(os_loader* loader, const char* module) {
  size_t name_size = strlen(module);
  char* path = ax_calloc(loader->allocator, char, name_size + 12);
  if (path == NULL)
    return NULL;
  (void)snprintf(path, name_size + 12, "plan/%s.plan", module);
  const os_rom_file* file = os_rom_find(path);
  ax_free(loader->allocator, path);
  if (file == NULL)
    return NULL;
  char* text = ax_calloc(loader->allocator, char, file->size + 1);
  if (text != NULL)
    memcpy(text, file->data, file->size);
  return text;
}

static bool process_file(os_loader* loader, const char* module);

static bool process_form(os_loader* loader, pl_val form) {
  char* include = include_name(loader, form);
  if (include != NULL) {
    bool ok = process_file(loader, include);
    ax_free(loader->allocator, include);
    return ok;
  }
  pl_val expanded = en_wisp_macroexpand(loader->wisp, form);
  if (expanded != 0)
    (void)en_wisp_thunk(loader->wisp, expanded);
  (void)pl_gc_collect_if_pressure(loader->wisp->t, (size_t)1 << 18);
  return true;
}

static en_env_entry* load_module(os_loader* loader, const char* name) {
  en_wisp* wisp = loader->wisp;
  if (!valid_module_name(name))
    return NULL;
  size_t mark = en_root_mark(wisp);
  en_root_push(wisp, en_string_nat(wisp, name));
  loader_module* cached = module_find(loader, wisp->tmp_v[mark]);
  if (cached != NULL) {
    en_env_entry* result = environment_clone(loader, cached->environment);
    en_root_pop(wisp, mark);
    return result;
  }

  char* text = module_text(loader, name);
  if (text == NULL) {
    en_root_pop(wisp, mark);
    return NULL;
  }
  environment_free(loader, wisp->env);
  wisp->env = NULL;
  char* cursor = text;
  while (!eat_space(&cursor)) {
    pl_val form = en_wisp_parse(wisp, &cursor);
    if (!process_form(loader, form)) {
      ax_free(loader->allocator, text);
      en_root_pop(wisp, mark);
      return NULL;
    }
  }
  ax_free(loader->allocator, text);

  en_env_entry* result = environment_clone(loader, wisp->env);
  en_env_entry* cache_environment = environment_clone(loader, result);
  if ((wisp->env != NULL && result == NULL) ||
      (result != NULL && cache_environment == NULL)) {
    en_root_pop(wisp, mark);
    return NULL;
  }
  loader_module* module = ax_calloc(loader->allocator, loader_module, 1);
  if (module == NULL) {
    en_root_pop(wisp, mark);
    return NULL;
  }
  *module = (loader_module){
      .key = wisp->tmp_v[mark],
      .environment = cache_environment,
      .next = loader->modules,
  };
  loader->modules = module;
  en_root_pop(wisp, mark);
  return result;
}

static bool process_file(os_loader* loader, const char* module) {
  en_wisp* wisp = loader->wisp;
  size_t mark = en_root_mark(wisp);
  size_t temporary_mark = loader->temporary_count;
  en_root_push(wisp, en_string_nat(wisp, module));
  en_env_entry* old = environment_clone(loader, wisp->env);
  if (wisp->env != NULL && old == NULL) {
    en_root_pop(wisp, mark);
    return false;
  }
  temporary_push(loader, old);
  en_env_entry* loaded = load_module(loader, module);
  bool found = loaded != NULL || module_find(loader, wisp->tmp_v[mark]) != NULL;
  if (!found) {
    loader->temporary_count = temporary_mark;
    en_root_pop(wisp, mark);
    return false;
  }
  temporary_push(loader, loaded);
  en_env_entry* merged = environment_merge(loader, old, loaded);
  if ((old != NULL || loaded != NULL) && merged == NULL) {
    loader->temporary_count = temporary_mark;
    en_root_pop(wisp, mark);
    return false;
  }
  environment_free(loader, wisp->env);
  environment_free(loader, old);
  environment_free(loader, loaded);
  wisp->env = merged;
  loader->temporary_count = temporary_mark;
  en_root_pop(wisp, mark);
  return true;
}

os_loader* os_loader_new(en_wisp* wisp) {
  os_loader* loader = calloc(1, sizeof(*loader));
  if (loader == NULL)
    return NULL;
  loader->allocator = ax_allocator_system();
  loader->wisp = wisp;
  pl_gc_add_root_source(wisp->heap, loader_roots, loader);
  return loader;
}

void os_loader_free(os_loader* loader) {
  if (loader == NULL)
    return;
  pl_gc_del_root_source(loader->wisp->heap, loader_roots, loader);
  while (loader->modules != NULL) {
    loader_module* next = loader->modules->next;
    environment_free(loader, loader->modules->environment);
    free(loader->modules);
    loader->modules = next;
  }
  free(loader);
}

bool os_loader_load(os_loader* loader, const char* module) {
  return process_file(loader, module);
}

bool os_loader_binding(os_loader* loader, const char* name, pl_val* out) {
  en_wisp* wisp = loader->wisp;
  size_t mark = en_root_mark(wisp);
  en_root_push(wisp, en_string_nat(wisp, name));
  en_env_entry* entry = en_wisp_getenv(wisp, wisp->tmp_v[mark]);
  if (entry != NULL && out != NULL)
    *out = entry->val_v;
  en_root_pop(wisp, mark);
  return entry != NULL;
}
