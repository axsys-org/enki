#ifndef HOST_GPU_METAL_H
#define HOST_GPU_METAL_H

/*
 * Direct-Metal host thunks for the op-83 binding table (plan/hostcall.h).
 *
 * Adapted from the shrineOS no-api Metal backend (NoApiMetalBackend.m):
 * sessions own the device, queue, arena, and every object they
 * materialize; thunks are inert mechanics that consume already-admitted
 * PLAN values and return witness/refusal rows.  No thunk interprets
 * artifact bytes beyond handing them to the Metal compiler; no thunk
 * owns fallback, selection, or scene/layout/material meaning.
 *
 * Carrier records (foil/shrine/no-api-carriers.plan) arrive as
 * under-applied laws: the record kind is the law name, the fields are
 * the app args.
 *
 * Registered bindings (argc; all args strict):
 *
 *   gpu.session-open     1  profile             -> (session)
 *   gpu.arena            2  session arena-rec   -> (capacity)
 *   gpu.alloc            2  session bytes-bar   -> (alloc 0 length gen)
 *   gpu.free             2  session pointer-rec -> (alloc gen')
 *   gpu.device-address   2  session pointer-rec -> (u64 gpu address)
 *   gpu.read             2  session pointer-rec -> (window bytes-bar)
 *   gpu.library          2  session msl-bar     -> (library hits)
 *   gpu.pipeline-render  2  session msl-bar     -> (pipeline hits)
 *   gpu.pipeline-compute 2  session msl-bar     -> (pipeline hits)
 *   gpu.target           3  session w h         -> (target)
 *   gpu.texture-2d       4  session w h bgra8-bar -> (texture)
 *   gpu.descriptor-heap  2  session heap-rec    -> (dheap count)
 *   gpu.descriptor-write 4  session dheap window-rec texture
 *                                                -> (index generation)
 *   gpu.command-graph    6  graph-rec session pipeline target-or-0
 *                           pointer-rec dheap-or-0
 *                                                -> (executed lowered event)
 *   gpu.readback         4  session target x y  -> (bgra8-pixel)
 *   gpu.session-close    1  session             -> ()
 *
 * A render pipeline makes the graph a render pass chain (draw/clear
 * steps; declared hazards lower to pass splits).  A compute pipeline
 * makes it a CONCURRENT compute encoder (dispatch steps {gx gy gz
 * threads-x}; declared hazards lower to real buffer memory barriers).
 * Root payloads may embed gpu.device-address values — the whole arena
 * is resident (useHeap); native never chases the addresses, shaders do.
 *
 * Bars follow the op-82 convention: payload bytes then a 0x01 top byte.
 * Library/pipeline witnesses carry cache evidence: hits = prior
 * compilations served from the artifact-keyed cache (0 = cold build).
 * The command-graph witness carries the hazard lowering (pass splits)
 * and the MTLSharedEvent signal value; gpu.readback waits that event —
 * waitUntilCompleted appears nowhere in the backend.
 */

#include <stdbool.h>

/* Contribute the gpu.* bindings; false if any registration is refused
 * (duplicate binding or table full). */
bool pl_host_gpu_metal_register(void);

#endif
