#include "plan/heap.h"
#include "plan/store.h"

pl_heap* os_pl_heap_new(size_t cells, pl_store* store) {
  const size_t compiler_cells = (size_t)1 << 23; /* 64 MiB per semispace */
  if (cells > compiler_cells)
    cells = compiler_cells;
  return pl_heap_new(cells, store);
}
