/* cache_progress.h -- "Caching mods..." splash drawn directly over the live
 * EGL surface while modpack.c rebuilds the mod cache on a cache miss.
 *
 * Copyright (C) 2026 ppkantorski <https://github.com/ppkantorski>
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __CACHE_PROGRESS_H__
#define __CACHE_PROGRESS_H__

// Wire up cocos2d::GL::invalidateStateCache so we can tell the engine its
// cached GL state is stale after we've drawn behind its back. Call once from
// main.c (same pattern as movie_set_gl_invalidate()) before the mod cache
// can possibly be rebuilt, i.e. before e_nativeInit().
void cache_progress_set_gl_invalidate(void (*fn)(void));

// Draws one frame of the caching splash -- white "Caching mods... NN%" text
// with a centered progress bar below it -- and swaps buffers. frac is
// clamped to [0,1]. Lazily creates its GL resources on the first call, and
// uses whatever EGL context/surface is already current on the calling
// thread (the main thread, which owns the context egl_init() created).
void cache_progress_update(float frac);

// Releases the GL resources created by cache_progress_update and invalidates
// cocos2d's GL state cache so its own renderer starts from a clean slate.
// Safe to call even if cache_progress_update was never called. Call exactly
// once after the rebuild finishes (success or failure), before control
// returns to the engine.
void cache_progress_finish(void);

#endif
