/*
 * Capability descriptor and self-registration.
 *
 * A capability is a hardware-backed service the shell exposes to apps through
 * the curated API (the camera viewfinder, the backlight brightness control,
 * and so on). Like apps, capabilities self-register into a linker section, so
 * including a capability component in the build adds its service and excluding
 * it removes the service, with no edit to the shell.
 *
 * The split between an app and a capability: an app is user-facing UI built on
 * the curated API; a capability is the C-side hardware service that backs one
 * of those API verbs. Apps are sandboxed and portable; capabilities own the
 * hardware and are the only code allowed to.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One capability's registration record.
 *
 *   name   Identifier the shell uses to query readiness (capability_ready) and
 *          to wire the matching API verbs. Matches the name the API binding
 *          checks.
 *   init   Brings up the capability's hardware. Returns ESP_OK when the service
 *          is usable. A non-OK return (or absent hardware) leaves the
 *          capability not-ready rather than failing startup.
 *   ready  Set by the capability host after init: true when init returned
 *          ESP_OK. The API layer gates the capability's verbs on this, so an
 *          uninitialized capability reports its service unavailable.
 */
typedef struct {
    const char *name;
    esp_err_t (*init)(void);
    bool ready;
} capability_descriptor_t;

/*
 * Place a pointer to a static capability_descriptor_t into the .cap_registry
 * section so the capability host discovers and initializes it at startup. As
 * with apps, the pointer is marked used and the component must be kept from
 * linker garbage collection (WHOLE_ARCHIVE), since nothing references the
 * descriptor by name.
 */
#define REGISTER_CAPABILITY(descriptor)                                       \
    static capability_descriptor_t *const _cap_reg_##descriptor               \
        __attribute__((used, aligned(4), section("cap_registry"))) = &descriptor

#ifdef __cplusplus
}
#endif