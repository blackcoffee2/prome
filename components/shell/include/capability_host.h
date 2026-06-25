/*
 * Capability host — public interface.
 *
 * Initializes every registered capability at startup and answers readiness
 * queries afterward. The curated API layer calls capability_ready to gate the
 * verbs it backs, so a capability whose hardware is absent reports its service
 * unavailable rather than faulting.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize all registered capabilities. Walks the capability registry and
 * calls each capability's init(), recording readiness. Initialization failure
 * is non-fatal: the capability is marked not-ready and the rest proceed.
 * Called once during shell startup, before any app can run.
 */
void capability_host_init(void);

/*
 * Whether the named capability initialized successfully and its service is
 * usable. Returns false for an unknown name or a capability whose init failed.
 * The API layer uses this to decide whether a verb is live.
 */
bool capability_ready(const char *name);

#ifdef __cplusplus
}
#endif