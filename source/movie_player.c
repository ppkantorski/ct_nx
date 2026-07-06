/* movie_player.c -- FMV cutscene playback (ffmpeg over the cocos GLES2 surface)
 *
 * The cutscenes are assets/001.dat..008.dat (and 007-en.dat): MP4 (H.264 + AAC)
 * XOR-obfuscated with out[i] = in[i] ^ (0xFF - (i & 0xFF)). The engine drives
 * them via Cocos2dxVideoHelper; movie_play() de-obfuscates, decodes, draws over
 * the engine surface and feeds audio to the opensles movie ring. Blocking, like
 * the software keyboard. Adapted from the CR3 Switch port.
 *
 * Copyright (C) 2026 NaGaa95 <https://github.com/NaGaa95>
 * Copyright (C) 2026 ppkantorski <https://github.com/ppkantorski>
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <switch.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>

#include "movie_player.h"
#include "opensles.h"
#include "asset.h"
#include "util.h"

static void (*g_gl_invalidate)(void); // cocos2d::GL::invalidateStateCache

void movie_set_gl_invalidate(void (*fn)(void)) { g_gl_invalidate = fn; }

// Read the whole movie by asset name, undoing the .dat XOR obfuscation.
static uint8_t *read_movie(const char *name, int *out_size) {
  if (!name || !name[0])
    return NULL;

  void *a = AAssetManager_open_fake(NULL, name, 0);
  if (!a)
    return NULL;
  int64_t sz = AAsset_getLength64_fake(a);
  if (sz <= 32 || sz > 800LL * 1024 * 1024) { AAsset_close_fake(a); return NULL; }

  uint8_t *data = malloc((size_t)sz);
  if (!data) { AAsset_close_fake(a); return NULL; }

  int64_t got = 0;
  while (got < sz) {
    int n = AAsset_read_fake(a, data + got, (size_t)(sz - got));
    if (n <= 0) break;
    got += n;
  }
  AAsset_close_fake(a);
  if (got != sz) { free(data); return NULL; }

  // the .dat cutscenes are XOR-obfuscated MP4s (plain files would not be)
  const size_t n = strlen(name);
  if (n >= 4 && !strcmp(name + n - 4, ".dat"))
    for (int64_t i = 0; i < sz; i++)
      data[i] ^= (uint8_t)(0xFFu - ((uint64_t)i & 0xFFu));

  *out_size = (int)sz;
  return data;
}

// --- in-memory AVIO ---------------------------------------------------------

typedef struct { const uint8_t *p; int size; int pos; } MemBuf;
static int mem_read(void *o, uint8_t *buf, int n) {
  MemBuf *m = o;
  int left = m->size - m->pos;
  if (left <= 0) return AVERROR_EOF;
  if (n > left) n = left;
  memcpy(buf, m->p + m->pos, n);
  m->pos += n;
  return n;
}
static int64_t mem_seek(void *o, int64_t off, int whence) {
  MemBuf *m = o;
  if (whence == AVSEEK_SIZE) return m->size;
  if (whence == SEEK_END) off += m->size;
  else if (whence == SEEK_CUR) off += m->pos;
  if (off < 0) off = 0;
  if (off > m->size) off = m->size;
  m->pos = (int)off;
  return m->pos;
}

// --- GLES2 fullscreen textured quad -----------------------------------------

static const char *VSH =
  "attribute vec2 aPos; attribute vec2 aTex; varying vec2 vTex;"
  "void main(){ vTex=aTex; gl_Position=vec4(aPos,0.0,1.0); }";
static const char *FSH =
  "precision mediump float; varying vec2 vTex; uniform sampler2D uTex;"
  "void main(){ gl_FragColor=texture2D(uTex,vTex); }";

static GLuint compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { char log[256]; glGetShaderInfoLog(s, sizeof(log), NULL, log); debugPrintf("movie: shader err: %s\n", log); }
  return s;
}

typedef struct {
  GLuint prog, tex, vs, fs;
  GLint aPos, aTex, uTex;
  int sw, sh;
} GLVid;

static int gl_init(GLVid *g) {
  memset(g, 0, sizeof(*g));
  GLint vp[4] = { 0, 0, 0, 0 };
  glGetIntegerv(GL_VIEWPORT, vp); // the engine's current viewport == screen
  g->sw = vp[2] > 0 ? vp[2] : 1280;
  g->sh = vp[3] > 0 ? vp[3] : 720;
  g->vs = compile(GL_VERTEX_SHADER, VSH);
  g->fs = compile(GL_FRAGMENT_SHADER, FSH);
  g->prog = glCreateProgram();
  glAttachShader(g->prog, g->vs);
  glAttachShader(g->prog, g->fs);
  glLinkProgram(g->prog);
  GLint ok = 0; glGetProgramiv(g->prog, GL_LINK_STATUS, &ok);
  if (!ok) { debugPrintf("movie: program link failed\n"); return 0; }
  g->aPos = glGetAttribLocation(g->prog, "aPos");
  g->aTex = glGetAttribLocation(g->prog, "aTex");
  g->uTex = glGetUniformLocation(g->prog, "uTex");
  glGenTextures(1, &g->tex);
  glBindTexture(GL_TEXTURE_2D, g->tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  return 1;
}

static void gl_draw(GLVid *g, const uint8_t *rgba, int vw, int vh) {
  // letterbox: fit vw x vh inside the surface preserving aspect ratio
  float sa = (float)g->sw / (float)g->sh, va = (float)vw / (float)vh;
  float ex = 1.0f, ey = 1.0f;
  if (va > sa) ey = sa / va; else ex = va / sa;
  const GLfloat quad[] = {
    -ex,  ey, 0.0f, 0.0f,
    -ex, -ey, 0.0f, 1.0f,
     ex,  ey, 1.0f, 0.0f,
     ex, -ey, 1.0f, 1.0f,
  };
  glViewport(0, 0, g->sw, g->sh);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(g->prog);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g->tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, vw, vh, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  glUniform1i(g->uTex, 0);
  glEnableVertexAttribArray(g->aPos);
  glEnableVertexAttribArray(g->aTex);
  glVertexAttribPointer(g->aPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad);
  glVertexAttribPointer(g->aTex, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad + 2);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void gl_present(void) {
  EGLDisplay dpy = eglGetCurrentDisplay();
  EGLSurface sfc = eglGetCurrentSurface(EGL_DRAW);
  if (dpy != EGL_NO_DISPLAY && sfc != EGL_NO_SURFACE)
    eglSwapBuffers(dpy, sfc);
}

static void gl_free(GLVid *g) {
  if (g->tex) glDeleteTextures(1, &g->tex);
  if (g->prog) glDeleteProgram(g->prog);
  if (g->vs) glDeleteShader(g->vs);
  if (g->fs) glDeleteShader(g->fs);
  glUseProgram(0);
}

// --- last-frame hold ---------------------------------------------------------
// After normal playback ends, stash the final rendered frame so the main render
// loop can re-present it while the engine's scene transition runs, preventing
// the black-screen gap between movie end and the engine's first post-movie draw.
//
// Flow:
//   movie_play() ends  -> stashes last frame in g_hold_*
//   g_video_cb(COMPLETED) fires  -> engine sets its 'video done' flag
//   movie_hold_last_frame(N)  -> arms g_hold_frames with N; returns immediately
//   main.c render loop: calls movie_post_render() instead of eglSwapBuffers
//     -> presents hold frame; on last frame frees GL resources + invalidates
//   Normal rendering resumes once g_hold_frames reaches 0.

static GLVid    g_hold_gl;
static int      g_hold_gl_ok  = 0;
static int      g_hold_vw     = 0;
static int      g_hold_vh     = 0;
static uint8_t *g_hold_rgba   = NULL;
static int      g_hold_frames = 0; // frames remaining to present via post_render
static int      g_completion_flag = 0; // set when movie ends normally; cleared by take

// Arm the hold counter. Call AFTER firing VIDEO_EVENT_COMPLETED so the engine
// flag is already set before the first post-render swap happens.
void movie_hold_last_frame(int frames) {
  if (!g_hold_gl_ok || !g_hold_rgba || frames <= 0) return;
  g_hold_frames = frames;
}

// Called from the main render loop in place of eglSwapBuffers.
// Returns 1 if it handled the swap (caller must NOT call eglSwapBuffers).
// Returns 0 if no hold is active (caller swaps normally).
int movie_post_render(EGLDisplay dpy, EGLSurface sfc) {
  if (!g_hold_gl_ok || !g_hold_rgba || g_hold_frames <= 0) return 0;

  gl_draw(&g_hold_gl, g_hold_rgba, g_hold_vw, g_hold_vh);
  if (dpy != EGL_NO_DISPLAY && sfc != EGL_NO_SURFACE)
    eglSwapBuffers(dpy, sfc);

  g_hold_frames--;
  if (g_hold_frames <= 0) {
    // All hold frames consumed: release GL resources and restore engine state.
    gl_free(&g_hold_gl);
    if (g_gl_invalidate) g_gl_invalidate();
    free(g_hold_rgba);
    g_hold_rgba  = NULL;
    g_hold_gl_ok = 0;
    g_hold_frames = 0;
  }
  return 1;
}

// Returns 1 exactly once after a movie completes normally, then resets.
// main.c polls this each frame to know when to inject a synthetic BACK key.
int movie_take_completion_flag(void) {
  if (!g_completion_flag) return 0;
  g_completion_flag = 0;
  return 1;
}

// Called from jni_fake.c after a movie finishes normally (not skipped).
void movie_signal_completion(void) {
  g_completion_flag = 1;
}

// --- playback ---------------------------------------------------------------

static int skip_pressed(PadState *pad) {
  padUpdate(pad);
  return (padGetButtonsDown(pad) & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_Plus)) != 0;
}

int movie_play(const char *name) {
  int msize = 0;
  uint8_t *mdata = read_movie(name, &msize);
  if (!mdata) {
    debugPrintf("movie: %s not found\n", name ? name : "(null)");
    return 0; // not found: just continue
  }
  cpu_boost(1); // H.264 software decode is heavy; boost for smooth playback

  int stop = 0;
  AVFormatContext *fmt = NULL;
  AVIOContext *avio = NULL;
  AVCodecContext *vdec = NULL, *adec = NULL;
  struct SwsContext *sws = NULL;
  SwrContext *swr = NULL;
  uint8_t *rgba = NULL;
  int16_t *apcm = NULL;
  AVPacket *pkt = NULL;
  AVFrame *frame = NULL;
  int vidx = -1, aidx = -1, vw = 0, vh = 0, dev_rate = 48000;
  GLVid gl; int gl_ok = 0;
  MemBuf mem = { mdata, msize, 0 };
  PadState pad;
  padInitializeDefault(&pad);

  unsigned char *iobuf = av_malloc(65536);
  avio = avio_alloc_context(iobuf, 65536, 0, &mem, mem_read, NULL, mem_seek);
  if (!avio) goto done;
  fmt = avformat_alloc_context();
  fmt->pb = avio;
  if (avformat_open_input(&fmt, NULL, NULL, NULL) < 0) { debugPrintf("movie: open_input failed\n"); goto done; }
  if (avformat_find_stream_info(fmt, NULL) < 0) { debugPrintf("movie: no stream info\n"); goto done; }

  for (unsigned i = 0; i < fmt->nb_streams; i++) {
    enum AVMediaType t = fmt->streams[i]->codecpar->codec_type;
    if (t == AVMEDIA_TYPE_VIDEO && vidx < 0) vidx = (int)i;
    else if (t == AVMEDIA_TYPE_AUDIO && aidx < 0) aidx = (int)i;
  }
  if (vidx < 0) goto done;

  {
    AVCodecParameters *vp = fmt->streams[vidx]->codecpar;
    const AVCodec *vc = avcodec_find_decoder(vp->codec_id);
    if (!vc) goto done;
    vdec = avcodec_alloc_context3(vc);
    avcodec_parameters_to_context(vdec, vp);
    vdec->thread_count = 3;
    if (avcodec_open2(vdec, vc, NULL) < 0) goto done;
    vw = vp->width; vh = vp->height;
    sws = sws_getContext(vw, vh, vdec->pix_fmt, vw, vh, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
    rgba = malloc((size_t)vw * vh * 4);
    if (!sws || !rgba) goto done;
  }

  dev_rate = opensles_movie_begin(48000);
  if (dev_rate <= 0) dev_rate = 48000;
  opensles_movie_set_paused(0); // play as soon as audio is queued

  if (aidx >= 0) {
    AVCodecParameters *ap = fmt->streams[aidx]->codecpar;
    const AVCodec *ac = avcodec_find_decoder(ap->codec_id);
    if (ac) {
      adec = avcodec_alloc_context3(ac);
      avcodec_parameters_to_context(adec, ap);
      if (avcodec_open2(adec, ac, NULL) == 0) {
        AVChannelLayout out_ch = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&swr, &out_ch, AV_SAMPLE_FMT_S16, dev_rate,
                                &adec->ch_layout, adec->sample_fmt, adec->sample_rate, 0, NULL) == 0 && swr)
          swr_init(swr);
        apcm = malloc((size_t)192000 * 2 * sizeof(int16_t));
      } else { aidx = -1; }
    } else { aidx = -1; }
  }

  gl_ok = gl_init(&gl);
  pkt = av_packet_alloc();
  frame = av_frame_alloc();

  const double tb_v = av_q2d(fmt->streams[vidx]->time_base);
  const double tickHz = (double)armGetSystemTickFreq();
  const uint64_t t0 = armGetSystemTick();

  while (!stop && av_read_frame(fmt, pkt) >= 0) {
    if (pkt->stream_index == aidx && adec && swr && apcm) {
      if (avcodec_send_packet(adec, pkt) == 0) {
        while (avcodec_receive_frame(adec, frame) == 0) {
          uint8_t *outp = (uint8_t *)apcm;
          int outn = swr_convert(swr, &outp, 192000,
                                 (const uint8_t **)frame->extended_data, frame->nb_samples);
          if (outn > 0) opensles_movie_queue(apcm, outn);
        }
      }
    } else if (pkt->stream_index == vidx) {
      if (avcodec_send_packet(vdec, pkt) == 0) {
        while (avcodec_receive_frame(vdec, frame) == 0) {
          uint8_t *dst[4] = { rgba, NULL, NULL, NULL };
          int dstst[4] = { vw * 4, 0, 0, 0 };
          sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0, vh, dst, dstst);

          // pace each frame to its PTS against the audio clock (else wall time)
          double pts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                         ? (double)frame->best_effort_timestamp * tb_v : 0.0;
          for (int spin = 0; spin < 2000 && !stop; spin++) {
            double clk = (aidx >= 0)
                           ? (double)opensles_movie_samples_played() / (double)dev_rate
                           : (double)(armGetSystemTick() - t0) / tickHz;
            if (clk + 0.001 >= pts) break;
            svcSleepThread(2000000ull); // 2ms
            if (skip_pressed(&pad)) stop = 1;
          }
          if (skip_pressed(&pad)) stop = 1;
          if (gl_ok) { gl_draw(&gl, rgba, vw, vh); gl_present(); }
          if (stop) break;
        }
      }
    }
    av_packet_unref(pkt);
  }

  // Flush the video decoder: H.264 buffers several frames internally.
  // Without this drain the last few frames (typically the fade-to-black)
  // are never presented, leaving the screen black until the engine repaints.
  if (!stop && vdec && gl_ok) {
    avcodec_send_packet(vdec, NULL); // NULL packet = flush signal
    while (avcodec_receive_frame(vdec, frame) == 0) {
      uint8_t *dst[4] = { rgba, NULL, NULL, NULL };
      int dstst[4] = { vw * 4, 0, 0, 0 };
      sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0, vh, dst, dstst);
      double pts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                     ? (double)frame->best_effort_timestamp * tb_v : 0.0;
      for (int spin = 0; spin < 2000 && !stop; spin++) {
        double clk = (aidx >= 0)
                       ? (double)opensles_movie_samples_played() / (double)dev_rate
                       : (double)(armGetSystemTick() - t0) / tickHz;
        if (clk + 0.001 >= pts) break;
        svcSleepThread(2000000ull);
        if (skip_pressed(&pad)) stop = 1;
      }
      if (!stop && skip_pressed(&pad)) stop = 1;
      if (!stop) { gl_draw(&gl, rgba, vw, vh); gl_present(); }
    }
  }

done:
  // If playback completed normally (not skipped), stash the last frame so
  // movie_post_render() can re-present it from the main render loop while the
  // engine runs its scene transition. gl_free and g_gl_invalidate are deferred
  // to movie_post_render() in that case; on skip we clean up immediately.
  g_hold_gl_ok = 0;
  if (!stop && gl_ok && rgba && vw > 0 && vh > 0) {
    g_hold_gl   = gl;
    g_hold_vw   = vw;
    g_hold_vh   = vh;
    g_hold_rgba = rgba;
    rgba        = NULL; // ownership transferred -- don't free below
    gl_ok       = 0;    // suppress gl_free below
    g_hold_gl_ok = 1;
  }
  if (gl_ok) gl_free(&gl);
  if (!g_hold_gl_ok && g_gl_invalidate) g_gl_invalidate();
  opensles_movie_end();
  if (pkt) av_packet_free(&pkt);
  if (frame) av_frame_free(&frame);
  if (swr) swr_free(&swr);
  if (sws) sws_freeContext(sws);
  if (vdec) avcodec_free_context(&vdec);
  if (adec) avcodec_free_context(&adec);
  if (fmt) avformat_close_input(&fmt);
  if (avio) { av_freep(&avio->buffer); avio_context_free(&avio); }
  free(rgba);
  free(apcm);
  free(mdata);
  cpu_boost(0);
  return 1;
}
