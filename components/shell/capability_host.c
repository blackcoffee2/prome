/*
 * Capability host — implementation.
 *
 * Enumerates the .cap_registry linker section and initializes each registered
 * capability. Initialization failure is non-fatal: the capability is simply
 * marked not-ready, and any app-API service it backs reports unavailable. This
 * is what lets a build that includes a capability run unchanged on a device
 * whose matching hardware is absent.
 */
#include "capability_host.h"
#include "capability.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "cap_host";

/* Registry section boundaries provided by linker.lf (the same fragment
 * declares both the app and capability sections). Declared as arrays so the
 * bounds and pointer arithmetic are well-defined. */
extern capability_descriptor_t *_cap_registry_start[];
extern capability_descriptor_t *_cap_registry_end[];

static size_t cap_count(void)
{
    return (size_t)(_cap_registry_end - _cap_registry_start);
}

void capability_host_init(void)
{
    size_t count = cap_count();
    for (size_t i = 0; i < count; i++) {
        capability_descriptor_t *cap = _cap_registry_start[i];
        if (cap == NULL) {
            continue;
        }
        esp_err_t err = cap->init ? cap->init() : ESP_OK;
        cap->ready = (err == ESP_OK);
        ESP_LOGI(TAG, "capability '%s' %s",
                 cap->name ? cap->name : "?",
                 cap->ready ? "ready" : "unavailable");
    }
}

bool capability_ready(const char *name)
{
    if (name == NULL) {
        return false;
    }
    size_t count = cap_count();
    for (size_t i = 0; i < count; i++) {
        capability_descriptor_t *cap = _cap_registry_start[i];
        if (cap && cap->name && strcmp(cap->name, name) == 0) {
            return cap->ready;
        }
    }
    return false;
}