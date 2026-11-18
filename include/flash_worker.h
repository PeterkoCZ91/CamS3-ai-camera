#ifndef FLASH_WORKER_H
#define FLASH_WORKER_H

// Runs short flash-touching jobs (saveConfig, event-log appends, loading /ca.pem)
// on one dedicated task with an INTERNAL stack.
//
// Why: while the flash is written or read through the SPI1 path the cache is off, and
// a task whose stack lives in PSRAM then faults. Tasks that never reach flash can keep
// their stacks in PSRAM, which gives the scarce internal heap back (see ROADMAP
// Phase 0). The rule for those tasks is therefore: never call LittleFS/NVS directly,
// hand the work to this worker with flashDefer().

typedef void (*FlashJob)();

void flashWorkerStart();                 // idempotent; call early in setup()
// Queue `job` to run on the worker. A job already waiting is not queued twice (so
// jobs must be idempotent "do whatever is pending" functions). If the worker is not
// running yet (early boot) the job runs inline in the caller. Never blocks.
void flashDefer(FlashJob job);

// Persist appConfig from any task, without touching flash on the caller's stack.
void saveConfigDeferred();

#endif
