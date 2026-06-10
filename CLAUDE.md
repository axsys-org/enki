# enki

C PLAN runtime. Layering (enforced by `make check-layering`): pkg/axsys < pkg/plan < pkg/enki. `reaver/` submodule is the Haskell reference implementation (semantic oracle).

## Running tests

- **Criterion unit tests cannot run in a local shell on this machine** (macOS arm64): boxfort fails with "Could not spawn test instance: Protocol error" everywhere, including `nix develop`. This is environmental, not a code problem. Run them inside the nix build sandbox instead:
  - one check: `nix build .#checks.aarch64-darwin.unit-tests-debug -L --no-link`
  - full matrix: `nix flake check -L --keep-going`
- theft property tests run fine locally: `make test-property` (also with `YIELD_STRESS=1 GC_STRESS=1`).
- Stress flags: `make ... GC_STRESS=1` (collect on every reserve), `make ... YIELD_STRESS=1` (suspend at every depth-0 safepoint). Use a separate `BUILD_DIR=` for stress builds to avoid stale objects. Flake checks: `{unit,property}-tests-debug-yield-stress`.

## nix flake check gotchas

- Flake source includes only git-tracked files: `git add -N <newfile>` before checking, or new tests silently won't run.
- Run `nix fmt` first; the `format` check (clang-format/mdformat/alejandra) fails the whole check otherwise.

## Writing plan-layer tests

- APP cells are WHNF by construction (data is under-applied by invariant). A hand-built saturated APP node is NOT a redex — the machine returns it unevaluated. Drive redexes through lazy thunks with KAL body code: see `test_thunk` in `tests/unit/test_plan_eval.c` and `tests/unit/test_plan_thread.c`. In a 1-slot-env thunk body, `(0 f x)` is application, `(0 v)` is a literal, and nats ≥ 1 are self-literals.
- GC discipline in tests: no `pl_val` C local may survive a `pl_gc_reserve` — root values on `t->vstack` and re-fetch (invariant I4 in plan/heap.h).
