/* modpack.h -- ChronoMod-compatible .ctp mod pack support
 *
 * Scans config.mods_dir for *.ctp files (standard ZIP archives whose
 * internal paths match resources.bin entry paths) and, if any are found,
 * produces a patched in-memory copy of resources.bin with those entries
 * replaced.  The original file on disk is never modified.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __MODPACK_H__
#define __MODPACK_H__

#include <stdint.h>
#include <stddef.h>

/* modpack_get_patched_resources
 *
 * If config.mods_dir contains at least one .ctp file with entries that match
 * paths inside resources.bin, allocates and returns a fully-rebuilt patched
 * copy of the archive in *out_buf / *out_len (caller must free(*out_buf)).
 *
 * Returns 1 on success (patched buffer ready), 0 if no patches were applied
 * (no mods folder, no .ctp files, or no matching entries found) -- in the
 * latter case *out_buf and *out_len are untouched and the caller should serve
 * the original file unchanged.
 *
 * On any internal error the function cleans up, prints a diagnostic via
 * debugPrintf, and returns 0 so the game always falls back to vanilla.
 */
int modpack_get_patched_resources(const char *real_path,
                                  uint8_t   **out_buf,
                                  size_t     *out_len);

#endif /* __MODPACK_H__ */