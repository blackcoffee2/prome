/*
 * Storage — implementation.
 *
 * Mounts LittleFS on a dedicated flash partition and exposes per-app-scoped
 * file access. LittleFS is chosen over SPIFFS for real directories (needed for
 * per-app subdirectories), power-loss resilience, and gentler flash wear.
 *
 * Scoping: every access resolves to "<root>/<scope>/<rel_path>". The scope is
 * the app's name; rel_path is sanitized to reject absolute paths and parent
 * traversal so an app cannot read or write outside its own folder. The folder
 * is created lazily on first write.
 *
 * Nothing above this module names LittleFS; callers pass POSIX-style relative
 * paths and a scope key.
 */
#include "storage.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_littlefs.h"
#include "esp_log.h"

static const char *TAG = "storage";

/* Mount point and partition label. The partition is defined in the partition
 * table (partitions.csv). */
#define STORAGE_ROOT "/store"
#define STORAGE_PARTITION_LABEL "storage"

static bool s_mounted = false;

esp_err_t storage_init(void)
{
    const esp_vfs_littlefs_conf_t conf = {
        .base_path = STORAGE_ROOT,
        .partition_label = STORAGE_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_mounted = true;
    ESP_LOGI(TAG, "littlefs mounted at %s", STORAGE_ROOT);
    return ESP_OK;
}

/*
 * Reject any relative path that is absolute or contains a parent reference, so
 * a scoped access cannot escape its folder. Returns true if the path is safe.
 */
static bool rel_path_is_safe(const char *rel_path)
{
    if (rel_path == NULL || rel_path[0] == '\0' || rel_path[0] == '/') {
        return false;
    }
    if (strstr(rel_path, "..") != NULL) {
        return false;
    }
    return true;
}

/*
 * Compose "<root>/<scope>/<rel_path>" into out. Returns true on success, false
 * if the inputs are unsafe or the result would overflow.
 */
static bool resolve_scoped(const char *scope, const char *rel_path,
                           char *out, size_t out_len)
{
    if (scope == NULL || !rel_path_is_safe(rel_path)) {
        return false;
    }
    int n = snprintf(out, out_len, "%s/%s/%s", STORAGE_ROOT, scope, rel_path);
    return n > 0 && (size_t)n < out_len;
}

/* Ensure the app's scope folder exists before a write. */
static void ensure_scope_dir(const char *scope)
{
    char dir[128];
    if (snprintf(dir, sizeof(dir), "%s/%s", STORAGE_ROOT, scope) < (int)sizeof(dir)) {
        mkdir(dir, 0755);
    }
}

int storage_read_scoped(const char *scope, const char *rel_path,
                        void *buf, size_t len)
{
    if (!s_mounted) {
        return -1;
    }
    char path[160];
    if (!resolve_scoped(scope, rel_path, path, sizeof(path))) {
        return -1;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    return (int)got;
}

int storage_write_scoped(const char *scope, const char *rel_path,
                         const void *buf, size_t len)
{
    if (!s_mounted) {
        return -1;
    }
    char path[160];
    if (!resolve_scoped(scope, rel_path, path, sizeof(path))) {
        return -1;
    }
    ensure_scope_dir(scope);

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return -1;
    }
    size_t put = fwrite(buf, 1, len, f);
    fclose(f);
    return (int)put;
}