/*
 * Storage — public interface.
 *
 * Persistent storage on a dedicated flash partition, exposed as per-app-scoped
 * file access. Every read and write is namespaced under a scope (the app's
 * name), and relative paths are sanitized so an app cannot reach outside its
 * own folder. Nothing above this module names the underlying filesystem.
 *
 * The curated app API's storage_read/storage_write forward here with the
 * running app's name as the scope, so an app simply reads and writes relative
 * paths and stays confined to its folder automatically.
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Mount the storage partition. Formats it on first use if the mount fails so a
 * blank device comes up with working storage. Returns ESP_OK on success; on
 * failure the scoped read/write calls report errors and the device continues
 * without persistence.
 */
esp_err_t storage_init(void);

/*
 * Read from "<scope>/<rel_path>" into buf (up to len bytes). Returns the number
 * of bytes read, 0 if the file does not exist, or negative on error (including
 * an unsafe rel_path or unmounted storage). rel_path must be relative and must
 * not escape the scope folder.
 */
int storage_read_scoped(const char *scope, const char *rel_path,
                        void *buf, size_t len);

/*
 * Write buf (len bytes) to "<scope>/<rel_path>", creating the scope folder if
 * needed and overwriting any existing file. Returns the number of bytes
 * written, or negative on error (including an unsafe rel_path or unmounted
 * storage).
 */
int storage_write_scoped(const char *scope, const char *rel_path,
                         const void *buf, size_t len);

#ifdef __cplusplus
}
#endif