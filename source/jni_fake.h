/* jni_fake.h -- fake JNI environment for cocos2d-x 3.14.1 (libchrono.so)
 *
 * Chrono Trigger is a cocos2d-x 3.14.1 game. The engine talks to the Android
 * framework exclusively through JNI: it resolves Java classes via a cached
 * ClassLoader (JniHelper) and calls static methods on org.cocos2dx.lib.* and
 * org.cocos2dx.cpp.AppActivity. We provide a functional JNIEnv/JavaVM so those
 * calls resolve to native C implementations, and we drive the engine's own
 * native entry points (nativeInit/nativeRender/touches/keys) directly.
 *
 * Copyright (C) 2026 NaGaa95 <https://github.com/NaGaa95>
 * Copyright (C) 2026 ppkantorski <https://github.com/ppkantorski>
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

#include <stdint.h>

extern void *fake_vm;  // JavaVM *
extern void *fake_env; // JNIEnv *

// set when the engine asks the activity to quit (terminateProcess / finish)
extern volatile int jni_quit_requested;

void jni_init(void);

// set the directory reported to the engine as the writable / save path
void jni_set_writable_path(const char *p);

// constructors for fake Java objects (used by main.c to build call arguments)
void *jni_make_string(const char *utf);
void *jni_make_object(const char *label);
void *jni_new_byte_array(const void *data, int len);
void *jni_new_int_array(const int *data, int len);
void *jni_new_float_array(const float *data, int len);
void  jni_delete_ref(void *ref);

// The text-bitmap path (createTextBitmapShadowStroke) renders with FreeType and
// hands the pixels back to the engine through its exported nativeInitBitmapDC.
// main.c resolves that symbol and registers it here.
typedef void (*BitmapDCFn)(void *env, void *thiz, int w, int h, void *pixels);
void jni_set_bitmap_cb(BitmapDCFn fn);

// The video helper (Cocos2dxVideoHelper) reports playback events back to the
// engine through its exported nativeExecuteVideoCallback.
typedef void (*VideoCbFn)(void *env, void *thiz, int index, int event);
void jni_set_video_cb(VideoCbFn fn);

// On-screen keyboard (IME): cocos opens the soft keyboard and feeds typed text
// back through the engine's exported nativeInsertText / nativeDeleteBackward.
typedef void (*ImeInsertFn)(void *env, void *thiz, void *jstr_text);
typedef void (*ImeDeleteFn)(void *env, void *thiz);
void jni_set_ime_cb(ImeInsertFn insert, ImeDeleteFn del);

// cocos ui::EditBox (Cocos2dxEditBoxHelper): swkbd-backed text entry, reported
// back through the engine's editBoxEditing{DidBegin,Changed,DidEnd} natives.
typedef void (*EbBeginFn)(void *env, void *cls, int index);
typedef void (*EbTextFn)(void *env, void *cls, int index, void *jstr_text);
void jni_set_editbox_cb(EbBeginFn begin, EbTextFn changed, EbTextFn ended);

// Show the software keyboard for a pending openKeyboard request (called once per
// frame from the main loop; swkbd is a blocking system applet).
void jni_ime_service(void);

#endif
