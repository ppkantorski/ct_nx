/* jni_fake.c -- fake JNI environment for cocos2d-x 3.14.1 (libchrono.so)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * Implements a JNIEnv/JavaVM whose class lookups and method calls resolve to
 * native C, backing the cocos Cocos2dxHelper surface, the bitmap text
 * rasteriser, the IME and the video helper. UserDefault persists via prefs.c;
 * text renders via gfx.c.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "jni_fake.h"
#include "asset.h"
#include "gfx.h"
#include "prefs.h"
#include "movie_player.h"

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t juint;

// ---------------------------------------------------------------------------
// fake object model
// ---------------------------------------------------------------------------

enum {
  TAG_OBJECT = 0x4f424a31, // 'OBJ1'  heap object (freeable)
  TAG_STRING = 0x53545231, // 'STR1'
  TAG_OBJARR = 0x4f415231, // 'OAR1'
  TAG_PRIARR = 0x50415231, // 'PAR1'
  TAG_ID     = 0x4d494431, // 'MID1'  pooled, never freed
  TAG_CLASS  = 0x434c5331, // 'CLS1'  singleton, never freed
};

typedef struct { uint32_t tag; char label[96]; } FakeObject;
typedef struct { uint32_t tag; char *utf; } FakeString;
typedef struct { uint32_t tag; int len; void **items; } FakeObjArray;
typedef struct { uint32_t tag; int len; int elem_size; void *data; } FakePriArray;
typedef struct { uint32_t tag; char name[96]; char cls[96]; } FakeID;

volatile int jni_quit_requested = 0;

static BitmapDCFn g_bitmap_cb = NULL;
static VideoCbFn  g_video_cb = NULL;
static ImeInsertFn g_ime_insert = NULL;
static ImeDeleteFn g_ime_delete = NULL;
static EbBeginFn g_eb_begin = NULL;
static EbTextFn  g_eb_changed = NULL;
static EbTextFn  g_eb_ended = NULL;

void jni_set_bitmap_cb(BitmapDCFn fn) { g_bitmap_cb = fn; }
void jni_set_video_cb(VideoCbFn fn) { g_video_cb = fn; }
void jni_set_ime_cb(ImeInsertFn insert, ImeDeleteFn del) { g_ime_insert = insert; g_ime_delete = del; }
void jni_set_editbox_cb(EbBeginFn begin, EbTextFn changed, EbTextFn ended) {
  g_eb_begin = begin; g_eb_changed = changed; g_eb_ended = ended;
}

// ---------------------------------------------------------------------------
// local reference registry (native code must free the refs it creates)
// ---------------------------------------------------------------------------

#define MAX_LOCALS 32768
#define MAX_FRAMES 128
static void *locals[MAX_LOCALS];
static int locals_top = 0;
static int frames[MAX_FRAMES];
static int frame_top = 0;
static Mutex locals_lock;

static void *reg_local(void *ref) {
  if (ref) {
    mutexLock(&locals_lock);
    if (locals_top < MAX_LOCALS) locals[locals_top++] = ref;
    else debugPrintf("JNI: local ref table full, leaking %p\n", ref);
    mutexUnlock(&locals_lock);
  }
  return ref;
}

static void free_ref(void *ref) {
  if (!ref) return;
  switch (*(uint32_t *)ref) {
    case TAG_STRING: { FakeString *s = ref; free(s->utf); free(s); break; }
    case TAG_PRIARR: { FakePriArray *a = ref; free(a->data); free(a); break; }
    case TAG_OBJARR: { FakeObjArray *a = ref; free(a->items); free(a); break; }
    case TAG_OBJECT: free(ref); break;
    default: break; // TAG_ID / TAG_CLASS are pooled, never freed
  }
}

static void delete_local(void *ref) {
  if (!ref) return;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--) {
    if (locals[i] == ref) {
      locals[i] = locals[--locals_top];
      free_ref(ref);
      break;
    }
  }
  mutexUnlock(&locals_lock);
}

// ---------------------------------------------------------------------------
// constructors
// ---------------------------------------------------------------------------

void *jni_make_object(const char *label) {
  FakeObject *o = calloc(1, sizeof(*o));
  o->tag = TAG_OBJECT;
  if (label) snprintf(o->label, sizeof(o->label), "%s", label);
  return reg_local(o);
}

void *jni_make_string(const char *utf) {
  FakeString *s = calloc(1, sizeof(*s));
  s->tag = TAG_STRING;
  s->utf = strdup(utf ? utf : "");
  return reg_local(s);
}

static void *make_pri_array_adopt(void *data, int len, int elem_size) {
  FakePriArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_PRIARR;
  a->len = len;
  a->elem_size = elem_size;
  a->data = data;
  return reg_local(a);
}

void *jni_new_byte_array(const void *data, int len) {
  void *buf = malloc(len > 0 ? len : 1);
  if (data && len > 0) memcpy(buf, data, len);
  else if (len > 0) memset(buf, 0, len);
  return make_pri_array_adopt(buf, len, 1);
}
void *jni_new_int_array(const int *data, int len) {
  int *buf = malloc((len > 0 ? len : 1) * sizeof(int));
  if (data && len > 0) memcpy(buf, data, (size_t)len * sizeof(int));
  return make_pri_array_adopt(buf, len, 4);
}
void *jni_new_float_array(const float *data, int len) {
  float *buf = malloc((len > 0 ? len : 1) * sizeof(float));
  if (data && len > 0) memcpy(buf, data, (size_t)len * sizeof(float));
  return make_pri_array_adopt(buf, len, 4);
}
void jni_delete_ref(void *ref) { delete_local(ref); }

static const char *obj_str(void *jstr) {
  FakeString *s = jstr;
  if (s && s->tag == TAG_STRING) return s->utf;
  return "";
}

// ---------------------------------------------------------------------------
// class + classloader + method-id pools (never freed)
// ---------------------------------------------------------------------------

#define MAX_CLASSES 128
static FakeObject class_pool[MAX_CLASSES];
static int class_count = 0;

static void *get_class(const char *name) {
  if (!name) name = "";
  for (int i = 0; i < class_count; i++)
    if (!strcmp(class_pool[i].label, name))
      return &class_pool[i];
  if (class_count >= MAX_CLASSES) return &class_pool[0];
  FakeObject *c = &class_pool[class_count++];
  c->tag = TAG_CLASS;
  strncpy(c->label, name, sizeof(c->label) - 1);
  return c;
}

static FakeObject g_classloader = { TAG_CLASS, "__classloader__" };

#define MAX_IDS 512
static FakeID id_pool[MAX_IDS];
static int id_count = 0;

static FakeID *get_id(const char *cls, const char *name, const char *sig) {
  (void)sig;
  for (int i = 0; i < id_count; i++)
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].cls, cls ? cls : ""))
      return &id_pool[i];
  if (id_count >= MAX_IDS) { debugPrintf("JNI: id pool full (%s)\n", name); return &id_pool[0]; }
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  snprintf(id->name, sizeof(id->name), "%s", name);
  snprintf(id->cls, sizeof(id->cls), "%s", cls ? cls : "");
  return id;
}

// ---------------------------------------------------------------------------
// writable path + package + device identity
// ---------------------------------------------------------------------------

static char g_writable[512];

void jni_set_writable_path(const char *p); // defined at end of file

static const char *writable_dir(void) {
  if (!g_writable[0]) {
    if (!getcwd(g_writable, sizeof(g_writable)) || !g_writable[0])
      strcpy(g_writable, ".");
    size_t n = strlen(g_writable);
    if (n > 1 && g_writable[n - 1] == '/') g_writable[n - 1] = 0;
  }
  return g_writable;
}

// ---------------------------------------------------------------------------
// on-screen keyboard (IME)
// ---------------------------------------------------------------------------

static void open_ime(const char *initial) {
  if (!g_ime_insert)
    return;
  SwkbdConfig kbd;
  if (R_FAILED(swkbdCreate(&kbd, 0)))
    return;
  swkbdConfigMakePresetDefault(&kbd);
  if (initial && initial[0])
    swkbdConfigSetInitialText(&kbd, initial);
  swkbdConfigSetStringLenMax(&kbd, 64);
  char out[256] = {0};
  Result rc = swkbdShow(&kbd, out, sizeof(out));
  swkbdClose(&kbd);
  if (R_SUCCEEDED(rc)) {
    void *js = jni_make_string(out);
    g_ime_insert(fake_env, NULL, js);
    delete_local(js);
  }
}

// ---------------------------------------------------------------------------
// cocos ui::EditBox (Cocos2dxEditBoxHelper): track each field's text + max
// length; openKeyboard flags a request that jni_ime_service() services with
// swkbd, reporting the result through the editBoxEditing* natives.
// ---------------------------------------------------------------------------

#define MAX_EDITBOX 32

typedef struct {
  int used;
  char text[512];
  int maxlen;
} EditBox;

static EditBox g_editbox[MAX_EDITBOX];
static volatile int g_eb_open_req = -1; // index of a pending openKeyboard, or -1

static int editbox_create(void) {
  for (int i = 0; i < MAX_EDITBOX; i++) {
    if (!g_editbox[i].used) {
      memset(&g_editbox[i], 0, sizeof(g_editbox[i]));
      g_editbox[i].used = 1;
      return i;
    }
  }
  return 0; // table full: reuse slot 0 rather than handing back a bad index
}

static EditBox *editbox_get(int idx) {
  if (idx < 0 || idx >= MAX_EDITBOX || !g_editbox[idx].used)
    return NULL;
  return &g_editbox[idx];
}

void jni_ime_service(void) {
  const int idx = g_eb_open_req;
  if (idx < 0)
    return;
  g_eb_open_req = -1;

  EditBox *eb = editbox_get(idx);
  if (!eb)
    return;

  if (g_eb_begin)
    g_eb_begin(fake_env, NULL, idx);

  // On cancel we keep eb->text unchanged and report it back (a no-op edit).
  SwkbdConfig kbd;
  char out[1024] = {0};
  if (R_SUCCEEDED(swkbdCreate(&kbd, 0))) {
    swkbdConfigMakePresetDefault(&kbd);
    if (eb->text[0])
      swkbdConfigSetInitialText(&kbd, eb->text);
    swkbdConfigSetStringLenMax(&kbd, eb->maxlen > 0 ? (u32)eb->maxlen : 64);
    if (R_SUCCEEDED(swkbdShow(&kbd, out, sizeof(out))))
      snprintf(eb->text, sizeof(eb->text), "%s", out);
    swkbdClose(&kbd);
  }

  void *js = jni_make_string(eb->text);
  if (g_eb_changed)
    g_eb_changed(fake_env, NULL, idx, js);
  if (g_eb_ended)
    g_eb_ended(fake_env, NULL, idx, js);
  delete_local(js);
}

// ---------------------------------------------------------------------------
// text encoding conversion (best effort: UTF-8 <-> UTF-16LE, else passthrough)
// ---------------------------------------------------------------------------

static int eq_ci(const char *a, const char *b) {
  for (; *a && *b; a++, b++) {
    int ca = *a, cb = *b;
    if (ca >= 'a' && ca <= 'z') ca -= 32;
    if (cb >= 'a' && cb <= 'z') cb -= 32;
    if (ca != cb) return 0;
  }
  return *a == *b;
}
static int is_utf16(const char *e) {
  return eq_ci(e, "UTF-16") || eq_ci(e, "UTF-16LE") || eq_ci(e, "UTF16") || eq_ci(e, "UnicodeLittle");
}
static int is_utf8(const char *e) { return eq_ci(e, "UTF-8") || eq_ci(e, "UTF8"); }

static void *conversion_encoding(FakePriArray *in, const char *from, const char *to) {
  if (!in || in->tag != TAG_PRIARR) return jni_new_byte_array(NULL, 0);
  const unsigned char *src = in->data;
  const int n = in->len;

  if (eq_ci(from, to) || (is_utf8(from) && is_utf8(to)))
    return jni_new_byte_array(src, n); // passthrough

  if (is_utf8(from) && is_utf16(to)) { // UTF-8 -> UTF-16LE
    unsigned char *out = malloc((size_t)n * 2 + 2);
    int o = 0, i = 0;
    while (i < n) {
      uint32_t c = src[i++];
      if (c >= 0xF0 && i + 2 < n) { c = ((c & 7) << 18) | ((src[i] & 0x3F) << 12) | ((src[i+1] & 0x3F) << 6) | (src[i+2] & 0x3F); i += 3; }
      else if (c >= 0xE0 && i + 1 < n) { c = ((c & 0xF) << 12) | ((src[i] & 0x3F) << 6) | (src[i+1] & 0x3F); i += 2; }
      else if (c >= 0xC0 && i < n) { c = ((c & 0x1F) << 6) | (src[i] & 0x3F); i += 1; }
      if (c < 0x10000) { out[o++] = c & 0xFF; out[o++] = (c >> 8) & 0xFF; }
      else { c -= 0x10000; uint16_t hi = 0xD800 + (c >> 10), lo = 0xDC00 + (c & 0x3FF);
             out[o++] = hi & 0xFF; out[o++] = hi >> 8; out[o++] = lo & 0xFF; out[o++] = lo >> 8; }
    }
    void *r = jni_new_byte_array(out, o); free(out); return r;
  }
  if (is_utf16(from) && is_utf8(to)) {
    unsigned char *out = malloc((size_t)n * 2 + 4);
    int o = 0;
    for (int i = 0; i + 1 < n; i += 2) {
      uint32_t c = src[i] | (src[i+1] << 8);
      if (c >= 0xD800 && c <= 0xDBFF && i + 3 < n) {
        uint32_t lo = src[i+2] | (src[i+3] << 8); i += 2;
        c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
      }
      if (c < 0x80) out[o++] = c;
      else if (c < 0x800) { out[o++] = 0xC0 | (c >> 6); out[o++] = 0x80 | (c & 0x3F); }
      else if (c < 0x10000) { out[o++] = 0xE0 | (c >> 12); out[o++] = 0x80 | ((c >> 6) & 0x3F); out[o++] = 0x80 | (c & 0x3F); }
      else { out[o++] = 0xF0 | (c >> 18); out[o++] = 0x80 | ((c >> 12) & 0x3F); out[o++] = 0x80 | ((c >> 6) & 0x3F); out[o++] = 0x80 | (c & 0x3F); }
    }
    void *r = jni_new_byte_array(out, o); free(out); return r;
  }
  return jni_new_byte_array(src, n); // unknown pair: passthrough
}

// ---------------------------------------------------------------------------
// text bitmap (createTextBitmapShadowStroke): render and feed nativeInitBitmapDC
// signature ([BLjava/lang/String;IIIIIIIIZFFFFZIIIIFZI)Z
// ---------------------------------------------------------------------------

static int create_text_bitmap(va_list va) {
  FakePriArray *text = va_arg(va, void *);      // 1: byte[] utf-8
  void *font = va_arg(va, void *);              // 2: String fontName
  const char *font_name = obj_str(font);        // (logged under DEBUG_INSTR)
  (void)font_name;
  int fontSize = va_arg(va, int);               // 3
  int r = va_arg(va, int);                      // 4 fill R
  int g = va_arg(va, int);                      // 5 fill G
  int b = va_arg(va, int);                      // 6 fill B
  int a = va_arg(va, int);                      // 7 fill A
  int align = va_arg(va, int);                  // 8 alignment (low nibble = horiz,
                                                //   high nibble = vert)
  int width = va_arg(va, int);                  // 9 constraint width (0 = auto)
  int height = va_arg(va, int);                 // 10 constraint height (0 = auto)
  // 11..23 shadow/stroke/wrap/overflow; jboolean->int, jfloat->double in varargs
  int    shadow        = va_arg(va, int);     // 11
  double shadowDX      = va_arg(va, double);  // 12
  double shadowDY      = va_arg(va, double);  // 13
  double shadowBlur    = va_arg(va, double);  // 14
  double shadowOpacity = va_arg(va, double);  // 15
  int    stroke        = va_arg(va, int);     // 16
  int    strokeR       = va_arg(va, int);     // 17
  int    strokeG       = va_arg(va, int);     // 18
  int    strokeB       = va_arg(va, int);     // 19
  int    strokeA       = va_arg(va, int);     // 20
  double strokeSize    = va_arg(va, double);  // 21
  int wrap = va_arg(va, int);                 // 22 enableWrap
  (void)va_arg(va, int);                      // 23 overflow
#if !DEBUG_INSTR
  (void)shadow; (void)shadowDX; (void)shadowDY; (void)shadowBlur; (void)shadowOpacity;
  (void)stroke; (void)strokeR; (void)strokeG; (void)strokeB; (void)strokeA; (void)strokeSize;
#endif

  if (!g_bitmap_cb || !text || text->tag != TAG_PRIARR)
    return 0;

  // byte[] is UTF-8 text without a NUL; copy and terminate
  char *str = malloc((size_t)text->len + 1);
  if (!str) return 0;
  memcpy(str, text->data, text->len);
  str[text->len] = 0;

#if DEBUG_INSTR
  // log how the engine lays out this text so we can see the input field's real
  // alignment/box AND the exact fill colour + shadow/stroke it asks for (grep the
  // option text in textdbg.log; rgba= is the fill colour the engine requested).
  {
    FILE *tl = fopen("textdbg.log", "a");
    if (tl) {
      fprintf(tl, "text='%s' font='%s' size=%d rgba=%d,%d,%d,%d alignH=%d alignV=%d "
                  "boxW=%d boxH=%d wrap=%d shadow=%d dx=%.1f dy=%.1f blur=%.1f op=%.2f "
                  "stroke=%d srgba=%d,%d,%d,%d ssize=%.1f\n",
              str, font_name ? font_name : "", fontSize, r, g, b, a,
              align & 0x0F, (align >> 4) & 0x0F, width, height, wrap,
              shadow, shadowDX, shadowDY, shadowBlur, shadowOpacity,
              stroke, strokeR, strokeG, strokeB, strokeA, strokeSize);
      fclose(tl);
    }
  }
#endif

  int w = 0, h = 0;
  unsigned char *rgba = gfx_render_text_rgba(str, fontSize, r, g, b, a,
                                             align & 0x0F, (align >> 4) & 0x0F,
                                             width, height, wrap, &w, &h);
  free(str);
  if (!rgba) return 0;

  // premultiplied RGBA8888 to nativeInitBitmapDC via a transient byte[];
  // the engine copies it out with GetByteArrayRegion.
  FakePriArray arr = { TAG_PRIARR, w * h * 4, 1, rgba };
  g_bitmap_cb(fake_env, NULL, w, h, &arr);
  free(rgba);
  return 1;
}

// ---------------------------------------------------------------------------
// video helper (Cocos2dxVideoHelper): the engine's cutscenes are the obfuscated
// MP4s in assets/ (001.dat..008.dat). setVideoUrl gives us the clip name;
// startVideo plays it (blocking, like the keyboard) via movie_player, then we
// report COMPLETED so the engine's PlayMovieScene continues.
// ---------------------------------------------------------------------------

#define VIDEO_EVENT_COMPLETED 3
static int g_video_next_index = 0;
static char g_video_url[512];

static int video_dispatch_int(const char *name) {
  if (!strcmp(name, "createVideoWidget"))
    return g_video_next_index++;
  return 0;
}

static void video_dispatch_void(const char *name, va_list va) {
  if (!strcmp(name, "setVideoUrl")) {
    (void)va_arg(va, int);                       // index
    (void)va_arg(va, int);                       // videoSource (resource/url)
    const char *url = obj_str(va_arg(va, void *)); // clip path, e.g. "008.dat"
    snprintf(g_video_url, sizeof(g_video_url), "%s", url ? url : "");
    return;
  }
  if (!strcmp(name, "startVideo")) {
    int idx = va_arg(va, int);
    if (g_video_url[0])
      movie_play(g_video_url); // blocking: runs to clip end or skip (A/B/+)
    if (g_video_cb) g_video_cb(fake_env, NULL, idx, VIDEO_EVENT_COMPLETED);
    return;
  }
  (void)va; // removeVideoWidget/setVideoRect/seek/visible/etc: no-op
}

// ---------------------------------------------------------------------------
// method dispatch (by Java method name)
// ---------------------------------------------------------------------------

static juint call_int(const char *name, va_list va) {
  if (!strcmp(name, "getDPI")) return 160;
  if (!strcmp(name, "getDeviceMaxAudioInstances")) return 24;
  if (!strcmp(name, "getIntegerForKey")) {
    const char *k = obj_str(va_arg(va, void *));
    int d = va_arg(va, int);
    int v = prefs_get_int(k, d);
    debugPrintf("prefs: getInt(%s) = %d (def %d)\n", k, v, d);
    return (juint)v;
  }
  if (!strcmp(name, "createVideoWidget")) return (juint)video_dispatch_int(name);
  if (!strcmp(name, "createEditBox")) return (juint)editbox_create();
  (void)va;
  return 0;
}

static juint call_bool(const char *name, va_list va) {
  if (!strcmp(name, "getBoolForKey")) {
    const char *k = obj_str(va_arg(va, void *));
    int d = va_arg(va, int);
    int v = prefs_get_bool(k, d);
    debugPrintf("prefs: getBool(%s) = %d (def %d)\n", k, v, d);
    return (juint)v;
  }
  if (!strcmp(name, "createTextBitmapShadowStroke"))
    return (juint)create_text_bitmap(va);
  if (!strcmp(name, "openURL")) return 0;
  if (!strcmp(name, "getBoolForKeyJNI")) {
    const char *k = obj_str(va_arg(va, void *));
    int d = va_arg(va, int);
    return (juint)prefs_get_bool(k, d);
  }
  (void)va;
  return 0;
}

static float call_float(const char *name, va_list va) {
  if (!strcmp(name, "getFloatForKey")) {
    const char *k = obj_str(va_arg(va, void *));
    double d = va_arg(va, double); // jfloat promoted in varargs
    return prefs_get_float(k, (float)d);
  }
  (void)va;
  return 0.0f;
}

static double call_double(const char *name, va_list va) {
  if (!strcmp(name, "getDoubleForKey")) {
    const char *k = obj_str(va_arg(va, void *));
    double d = va_arg(va, double);
    return (double)prefs_get_float(k, (float)d); // android stores doubles as float
  }
  (void)va;
  return 0.0;
}

static juint call_long(const char *name, va_list va) {
  (void)name; (void)va;
  return 0;
}

static void *call_object(const char *name, va_list va) {
  if (!strcmp(name, "getCurrentLanguage") || !strcmp(name, "getCurrentLanguageCode") ||
      !strcmp(name, "getLanguage") || !strcmp(name, "getDeviceLanguage")) {
    const char *lang = config.language[0] ? config.language : LANG_DEFAULT;
    debugPrintf("jni: %s -> \"%s\"\n", name, lang);
    return jni_make_string(lang);
  }
  // java.util.Locale path: some engine code reads the locale directly
  if (!strcmp(name, "getDefault")) return jni_make_object("Locale");
  if (!strcmp(name, "getCountry") || !strcmp(name, "getDeviceCountry"))
    return jni_make_string("US");
  if (!strcmp(name, "getCocos2dxWritablePath") || !strcmp(name, "getWritablePath"))
    return jni_make_string(writable_dir());
  if (!strcmp(name, "getCocos2dxPackageName") || !strcmp(name, "getPackageName"))
    return jni_make_string("com.square_enix.android_googleplay.chrono");
  if (!strcmp(name, "getDeviceModel")) return jni_make_string("Nintendo Switch");
  if (!strcmp(name, "getVersion")) return jni_make_string("2.1.5");
  if (!strcmp(name, "getStringForKey")) {
    const char *k = obj_str(va_arg(va, void *));
    const char *d = obj_str(va_arg(va, void *));
    const char *v = prefs_get_string(k, d);
    debugPrintf("prefs: getString(%s) = \"%s\" (def \"%s\")\n", k, v, d);
    return jni_make_string(v);
  }
  if (!strcmp(name, "conversionEncoding")) {
    FakePriArray *in = va_arg(va, void *);
    const char *from = obj_str(va_arg(va, void *));
    const char *to = obj_str(va_arg(va, void *));
    return conversion_encoding(in, from, to);
  }
  // (String)->long[] asset fd probe, returning { fd, offset, length }
  if (!strcmp(name, "getObbAssetFileDescriptor") ||
      !strcmp(name, "getAssetsFileDescriptor") ||
      !strcmp(name, "getAssetFileDescriptor")) {
    const char *path = obj_str(va_arg(va, void *));
    int64_t off = 0, len = 0;
    int fd = asset_open_fd(path, &off, &len);
    int64_t v[3] = { fd, off, len };
    int64_t *buf = malloc(sizeof(v));
    memcpy(buf, v, sizeof(v));
    return make_pri_array_adopt(buf, 3, 8);
  }
  if (!strcmp(name, "getEditBoxText") || !strcmp(name, "getContentText"))
    return jni_make_string("");
  (void)va;
  return NULL;
}

static void call_void(const char *name, va_list va) {
  // UserDefault writers
  if (!strcmp(name, "setBoolForKey"))   { const char *k = obj_str(va_arg(va, void *)); int v = va_arg(va, int); prefs_set_bool(k, v); return; }
  if (!strcmp(name, "setIntegerForKey")){ const char *k = obj_str(va_arg(va, void *)); int v = va_arg(va, int); prefs_set_int(k, v); return; }
  if (!strcmp(name, "setFloatForKey"))  { const char *k = obj_str(va_arg(va, void *)); double v = va_arg(va, double); prefs_set_float(k, (float)v); return; }
  if (!strcmp(name, "setDoubleForKey")) { const char *k = obj_str(va_arg(va, void *)); double v = va_arg(va, double); prefs_set_float(k, (float)v); return; }
  if (!strcmp(name, "setStringForKey")) { const char *k = obj_str(va_arg(va, void *)); const char *v = obj_str(va_arg(va, void *)); prefs_set_string(k, v); return; }
  if (!strcmp(name, "deleteValueForKey")) { const char *k = obj_str(va_arg(va, void *)); prefs_delete(k); return; }

  // IME / edit box
  if (!strcmp(name, "openIMEKeyboard")) { open_ime(NULL); return; }
  if (!strcmp(name, "closeIMEKeyboard")) { return; }

  // cocos ui::EditBox: track text + length + the open request; the geometry/
  // font/visibility setters fall through to the no-op default below.
  if (!strcmp(name, "setText")) {
    int idx = va_arg(va, int);
    const char *t = obj_str(va_arg(va, void *));
    EditBox *eb = editbox_get(idx);
    if (eb) { strncpy(eb->text, t ? t : "", sizeof(eb->text) - 1); eb->text[sizeof(eb->text)-1] = 0; }
    return;
  }
  if (!strcmp(name, "setMaxLength")) {
    int idx = va_arg(va, int);
    int max = va_arg(va, int);
    EditBox *eb = editbox_get(idx);
    if (eb) eb->maxlen = max;
    return;
  }
  if (!strcmp(name, "openKeyboard")) {
    g_eb_open_req = va_arg(va, int); // serviced next frame by jni_ime_service()
    return;
  }
  if (!strcmp(name, "removeEditBox")) {
    int idx = va_arg(va, int);
    EditBox *eb = editbox_get(idx);
    if (eb) eb->used = 0;
    return;
  }

  // device niceties -- safe to ignore on Switch
  if (!strcmp(name, "enableAccelerometer") || !strcmp(name, "disableAccelerometer") ||
      !strcmp(name, "setAccelerometerInterval") || !strcmp(name, "setKeepScreenOn") ||
      !strcmp(name, "setAnimationInterval") || !strcmp(name, "vibrate") ||
      !strcmp(name, "setEnableAudioFocus") || !strcmp(name, "preloadBackgroundMusic") ||
      !strcmp(name, "showDialog") || !strcmp(name, "setResourceManagerListener"))
    return;

  if (!strcmp(name, "terminateProcess") || !strcmp(name, "finish") || !strcmp(name, "exitGame")) {
    jni_quit_requested = 1;
    return;
  }

  // video helper
  if (!strcmp(name, "startVideo") || !strcmp(name, "removeVideoWidget") ||
      !strcmp(name, "setVideoUrl") || !strcmp(name, "setVideoRect") ||
      !strcmp(name, "setFullScreenEnabled") || !strcmp(name, "setVideoKeepRatioEnabled") ||
      !strcmp(name, "pauseVideo") || !strcmp(name, "resumeVideo") ||
      !strcmp(name, "stopVideo") || !strcmp(name, "seekVideoTo") ||
      !strcmp(name, "setVideoVisible")) {
    video_dispatch_void(name, va);
    return;
  }
  (void)va;
}

// ---------------------------------------------------------------------------
// JNIEnv function implementations
// ---------------------------------------------------------------------------

static juint j_GetVersion(void *env) { (void)env; return JNI_VERSION_1_6; }
static void *j_FindClass(void *env, const char *name) { (void)env; return get_class(name); }
static void *j_GetObjectClass(void *env, void *obj) { (void)env; (void)obj; return get_class("java/lang/Object"); }
static juint j_IsInstanceOf(void *env, void *obj, void *cls) { (void)env; (void)obj; (void)cls; return 1; }

static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env;
  FakeObject *c = cls;
  return get_id(c && c->tag == TAG_CLASS ? c->label : "", name, sig);
}

static void *j_NewGlobalRef(void *env, void *obj) {
  (void)env;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--)
    if (locals[i] == obj) { locals[i] = locals[--locals_top]; break; }
  mutexUnlock(&locals_lock);
  return obj;
}
static void j_DeleteGlobalRef(void *env, void *obj) { (void)env; free_ref(obj); }
static void j_DeleteLocalRef(void *env, void *obj) { (void)env; delete_local(obj); }
static void *j_NewLocalRef(void *env, void *obj) { (void)env; return obj; }
static juint j_IsSameObject(void *env, void *a, void *b) { (void)env; return a == b; }
static juint j_EnsureLocalCapacity(void *env, int cap) { (void)env; (void)cap; return 0; }

static juint j_PushLocalFrame(void *env, int cap) {
  (void)env; (void)cap;
  mutexLock(&locals_lock);
  if (frame_top < MAX_FRAMES) frames[frame_top++] = locals_top;
  mutexUnlock(&locals_lock);
  return 0;
}
static void *j_PopLocalFrame(void *env, void *result) {
  (void)env;
  mutexLock(&locals_lock);
  const int mark = frame_top > 0 ? frames[--frame_top] : 0;
  for (int i = mark; i < locals_top; i++)
    if (locals[i] != result) free_ref(locals[i]);
  locals_top = mark;
  if (result && locals_top < MAX_LOCALS) locals[locals_top++] = result;
  mutexUnlock(&locals_lock);
  return result;
}

// --- Call<type>Method (instance): ClassLoader specials, then shared dispatch --

static void *call_object_instance(void *recv, FakeID *id, va_list va) {
  if (!strcmp(id->name, "loadClass")) {
    void *jstr = va_arg(va, void *);
    return get_class(obj_str(jstr));
  }
  if (!strcmp(id->name, "getClassLoader"))
    return &g_classloader;
  (void)recv;
  return call_object(id->name, va);
}

#define CALL_INST(fn, ret_t, dispatch) \
  static ret_t fn(void *env, void *recv, FakeID *id, ...) { \
    (void)env; va_list va; va_start(va, id); ret_t r = dispatch; va_end(va); return r; } \
  static ret_t fn##V(void *env, void *recv, FakeID *id, va_list va) { \
    (void)env; return dispatch; }

CALL_INST(j_CallObjectMethod, void *, call_object_instance(recv, id, va))
CALL_INST(j_CallIntMethod, juint, call_int(id->name, va))
CALL_INST(j_CallBooleanMethod, juint, call_bool(id->name, va))
CALL_INST(j_CallLongMethod, juint, call_long(id->name, va))
CALL_INST(j_CallFloatMethod, float, call_float(id->name, va))
CALL_INST(j_CallDoubleMethod, double, call_double(id->name, va))

static void j_CallVoidMethod(void *env, void *recv, FakeID *id, ...) {
  (void)env; (void)recv; va_list va; va_start(va, id); call_void(id->name, va); va_end(va);
}
static void j_CallVoidMethodV(void *env, void *recv, FakeID *id, va_list va) {
  (void)env; (void)recv; call_void(id->name, va);
}

// --- CallStatic<type>Method (cocos uses these for everything) ----------------

#define CALL_STATIC(fn, ret_t, dispatch) \
  static ret_t fn(void *env, void *cls, FakeID *id, ...) { \
    (void)env; (void)cls; va_list va; va_start(va, id); ret_t r = dispatch; va_end(va); return r; } \
  static ret_t fn##V(void *env, void *cls, FakeID *id, va_list va) { \
    (void)env; (void)cls; return dispatch; }

CALL_STATIC(j_CallStaticObjectMethod, void *, call_object(id->name, va))
CALL_STATIC(j_CallStaticIntMethod, juint, call_int(id->name, va))
CALL_STATIC(j_CallStaticBooleanMethod, juint, call_bool(id->name, va))
CALL_STATIC(j_CallStaticLongMethod, juint, call_long(id->name, va))
CALL_STATIC(j_CallStaticFloatMethod, float, call_float(id->name, va))
CALL_STATIC(j_CallStaticDoubleMethod, double, call_double(id->name, va))

static void j_CallStaticVoidMethod(void *env, void *cls, FakeID *id, ...) {
  (void)env; (void)cls; va_list va; va_start(va, id); call_void(id->name, va); va_end(va);
}
static void j_CallStaticVoidMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls; call_void(id->name, va);
}

// --- strings ----------------------------------------------------------------
// cocos StringUtils::getStringUTFCharsJNI(), the hot path for every jstring the
// engine reads, uses the UTF-16 GetStringChars/GetStringLength/ReleaseStringChars
// trio rather than GetStringUTFChars. Our fake strings are UTF-8, so we convert.

// UTF-8 -> freshly malloc'd, NUL-terminated UTF-16; *out_units = code-unit count
static uint16_t *jni_u8_to_u16(const char *s, size_t *out_units) {
  const size_t n = strlen(s);
  uint16_t *out = malloc((n + 1) * sizeof(uint16_t)); // units <= bytes
  if (!out) { if (out_units) *out_units = 0; return NULL; }
  size_t u = 0;
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    uint32_t c = *p++;
    if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = ((c & 7) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
    else if (c >= 0xE0 && p[0] && p[1]) { c = ((c & 0xF) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
    else if (c >= 0xC0 && p[0]) { c = ((c & 0x1F) << 6) | (p[0] & 0x3F); p += 1; }
    if (c < 0x10000) out[u++] = (uint16_t)c;
    else { c -= 0x10000; out[u++] = (uint16_t)(0xD800 + (c >> 10)); out[u++] = (uint16_t)(0xDC00 + (c & 0x3FF)); }
  }
  out[u] = 0;
  if (out_units) *out_units = u;
  return out;
}

static void *j_NewStringUTF(void *env, const char *utf) { (void)env; return jni_make_string(utf); }
static const char *j_GetStringUTFChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0; return obj_str(jstr);
}
static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *utf) { (void)env; (void)jstr; (void)utf; }
static juint j_GetStringUTFLength(void *env, void *jstr) { (void)env; return strlen(obj_str(jstr)); }

static juint j_GetStringLength(void *env, void *jstr) {
  (void)env;
  size_t u = 0;
  uint16_t *t = jni_u8_to_u16(obj_str(jstr), &u);
  free(t);
  return (juint)u;
}
static const uint16_t *j_GetStringChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env;
  if (is_copy) *is_copy = 1; // always a fresh copy; freed by ReleaseStringChars
  return jni_u8_to_u16(obj_str(jstr), NULL);
}
static void j_ReleaseStringChars(void *env, void *jstr, const uint16_t *chars) {
  (void)env; (void)jstr; free((void *)chars);
}
static void *j_NewString(void *env, const uint16_t *chars, int len) {
  (void)env;
  if (!chars || len < 0) return jni_make_string("");
  char *utf8 = malloc((size_t)len * 4 + 1);
  if (!utf8) return jni_make_string("");
  size_t o = 0;
  for (int i = 0; i < len; i++) {
    uint32_t c = chars[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < len) { uint32_t lo = chars[++i]; c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); }
    if (c < 0x80) utf8[o++] = (char)c;
    else if (c < 0x800) { utf8[o++] = 0xC0 | (c >> 6); utf8[o++] = 0x80 | (c & 0x3F); }
    else if (c < 0x10000) { utf8[o++] = 0xE0 | (c >> 12); utf8[o++] = 0x80 | ((c >> 6) & 0x3F); utf8[o++] = 0x80 | (c & 0x3F); }
    else { utf8[o++] = 0xF0 | (c >> 18); utf8[o++] = 0x80 | ((c >> 12) & 0x3F); utf8[o++] = 0x80 | ((c >> 6) & 0x3F); utf8[o++] = 0x80 | (c & 0x3F); }
  }
  utf8[o] = 0;
  void *r = jni_make_string(utf8);
  free(utf8);
  return r;
}
static void j_GetStringUTFRegion(void *env, void *jstr, int start, int len, char *buf) {
  (void)env;
  const char *s = obj_str(jstr);
  int sl = (int)strlen(s);
  if (start < 0 || start > sl) return;
  if (start + len > sl) len = sl - start;
  memcpy(buf, s + start, len > 0 ? len : 0);
}
static void j_GetStringRegion(void *env, void *jstr, int start, int len, uint16_t *buf) {
  (void)env;
  size_t u = 0;
  uint16_t *t = jni_u8_to_u16(obj_str(jstr), &u);
  if (!t) return;
  if (start >= 0 && len > 0 && (size_t)(start + len) <= u)
    memcpy(buf, t + start, (size_t)len * sizeof(uint16_t));
  free(t);
}

// --- arrays -----------------------------------------------------------------

static juint j_GetArrayLength(void *env, void *arr) {
  (void)env;
  FakeObjArray *a = arr; // len at same offset in FakeObjArray/FakePriArray
  if (a && (a->tag == TAG_PRIARR || a->tag == TAG_OBJARR)) return a->len;
  return 0;
}

static void *new_pri_array(int len, int elem_size) {
  void *data = calloc(len > 0 ? len : 1, elem_size);
  return make_pri_array_adopt(data, len, elem_size);
}
static void *j_NewByteArray(void *env, int len) { (void)env; return new_pri_array(len, 1); }
static void *j_NewIntArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }
static void *j_NewFloatArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }
static void *j_NewLongArray(void *env, int len) { (void)env; return new_pri_array(len, 8); }

static void *j_GetPriArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0;
  FakePriArray *a = arr;
  return (a && a->tag == TAG_PRIARR) ? a->data : NULL;
}
static void j_ReleasePriArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}
static void j_GetPriArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy(buf, (char *)a->data + (size_t)start * a->elem_size, (size_t)len * a->elem_size);
}
static void j_SetPriArrayRegion(void *env, void *arr, int start, int len, const void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy((char *)a->data + (size_t)start * a->elem_size, buf, (size_t)len * a->elem_size);
}

static void *j_NewObjectArray(void *env, int len, void *cls, void *init) {
  (void)env; (void)cls;
  FakeObjArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_OBJARR;
  a->len = len;
  a->items = calloc(len > 0 ? len : 1, sizeof(void *));
  for (int i = 0; i < len; i++) a->items[i] = init;
  return reg_local(a);
}
static void *j_GetObjectArrayElement(void *env, void *arr, int idx) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && idx >= 0 && idx < a->len) return a->items[idx];
  return NULL;
}
static void j_SetObjectArrayElement(void *env, void *arr, int idx, void *val) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && idx >= 0 && idx < a->len) a->items[idx] = val;
}

// --- misc -------------------------------------------------------------------

static juint j_RegisterNatives(void *env, void *cls, void *methods, int n) {
  (void)env; (void)cls; (void)methods; (void)n; return 0;
}
static juint j_GetJavaVM(void *env, void **vm) { (void)env; *vm = fake_vm; return JNI_OK; }
static juint j_ExceptionCheck(void *env) { (void)env; return 0; }
static void *j_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void *j_NewObject(void *env, void *cls, FakeID *id, ...) { (void)env; (void)id; FakeObject *c = cls; return jni_make_object(c ? c->label : "obj"); }
static void j_void1(void *env) { (void)env; }
static juint j_unimplemented(void) {
  debugPrintf("JNI: unimplemented slot called\n");
  return 0;
}

// ---------------------------------------------------------------------------
// table assembly (indices per the JNI specification)
// ---------------------------------------------------------------------------

static void *env_table[233];
static void **env_table_ptr = env_table;
void *fake_env = &env_table_ptr;

static juint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm; (void)args; if (env) *env = fake_env; return JNI_OK;
}
static juint vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static juint vm_GetEnv(void *vm, void **env, int version) {
  (void)vm; (void)version; if (env) *env = fake_env; return JNI_OK;
}
static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  mutexInit(&locals_lock);

  for (int i = 0; i < 233; i++) env_table[i] = (void *)j_unimplemented;

  env_table[4]   = (void *)j_GetVersion;
  env_table[6]   = (void *)j_FindClass;
  env_table[13]  = (void *)j_void1; // Throw
  env_table[14]  = (void *)j_void1; // ThrowNew
  env_table[15]  = (void *)j_ExceptionOccurred;
  env_table[16]  = (void *)j_void1; // ExceptionDescribe
  env_table[17]  = (void *)j_void1; // ExceptionClear
  env_table[18]  = (void *)j_void1; // FatalError
  env_table[19]  = (void *)j_PushLocalFrame;
  env_table[20]  = (void *)j_PopLocalFrame;
  env_table[21]  = (void *)j_NewGlobalRef;
  env_table[22]  = (void *)j_DeleteGlobalRef;
  env_table[23]  = (void *)j_DeleteLocalRef;
  env_table[24]  = (void *)j_IsSameObject;
  env_table[25]  = (void *)j_NewLocalRef;
  env_table[26]  = (void *)j_EnsureLocalCapacity;
  env_table[27]  = (void *)jni_make_object;  // AllocObject (best effort)
  env_table[28]  = (void *)j_NewObject;
  env_table[29]  = (void *)j_NewObject;      // NewObjectV
  env_table[31]  = (void *)j_GetObjectClass;
  env_table[32]  = (void *)j_IsInstanceOf;
  env_table[33]  = (void *)j_GetMethodID;
  env_table[34]  = (void *)j_CallObjectMethod;
  env_table[35]  = (void *)j_CallObjectMethodV;
  env_table[37]  = (void *)j_CallBooleanMethod;
  env_table[38]  = (void *)j_CallBooleanMethodV;
  env_table[40]  = (void *)j_CallIntMethod;  // CallByteMethod (treat as int)
  env_table[41]  = (void *)j_CallIntMethodV;
  env_table[49]  = (void *)j_CallIntMethod;
  env_table[50]  = (void *)j_CallIntMethodV;
  env_table[52]  = (void *)j_CallLongMethod;
  env_table[53]  = (void *)j_CallLongMethodV;
  env_table[55]  = (void *)j_CallFloatMethod;
  env_table[56]  = (void *)j_CallFloatMethodV;
  env_table[58]  = (void *)j_CallDoubleMethod;
  env_table[59]  = (void *)j_CallDoubleMethodV;
  env_table[61]  = (void *)j_CallVoidMethod;
  env_table[62]  = (void *)j_CallVoidMethodV;
  env_table[113] = (void *)j_GetMethodID;            // GetStaticMethodID
  env_table[114] = (void *)j_CallStaticObjectMethod;
  env_table[115] = (void *)j_CallStaticObjectMethodV;
  env_table[117] = (void *)j_CallStaticBooleanMethod;
  env_table[118] = (void *)j_CallStaticBooleanMethodV;
  env_table[120] = (void *)j_CallStaticIntMethod;    // CallStaticByteMethod
  env_table[121] = (void *)j_CallStaticIntMethodV;
  env_table[129] = (void *)j_CallStaticIntMethod;
  env_table[130] = (void *)j_CallStaticIntMethodV;
  env_table[132] = (void *)j_CallStaticLongMethod;
  env_table[133] = (void *)j_CallStaticLongMethodV;
  env_table[135] = (void *)j_CallStaticFloatMethod;
  env_table[136] = (void *)j_CallStaticFloatMethodV;
  env_table[138] = (void *)j_CallStaticDoubleMethod;
  env_table[139] = (void *)j_CallStaticDoubleMethodV;
  env_table[141] = (void *)j_CallStaticVoidMethod;
  env_table[142] = (void *)j_CallStaticVoidMethodV;
  env_table[144] = (void *)j_GetMethodID;            // GetStaticFieldID
  env_table[163] = (void *)j_NewString;              // NewString (UTF-16)
  env_table[164] = (void *)j_GetStringLength;
  env_table[165] = (void *)j_GetStringChars;
  env_table[166] = (void *)j_ReleaseStringChars;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[172] = (void *)j_NewObjectArray;
  env_table[173] = (void *)j_GetObjectArrayElement;
  env_table[174] = (void *)j_SetObjectArrayElement;
  env_table[176] = (void *)j_NewByteArray;
  env_table[179] = (void *)j_NewIntArray;
  env_table[180] = (void *)j_NewLongArray;
  env_table[181] = (void *)j_NewFloatArray;
  for (int i = 183; i <= 190; i++) env_table[i] = (void *)j_GetPriArrayElements;
  for (int i = 191; i <= 198; i++) env_table[i] = (void *)j_ReleasePriArrayElements;
  for (int i = 199; i <= 206; i++) env_table[i] = (void *)j_GetPriArrayRegion;
  for (int i = 207; i <= 214; i++) env_table[i] = (void *)j_SetPriArrayRegion;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[219] = (void *)j_GetJavaVM;
  env_table[220] = (void *)j_GetStringRegion;
  env_table[221] = (void *)j_GetStringUTFRegion;
  env_table[224] = (void *)j_GetStringChars;          // GetStringCritical
  env_table[225] = (void *)j_ReleaseStringChars;      // ReleaseStringCritical
  env_table[222] = (void *)j_GetPriArrayElements;     // GetPrimitiveArrayCritical
  env_table[223] = (void *)j_ReleasePriArrayElements; // ReleasePrimitiveArrayCritical
  env_table[226] = (void *)j_NewGlobalRef;            // NewWeakGlobalRef
  env_table[227] = (void *)j_DeleteGlobalRef;         // DeleteWeakGlobalRef
  env_table[228] = (void *)j_ExceptionCheck;

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread; // AttachCurrentThreadAsDaemon
}

// exposed for main.c to set the save/writable directory
void jni_set_writable_path(const char *p) {
  if (p && p[0]) { strncpy(g_writable, p, sizeof(g_writable) - 1); g_writable[sizeof(g_writable)-1] = 0; }
}