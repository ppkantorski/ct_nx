/* movie_player.h -- FMV cutscene playback for the Chrono Trigger Switch port
 *
 * Copyright (C) 2026 NaGaa95 <https://github.com/NaGaa95>
 * Copyright (C) 2026 ppkantorski <https://github.com/ppkantorski>
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __MOVIE_PLAYER_H__
#define __MOVIE_PLAYER_H__

#include <EGL/egl.h>

// Play the named cutscene (e.g. "008.dat", an XOR-obfuscated MP4 in assets/).
// Blocking; returns when the clip ends or the player skips with A/B/+. Returns 0
// if the file was not found (caller should then just continue).
int movie_play(const char *name);

// Register cocos2d::GL::invalidateStateCache (must be resolved at boot, before
// so_finalize locks the symbol tables) so the engine re-applies its GL state
// cache after a movie.
void movie_set_gl_invalidate(void (*fn)(void));

// Re-present the last rendered frame for `frames` additional vsync cycles.
// Call after movie_play() returns AND after firing VIDEO_EVENT_COMPLETED so the
// engine's scene transition runs behind the last video frame instead of black.
void movie_hold_last_frame(int frames);

// Called from the main render loop in place of eglSwapBuffers when a post-play
// hold is pending. Presents the stashed last frame, drains the remaining hold
// count, then flushes GL state. Returns 1 if it handled the swap (caller must
// NOT call eglSwapBuffers); returns 0 if no hold is active (caller swaps normally).
int movie_post_render(EGLDisplay dpy, EGLSurface sfc);

// Returns 1 on the first call after a movie completes normally (not skipped),
// then resets to 0. main.c uses this to inject a synthetic BACK key event on
// the next frame so PlayMovieScene auto-advances without a real button press.
// (PlayMovieScene::update() auto-advances only when VirtualController reports
// a key-release, but our InvaildDevice stub always returns 0 for that query.)
int movie_take_completion_flag(void);

// Called from jni_fake.c's startVideo handler to arm the completion flag.
// (Separate from movie_hold_last_frame: hold can fire on skip too, but we only
// want to inject a BACK key when the video finished on its own — user-pressed
// skip already counts as the user's input and the scene advances via B.)
void movie_signal_completion(void);

#endif
