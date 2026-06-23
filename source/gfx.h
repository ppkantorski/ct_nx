/* gfx.h -- system-font text rasterisation for cocos2d-x label rendering
 *
 * cocos2d-x system-font Labels render text through the Android Cocos2dxBitmap
 * (Canvas/Paint) and hand the engine premultiplied ARGB_8888 pixels via
 * nativeInitBitmapDC. We replace the Java raster with FreeType on the Switch
 * shared font and produce the same premultiplied RGBA8888 byte layout.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __GFX_H__
#define __GFX_H__

// must be called once after plInitialize(); loads the Switch shared fonts.
void gfx_init(void);

// horizontal alignment (cocos: low nibble of the alignment argument)
#define GFX_ALIGN_LEFT   0
#define GFX_ALIGN_CENTER 1
#define GFX_ALIGN_RIGHT  2

// Render UTF-8 text into a freshly malloc'd, premultiplied RGBA8888 buffer of
// *out_w * *out_h * 4 bytes (byte order R,G,B,A). r/g/b/a are the 0-255 fill
// colour. max_w / max_h are the requested constraint box (0 = size to content).
// wrap enables greedy word wrapping to max_w. Returns NULL on failure.
unsigned char *gfx_render_text_rgba(const char *text, int font_size,
                                    int r, int g, int b, int a,
                                    int align_h, int max_w, int max_h, int wrap,
                                    int *out_w, int *out_h);

#endif
