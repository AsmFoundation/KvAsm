/*
 * engine.h — the main thread's view of the load engine (engine.c).
 */
#ifndef KVBENCH_ENGINE_H
#define KVBENCH_ENGINE_H

#include "kvbench.h"

/** Sums over all workers after a run. */
typedef struct {
  uint64_t done[OP_COUNT];  /**< completions in the measured phase, per op */
  uint64_t done_all;        /**< completions in any phase */
  uint64_t misses;          /**< GETs that found no value (all keys were preloaded) */
  uint64_t bad_values;      /**< GETs that returned wrong bytes */
  uint64_t errors;          /**< server errors and connection failures */
  uint64_t stalls;          /**< open loop: requests that waited for a free slot */
  Hist hist[OP_COUNT];      /**< measured-phase latency per op */
  char error_text[200];     /**< first error description, empty if none */
} Totals;

/** Resolves g_opt.host/port and selects the protocol. @returns 0 or -1. */
int engine_resolve(void);

/**
 * Waits until the server accepts TCP connections.
 * @param seconds  timeout
 * @returns 0, or -1 on timeout
 */
int engine_wait_ready(double seconds);

/**
 * Starts the worker threads; returns when every connection is established.
 * @param preload  1 to write each key once instead of running the workload
 * @returns 0, or -1 if a connection failed (details after engine_join)
 */
int engine_start(int preload);

/**
 * Joins the workers (preload: after their key ranges are written; workload:
 * after g_phase became PHASE_STOP) and aggregates their results.
 * @param t  receives the totals
 */
void engine_join(Totals *t);

/** Renders the text protocols' constant length headers (after option parsing). */
void proto_text_init(void);

/**
 * Total completions so far across all workers (for the per-interval
 * timeline sampled by the main thread during the measured phase).
 * @returns completion count
 */
uint64_t engine_progress(void);

#endif
