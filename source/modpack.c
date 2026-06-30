/* modpack.c -- ChronoMod-compatible .ctp mod pack support
 *
 * resources.bin format (from ChronoMod / DetchmanResource reverse engineering):
 *
 *   Bytes 0..15: 16-byte header, XOR-encoded with keystream K(0..15).
 *     [0..3]  magic "ARC1"
 *     [4..7]  uint32 LE: total file size
 *     [8..11] uint32 LE: offset_header (byte offset to the index blob)
 *     [12..15] uint32 LE: compressed_length_header (length of index blob)
 *
 *   Bytes 16..offset_header-1: per-entry data blobs, each XOR-encoded at
 *     their own absolute offset.  Each blob is:
 *     [0..3] uint32 big-endian: uncompressed length of the following gzip data
 *     [4..N] gzip-compressed (inflateInit2 with 16+MAX_WBITS) file data
 *
 *   Bytes offset_header..end: the index blob, also XOR-encoded.
 *     [0..3]  uint32 big-endian: uncompressed length of what follows
 *     [4..M]  gzip-compressed index, which decompresses to:
 *       [0..3]  uint32 LE: entry_count
 *       then entry_count * 12 bytes:
 *         [0..3]  uint32 LE: path_name_offset (absolute into decompressed buffer)
 *         [4..7]  uint32 LE: entry_offset (absolute byte offset of data blob)
 *         [8..11] uint32 LE: entry_length  (byte length of data blob, encoded)
 *       followed immediately by null-terminated entry path strings.
 *
 * XOR keystream K(offset, length):
 *   tmp = 0x19000000 + offset  (seed = absolute file offset)
 *   for i in 0..length-1:
 *     tmp = tmp * 0x41c64e6d + 0x3039  (LCG, C int32 wraparound)
 *     buf[i] ^= (tmp >> 24) & 0xFF
 *
 * .ctp format: a standard PKZIP file (deflate or stored).  Each ZIP entry's
 * relative path is identical to the resources.bin entry path it replaces
 * (e.g. "Game/common/AccessorieDataTable.dat").  The stored content is the
 * raw replacement data (uncompressed; we re-gzip it when rebuilding the bin).
 *
 * Folder mods: a subdirectory inside mods_dir is treated as a loose-file mod.
 * Files at mods_dir/<ModName>/Game/common/foo.dat replace Game/common/foo.dat.
 * Folder mods are processed after .ctp files, so they take priority.
 *
 * Cache: after the first rebuild, the patched archive is written to
 * mods_dir/.modcache.  On subsequent boots a header (magic + fingerprints of
 * resources.bin and every mod source) is compared against values computed once
 * during the candidate scan.  If it matches the cache is loaded directly with
 * no further filesystem work -- one sequential SD read, no re-stat, no re-walk.
 *
 * Fingerprinting:
 *   .ctp files  -- (st_size, st_mtime) pair computed once during scan.
 *   folder mods -- recursive XOR of (st_size ^ (uint32)st_mtime) over every
 *                  regular file in the tree, computed once during scan.
 *                  An empty folder (fingerprint == 0) is excluded entirely so
 *                  it never affects the cache key.
 *   Both are stored in SrcPath.desc and reused for cache_load and cache_save
 *   without any repeated stat or directory walk.
 *
 * Threading: on a cache miss the gzip recompression of override entries is
 * split across up to two worker threads (cores 1 and 2) plus the main thread
 * (core 0).  Only override entries get a slot -- unmodified entries are not
 * touched by the workers at all.
 *
 * Memory: arc_load reads only the 16-byte header and the compressed index;
 * the large data region of resources.bin is NOT buffered.  arc_rebuild streams
 * unmodified blobs directly from the open file into the output buffer using a
 * single fread per blob (no intermediate copy), cutting peak RAM by ~50% and
 * eliminating the double-copy that the previous stream_copy helper did.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "modpack.h"
#include "config.h"
#include "util.h"
#include "cache_progress.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <zlib.h>
#include <switch.h>   /* Thread, Mutex, threadCreate/Start/WaitForExit/Close */

/* --------------------------------------------------------------------------
 * 32-bit byte-swap.
 * -------------------------------------------------------------------------- */
static inline uint32_t bswap32(uint32_t x) {
  return __builtin_bswap32(x);
}

/* --------------------------------------------------------------------------
 * LCG keystream -- identical to DetchmanResource::decode().
 * offset = absolute byte position of buf[0] within resources.bin.
 * -------------------------------------------------------------------------- */
static void arc_xor(int offset, int length, uint8_t *buf) {
  int tmp = 0x19000000 + offset;
  for (int i = 0; i < length; i++) {
    tmp = tmp * 0x41c64e6d + 0x3039;
    buf[i] ^= (uint8_t)((tmp >> 24) & 0xFF);
  }
}

/* Single-pass re-key: decode old offset, encode new offset simultaneously. */
static void arc_rekey(int old_off, int new_off, int length, uint8_t *buf) {
  int t_old = 0x19000000 + old_off;
  int t_new = 0x19000000 + new_off;
  for (int i = 0; i < length; i++) {
    t_old = t_old * 0x41c64e6d + 0x3039;
    t_new = t_new * 0x41c64e6d + 0x3039;
    buf[i] ^= (uint8_t)((t_old >> 24) & 0xFF) ^ (uint8_t)((t_new >> 24) & 0xFF);
  }
}

/* --------------------------------------------------------------------------
 * gzip decompress / compress helpers.
 * -------------------------------------------------------------------------- */
static int gz_decompress(const uint8_t *src, uLong src_len,
                          uint8_t *dst, uLong *dst_len) {
  z_stream s;
  memset(&s, 0, sizeof(s));
  s.next_in   = (Bytef *)src;  s.avail_in  = (uInt)src_len;
  s.next_out  = (Bytef *)dst;  s.avail_out = (uInt)*dst_len;
  int r = inflateInit2(&s, 16 + MAX_WBITS);
  if (r != Z_OK) return r;
  r = inflate(&s, Z_FINISH);
  *dst_len = s.total_out;
  inflateEnd(&s);
  return (r == Z_STREAM_END) ? Z_OK : r;
}

/* Compress into a caller-supplied buffer; returns compressed size or 0. */
static uLong gz_compress_into(const uint8_t *src, uLong src_len,
                               uint8_t *dst, uLong dst_cap) {
  z_stream s;
  memset(&s, 0, sizeof(s));
  s.next_in   = (Bytef *)src;  s.avail_in  = (uInt)src_len;
  s.next_out  = (Bytef *)dst;  s.avail_out = (uInt)dst_cap;
  int r = deflateInit2(&s, Z_BEST_SPEED, Z_DEFLATED, 16 + MAX_WBITS,
                        8, Z_DEFAULT_STRATEGY);
  if (r != Z_OK) return 0;
  r = deflate(&s, Z_FINISH);
  uLong out = s.total_out;
  deflateEnd(&s);
  return (r == Z_STREAM_END) ? out : 0;
}

/* ==========================================================================
 * Minimal PKZIP central-directory reader
 * ========================================================================== */

typedef struct {
  char    *name;
  uint16_t method;       /* 0=stored, 8=deflate */
  uint32_t comp_size;
  uint32_t uncomp_size;
  uint32_t local_offset;
} ZipEntry;

typedef struct {
  FILE     *f;
  ZipEntry *entries;
  int       count;
} ZipReader;

static void zip_free(ZipReader *z) {
  if (!z) return;
  if (z->entries) {
    for (int i = 0; i < z->count; i++) free(z->entries[i].name);
    free(z->entries);
  }
  if (z->f) fclose(z->f);
}

static long zip_find_eocd(FILE *f, long fsize) {
  long tail_size = fsize < 65557 ? fsize : 65557;
  uint8_t *tail = (uint8_t *)malloc(tail_size);
  if (!tail) return -1;
  fseek(f, fsize - tail_size, SEEK_SET);
  if ((long)fread(tail, 1, tail_size, f) != tail_size) { free(tail); return -1; }
  long eocd_off = -1;
  for (long i = tail_size - 22; i >= 0; i--) {
    if (tail[i]==0x50 && tail[i+1]==0x4B && tail[i+2]==0x05 && tail[i+3]==0x06) {
      eocd_off = (fsize - tail_size) + i; break;
    }
  }
  free(tail);
  return eocd_off;
}

static int zip_open(ZipReader *z, const char *path) {
  memset(z, 0, sizeof(*z));
  z->f = fopen(path, "rb");
  if (!z->f) return 0;
  fseek(z->f, 0, SEEK_END);
  long fsize = ftell(z->f);
  long eocd_off = zip_find_eocd(z->f, fsize);
  if (eocd_off < 0) { zip_free(z); return 0; }

  uint8_t eocd[22];
  fseek(z->f, eocd_off, SEEK_SET);
  if (fread(eocd, 1, 22, z->f) != 22) { zip_free(z); return 0; }
  uint16_t total_entries; uint32_t cd_size, cd_offset;
  memcpy(&total_entries, eocd+10, 2);
  memcpy(&cd_size,       eocd+12, 4);
  memcpy(&cd_offset,     eocd+16, 4);

  uint8_t *cd = (uint8_t *)malloc(cd_size);
  if (!cd) { zip_free(z); return 0; }
  fseek(z->f, cd_offset, SEEK_SET);
  if (fread(cd, 1, cd_size, z->f) != cd_size) { free(cd); zip_free(z); return 0; }

  z->entries = (ZipEntry *)calloc(total_entries, sizeof(ZipEntry));
  if (!z->entries) { free(cd); zip_free(z); return 0; }

  uint32_t pos = 0;
  for (uint16_t i = 0; i < total_entries && pos+46 <= cd_size; i++) {
    if (cd[pos]!=0x50||cd[pos+1]!=0x4B||cd[pos+2]!=0x01||cd[pos+3]!=0x02) break;
    uint16_t method, name_len, extra_len, comment_len;
    uint32_t comp_size, uncomp_size, local_offset;
    memcpy(&method,       cd+pos+10, 2); memcpy(&comp_size,    cd+pos+20, 4);
    memcpy(&uncomp_size,  cd+pos+24, 4); memcpy(&name_len,     cd+pos+28, 2);
    memcpy(&extra_len,    cd+pos+30, 2); memcpy(&comment_len,  cd+pos+32, 2);
    memcpy(&local_offset, cd+pos+42, 4);
    if (pos+46+name_len > cd_size) break;
    if (name_len > 0 && cd[pos+46+name_len-1] != '/') {
      ZipEntry *e = &z->entries[z->count];
      e->name = (char *)malloc(name_len+1);
      if (!e->name) break;
      memcpy(e->name, cd+pos+46, name_len);
      e->name[name_len] = '\0';
      e->method = method; e->comp_size = comp_size;
      e->uncomp_size = uncomp_size; e->local_offset = local_offset;
      z->count++;
    }
    pos += 46 + name_len + extra_len + comment_len;
  }
  free(cd);
  return (z->count > 0) ? 1 : 0;
}

/* Extract into a caller-supplied buffer; returns uncompressed size, 0 on error. */
static size_t zip_extract_into(ZipReader *z, const ZipEntry *e, uint8_t *buf) {
  uint8_t lhdr[30];
  fseek(z->f, e->local_offset, SEEK_SET);
  if (fread(lhdr, 1, 30, z->f) != 30) return 0;
  if (lhdr[0]!=0x50||lhdr[1]!=0x4B||lhdr[2]!=0x03||lhdr[3]!=0x04) return 0;
  uint16_t name_len, extra_len;
  memcpy(&name_len,  lhdr+26, 2);
  memcpy(&extra_len, lhdr+28, 2);
  long data_off = (long)e->local_offset + 30 + name_len + extra_len;

  if (e->method == 0) {
    fseek(z->f, data_off, SEEK_SET);
    if (fread(buf, 1, e->uncomp_size, z->f) != e->uncomp_size) return 0;
    return e->uncomp_size;
  } else if (e->method == 8) {
    uint8_t *comp = (uint8_t *)malloc(e->comp_size);
    if (!comp) return 0;
    fseek(z->f, data_off, SEEK_SET);
    if (fread(comp, 1, e->comp_size, z->f) != e->comp_size) { free(comp); return 0; }
    z_stream s; memset(&s, 0, sizeof(s));
    s.next_in = comp; s.avail_in = e->comp_size;
    s.next_out = buf; s.avail_out = e->uncomp_size;
    int r = inflateInit2(&s, -MAX_WBITS);
    if (r == Z_OK) r = inflate(&s, Z_FINISH);
    size_t got = s.total_out;
    inflateEnd(&s); free(comp);
    return (r == Z_STREAM_END) ? got : 0;
  }
  return 0;
}

/* ==========================================================================
 * In-memory resources.bin representation
 *
 * arc_load reads ONLY the 16-byte header + compressed index blob.
 * The file handle (Arc.f) is kept open so arc_rebuild can fread unmodified
 * entry blobs directly into the output buffer without any intermediate copy.
 * ========================================================================== */

typedef struct {
  uint32_t path_name_offset; /* absolute byte offset into Arc.idx_buf */
  uint32_t entry_offset;
  uint32_t entry_length;
  char    *path;             /* points into Arc.idx_buf */
  uint8_t *override_data;   /* malloc'd raw replacement, or NULL */
  size_t   override_size;
} ArcEntry;

typedef struct {
  FILE    *f;          /* open handle to resources.bin (kept for streaming) */
  size_t   file_size;  /* total file size, used for output capacity estimate */
  uint32_t  offset_header;
  uint32_t  compressed_length_header;
  uint32_t  uncompressed_length_header;
  ArcEntry *entries;
  int       entry_count;
  char     *idx_buf;          /* full decompressed index (pno absolute into here) */
  size_t    idx_size;
  size_t    path_section_off;
} Arc;

/* ==========================================================================
 * Open-addressing hash map: string -> int (arc entry index).
 * ========================================================================== */

#define HM_EMPTY (-1)

typedef struct { const char *key; int val; } HmSlot;
typedef struct { HmSlot *slots; int cap; } HashMap;

static uint32_t hm_hash(const char *s) {
  uint32_t h = 2166136261u;
  while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
  return h;
}
static void hm_free(HashMap *m) { free(m->slots); m->slots = NULL; m->cap = 0; }

static int hm_build(HashMap *m, ArcEntry *entries, int n) {
  int cap = 16;
  while (cap < n * 2) cap <<= 1;
  m->slots = (HmSlot *)malloc((size_t)cap * sizeof(HmSlot));
  if (!m->slots) return 0;
  m->cap = cap;
  for (int i = 0; i < cap; i++) m->slots[i].val = HM_EMPTY;
  for (int i = 0; i < n; i++) {
    if (!entries[i].path) continue;
    uint32_t h = hm_hash(entries[i].path) & (uint32_t)(cap-1);
    while (m->slots[h].val != HM_EMPTY) h = (h+1) & (uint32_t)(cap-1);
    m->slots[h].key = entries[i].path;
    m->slots[h].val = i;
  }
  return 1;
}

static int hm_lookup(const HashMap *m, const char *key) {
  uint32_t h = hm_hash(key) & (uint32_t)(m->cap-1);
  while (m->slots[h].val != HM_EMPTY) {
    if (strcmp(m->slots[h].key, key) == 0) return m->slots[h].val;
    h = (h+1) & (uint32_t)(m->cap-1);
  }
  return HM_EMPTY;
}

/* ==========================================================================
 * arc_free / arc_load
 * ========================================================================== */

static void arc_free(Arc *a) {
  if (!a) return;
  if (a->f) { fclose(a->f); a->f = NULL; }
  if (a->entries) {
    for (int i = 0; i < a->entry_count; i++) free(a->entries[i].override_data);
    free(a->entries);
  }
  free(a->idx_buf);
}

/* arc_load: open resources.bin, decode the 16-byte header and the compressed
 * index blob only.  The file handle is left open for streaming during rebuild.
 * The large data region (entry blobs) is NOT read into RAM here. */
static int arc_load(Arc *a, const char *path) {
  memset(a, 0, sizeof(*a));
  a->f = fopen(path, "rb");
  if (!a->f) return 0;
  fseek(a->f, 0, SEEK_END);
  long fsize = ftell(a->f);
  if (fsize < 16) { arc_free(a); return 0; }
  a->file_size = (size_t)fsize;

  /* Read and decode the 16-byte header. */
  uint8_t hdr[16];
  fseek(a->f, 0, SEEK_SET);
  if (fread(hdr, 1, 16, a->f) != 16) { arc_free(a); return 0; }
  arc_xor(0, 16, hdr);
  if (memcmp(hdr, "ARC1", 4) != 0) { arc_free(a); return 0; }
  memcpy(&a->offset_header,            hdr+8,  4);
  memcpy(&a->compressed_length_header, hdr+12, 4);

  if ((size_t)a->offset_header + a->compressed_length_header > a->file_size) {
    arc_free(a); return 0;
  }

  /* Read, decode, and decompress the index blob only. */
  uint32_t enc_len = a->compressed_length_header;
  uint8_t *idx_enc = (uint8_t *)malloc(enc_len);
  if (!idx_enc) { arc_free(a); return 0; }
  fseek(a->f, (long)a->offset_header, SEEK_SET);
  if (fread(idx_enc, 1, enc_len, a->f) != enc_len) { free(idx_enc); arc_free(a); return 0; }
  arc_xor((int)a->offset_header, (int)enc_len, idx_enc);

  uint32_t unc_len_be;
  memcpy(&unc_len_be, idx_enc, 4);
  a->uncompressed_length_header = bswap32(unc_len_be);

  uint8_t *idx = (uint8_t *)malloc(a->uncompressed_length_header);
  if (!idx) { free(idx_enc); arc_free(a); return 0; }
  uLong unc = a->uncompressed_length_header;
  int r = gz_decompress(idx_enc + 4, enc_len - 4, idx, &unc);
  free(idx_enc);
  if (r != Z_OK) { free(idx); arc_free(a); return 0; }

  if (unc < 4) { free(idx); arc_free(a); return 0; }
  uint32_t entry_count;
  memcpy(&entry_count, idx, 4);
  if (entry_count == 0 || 4 + (size_t)entry_count * 12 > unc) {
    free(idx); arc_free(a); return 0;
  }

  a->entries = (ArcEntry *)calloc(entry_count, sizeof(ArcEntry));
  if (!a->entries) { free(idx); arc_free(a); return 0; }
  a->entry_count      = (int)entry_count;
  a->idx_buf          = (char *)idx;
  a->idx_size         = (size_t)unc;
  a->path_section_off = 4 + (size_t)entry_count * 12;

  uint32_t tbl_off = 4;
  for (uint32_t i = 0; i < entry_count; i++) {
    uint32_t pno, eo, el;
    memcpy(&pno, idx + tbl_off,     4);
    memcpy(&eo,  idx + tbl_off + 4, 4);
    memcpy(&el,  idx + tbl_off + 8, 4);
    tbl_off += 12;
    a->entries[i].path_name_offset = pno;
    a->entries[i].entry_offset     = eo;
    a->entries[i].entry_length     = el;
    a->entries[i].path = (pno < a->idx_size)
                         ? a->idx_buf + pno
                         : a->idx_buf + a->path_section_off;
  }
  return 1;
}

/* ==========================================================================
 * Apply mods: .ctp files and loose-file folder mods
 * ========================================================================== */

/* Register override_data/size against the matching arc entry.
 * Consumes data on success (takes ownership); frees it on no-match. */
static void arc_register_override(Arc *a, const HashMap *hm, int hm_ok,
                                   const char *rel_path,
                                   uint8_t *data, size_t data_len) {
  int ai = hm_ok ? hm_lookup(hm, rel_path) : HM_EMPTY;
  if (ai == HM_EMPTY) {
    /* Fallback linear scan if hash map unavailable */
    ai = -1;
    for (int k = 0; k < a->entry_count; k++) {
      if (strcmp(a->entries[k].path, rel_path) == 0) { ai = k; break; }
    }
  }
  if (ai < 0) { free(data); return; }
  free(a->entries[ai].override_data);
  a->entries[ai].override_data = data;
  a->entries[ai].override_size = data_len;
}

/* Apply a single .ctp (ZIP) mod file. */
static int apply_ctp(Arc *a, const HashMap *hm, int hm_ok,
                      const char *ctp_path) {
  ZipReader z;
  if (!zip_open(&z, ctp_path)) return 0;
  int count = 0;
  for (int zi = 0; zi < z.count; zi++) {
    const ZipEntry *ze = &z.entries[zi];
    uint8_t *data = (uint8_t *)malloc(ze->uncomp_size ? ze->uncomp_size : 1);
    if (!data) continue;
    size_t got = zip_extract_into(&z, ze, data);
    if (!got) { free(data); continue; }
    arc_register_override(a, hm, hm_ok, ze->name, data, got);
    count++;
  }
  zip_free(&z);
  return count;
}

/* Recursively walk dir_path, building rel_path as we go, registering each
 * regular file as an override.  rel_prefix is the slash-terminated path
 * relative to the mod root (empty string at the top level). */
static int apply_folder_recursive(Arc *a, const HashMap *hm, int hm_ok,
                                   const char *dir_path,
                                   const char *rel_prefix) {
  DIR *d = opendir(dir_path);
  if (!d) return 0;
  int count = 0;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (ent->d_name[0] == '.') continue; /* skip . .. and hidden */
    char full[1024];
    snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);
    struct stat st;
    if (stat(full, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      /* recurse: build new rel_prefix */
      char new_rel[512];
      snprintf(new_rel, sizeof(new_rel), "%s%s/", rel_prefix, ent->d_name);
      count += apply_folder_recursive(a, hm, hm_ok, full, new_rel);
    } else if (S_ISREG(st.st_mode)) {
      /* relative path = rel_prefix + filename */
      char rel[512];
      snprintf(rel, sizeof(rel), "%s%s", rel_prefix, ent->d_name);
      /* Load file contents */
      FILE *f = fopen(full, "rb");
      if (!f) continue;
      fseek(f, 0, SEEK_END);
      long fsz = ftell(f);
      fseek(f, 0, SEEK_SET);
      if (fsz <= 0) { fclose(f); continue; }
      uint8_t *data = (uint8_t *)malloc((size_t)fsz);
      if (!data) { fclose(f); continue; }
      if ((long)fread(data, 1, (size_t)fsz, f) != fsz) {
        fclose(f); free(data); continue;
      }
      fclose(f);
      arc_register_override(a, hm, hm_ok, rel, data, (size_t)fsz);
      count++;
    }
  }
  closedir(d);
  return count;
}

static void arc_apply_mods(Arc *a, const char *mods_dir) {
  DIR *d = opendir(mods_dir);
  if (!d) return;

  HashMap hm;
  memset(&hm, 0, sizeof(hm));
  int hm_ok = hm_build(&hm, a->entries, a->entry_count);

  struct dirent *ent;
  int total = 0;

  /* Pass 1: .ctp zip files */
  while ((ent = readdir(d)) != NULL) {
    size_t nl = strlen(ent->d_name);
    if (nl < 5 || strcmp(ent->d_name + nl - 4, ".ctp") != 0) continue;
    char ctp_path[1024];
    snprintf(ctp_path, sizeof(ctp_path), "%s/%s", mods_dir, ent->d_name);
    int n = apply_ctp(a, &hm, hm_ok, ctp_path);
    debugPrintf("modpack: '%s' -> %d override(s)\n", ent->d_name, n);
    total += n;
  }

  /* Pass 2: loose-file folder mods (folders take priority over .ctp) */
  rewinddir(d);
  while ((ent = readdir(d)) != NULL) {
    if (ent->d_name[0] == '.') continue;
    char full[1024];
    snprintf(full, sizeof(full), "%s/%s", mods_dir, ent->d_name);
    struct stat st;
    if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
    int n = apply_folder_recursive(a, &hm, hm_ok, full, "");
    debugPrintf("modpack: folder '%s' -> %d override(s)\n", ent->d_name, n);
    total += n;
  }

  closedir(d);
  hm_free(&hm);
  debugPrintf("modpack: %d total override(s)\n", total);
}

/* ==========================================================================
 * Threaded rebuild: compress override entries in parallel on cores 0, 1, 2.
 *
 * Override entries are collected into a compact array (one slot per override,
 * not one per arc entry) so the per-thread work counter stays dense.
 * Unmodified entry blobs are read directly from Arc.f into the output buffer
 * via a single fread per blob -- no intermediate copy buffer.
 * ========================================================================== */

typedef struct {
  int       arc_idx;    /* index into Arc.entries[] */
  uint8_t  *gz_buf;    /* pre-allocated output buffer */
  uLong     gz_cap;    /* capacity */
  uLong     gz_len;    /* actual compressed size (filled by worker) */
} OverrideSlot;

typedef struct {
  ArcEntry     *entries;     /* Arc.entries base (for override_data/size) */
  OverrideSlot *slots;       /* compact array of overrides only */
  int           slot_count;
  uint32_t      next_slot;   /* atomic work counter */
  uint32_t      completed;   /* atomic count of finished slots (progress UI) */
  Mutex         mtx;
  int           failed;
} CompressCtx;

/* Compresses a single claimed slot and records the result. Shared by
 * compress_worker (t1/t2, runs to exhaustion) and compress_worker_batch
 * (main thread, claims a bounded number at a time so it can interleave
 * cache_progress_update() draws between batches). */
static void compress_one(CompressCtx *ctx, uint32_t si) {
  OverrideSlot *s = &ctx->slots[si];
  ArcEntry     *e = &ctx->entries[s->arc_idx];
  if (s->gz_buf) { /* pre-alloc failed; will be caught in assemble */
    uLong gz_len = gz_compress_into(e->override_data, (uLong)e->override_size,
                                    s->gz_buf, s->gz_cap);
    s->gz_len = gz_len;
    if (!gz_len) {
      mutexLock(&ctx->mtx);
      ctx->failed = 1;
      mutexUnlock(&ctx->mtx);
    }
  }
  __atomic_fetch_add(&ctx->completed, 1, __ATOMIC_SEQ_CST);
}

static void compress_worker(void *arg) {
  CompressCtx *ctx = (CompressCtx *)arg;
  while (1) {
    uint32_t si = __atomic_fetch_add(&ctx->next_slot, 1, __ATOMIC_SEQ_CST);
    if ((int)si >= ctx->slot_count) break;
    compress_one(ctx, si);
  }
}

/* Claims and processes at most max_items slots, then returns the number
 * actually claimed (0 once the work is exhausted). Lets the main thread
 * keep doing its third of the compression work while still getting back to
 * the render loop regularly enough to redraw the progress splash. */
static int compress_worker_batch(CompressCtx *ctx, int max_items) {
  int n;
  for (n = 0; n < max_items; n++) {
    uint32_t si = __atomic_fetch_add(&ctx->next_slot, 1, __ATOMIC_SEQ_CST);
    if ((int)si >= ctx->slot_count) break;
    compress_one(ctx, si);
  }
  return n;
}

static uint8_t *arc_rebuild_threaded(Arc *a, size_t *out_size,
                                      float *out_save_start_frac) {
  int n = a->entry_count;

  /* Build compact override slot array -- one slot per override entry only. */
  int n_overrides = 0;
  for (int i = 0; i < n; i++)
    if (a->entries[i].override_data) n_overrides++;

  if (n_overrides == 0) return NULL;

  OverrideSlot *slots = (OverrideSlot *)calloc((size_t)n_overrides, sizeof(OverrideSlot));
  if (!slots) return NULL;

  /* Fill slots and pre-allocate gzip output buffers. */
  size_t override_total = 0;
  int si = 0;
  for (int i = 0; i < n; i++) {
    if (!a->entries[i].override_data) continue;
    slots[si].arc_idx = i;
    uLong cap = compressBound((uLong)a->entries[i].override_size) + 32;
    slots[si].gz_buf = (uint8_t *)malloc(cap);
    slots[si].gz_cap = cap;
    slots[si].gz_len = 0;
    override_total  += a->entries[i].override_size + 36;
    si++;
  }

  /* The splash's 0..1 range is split into three phases weighted by the byte
   * volume each one actually has to move, not by slot/entry count -- a
   * handful of huge overrides shouldn't look identical to a thousand tiny
   * ones. compress moves override_total bytes through gzip (parallel,
   * counted exactly via ctx.completed below); assemble and cache_save each
   * move roughly a->file_size bytes (the whole archive gets read+recombined,
   * then the whole result gets written back out), so file_size is counted
   * twice in the weight total. This is what makes the bar's "100%" line up
   * with the rebuild actually being done, instead of stopping when only the
   * compress slice has finished. */
  double total_weight = (double)override_total + 2.0 * (double)a->file_size;
  if (total_weight < 1.0) total_weight = 1.0;
  float bp1 = (float)((double)override_total / total_weight);       /* end of compress */
  float bp2 = bp1 + (float)((double)a->file_size / total_weight);   /* end of assemble */
  if (bp2 > 0.999f) bp2 = 0.999f; /* leave a sliver for cache_save even on edge cases */

  /* Set up shared compression context. */
  CompressCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.entries    = a->entries;
  ctx.slots      = slots;
  ctx.slot_count = n_overrides;
  ctx.next_slot  = 0;
  ctx.failed     = 0;
  mutexInit(&ctx.mtx);

  /* Launch two worker threads on cores 1 and 2; the main thread (core 0) is
   * the third worker, but claims its share in small batches interleaved
   * with cache_progress_update() draws, so the "Caching mods..." splash
   * stays live throughout the rebuild instead of only appearing once at
   * the very end. t1/t2 keep running continuously the whole time. */
  Thread t1, t2;
  int t1_ok = R_SUCCEEDED(threadCreate(&t1, compress_worker, &ctx,
                                        NULL, 64*1024, 0x2C, 1));
  int t2_ok = R_SUCCEEDED(threadCreate(&t2, compress_worker, &ctx,
                                        NULL, 64*1024, 0x2C, 2));
  if (t1_ok) threadStart(&t1);
  if (t2_ok) threadStart(&t2);

  int batch = n_overrides / 40;
  if (batch < 1) batch = 1;
  if (batch > 64) batch = 64;

  cache_progress_update(0.0f);
  while (compress_worker_batch(&ctx, batch) > 0) {
    uint32_t done = __atomic_load_n(&ctx.completed, __ATOMIC_SEQ_CST);
    cache_progress_update(bp1 * ((float)done / (float)n_overrides));
  }
  cache_progress_update(bp1);

  if (t1_ok) { threadWaitForExit(&t1); threadClose(&t1); }
  if (t2_ok) { threadWaitForExit(&t2); threadClose(&t2); }

  if (ctx.failed) {
    cache_progress_finish();
    for (int i = 0; i < n_overrides; i++) free(slots[i].gz_buf);
    free(slots);
    return NULL;
  }

  /* Build a slot lookup: arc_entry_index -> slot_index for O(1) assemble. */
  int *entry_to_slot = (int *)malloc((size_t)n * sizeof(int));
  if (!entry_to_slot) {
    cache_progress_finish();
    for (int i = 0; i < n_overrides; i++) free(slots[i].gz_buf);
    free(slots);
    return NULL;
  }
  for (int i = 0; i < n; i++) entry_to_slot[i] = -1;
  for (int i = 0; i < n_overrides; i++) entry_to_slot[slots[i].arc_idx] = i;

  /* Allocate the output buffer. */
  size_t cap = a->file_size + override_total + (1 << 20);
  uint8_t *out = (uint8_t *)malloc(cap);
  uint32_t *new_offsets = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
  uint32_t *new_lengths = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
  if (!out || !new_offsets || !new_lengths) goto fail_assemble;

  size_t wpos = 16; /* header written last, reserve 16 bytes */

  /* Redraw roughly 40 times across the pass, but gate on bytes moved (not
   * loop index) since entry sizes vary hugely -- a few big background
   * images can dwarf hundreds of small ones, so index-based striding alone
   * would make the bar race ahead then stall. */
  size_t assembled_bytes = 0;
  size_t assemble_redraw_step = (a->file_size / 40);
  if (assemble_redraw_step < 65536) assemble_redraw_step = 65536;
  size_t next_redraw_at = assemble_redraw_step;

  for (int i = 0; i < n; i++) {
    ArcEntry *e = &a->entries[i];
    if (!e->override_data) {
      /* Unchanged entry: fread directly into the output buffer at wpos.
       * Single fread -- no intermediate copy buffer. */
      if (wpos + e->entry_length > cap) goto fail_assemble;
      if (fseek(a->f, (long)e->entry_offset, SEEK_SET) != 0) goto fail_assemble;
      if (fread(out + wpos, 1, e->entry_length, a->f) != e->entry_length)
        goto fail_assemble;
      /* Re-key in-place if the blob shifted position. */
      if ((size_t)e->entry_offset != wpos)
        arc_rekey((int)e->entry_offset, (int)wpos, (int)e->entry_length, out + wpos);
      new_offsets[i] = (uint32_t)wpos;
      new_lengths[i] = e->entry_length;
      wpos += e->entry_length;
    } else {
      /* Override: use the pre-compressed buffer produced by the worker. */
      int sj = entry_to_slot[i];
      uLong gz_len = (sj >= 0) ? slots[sj].gz_len : 0;
      if (!gz_len) goto fail_assemble;
      size_t blob_len = 4 + (size_t)gz_len;
      if (wpos + blob_len > cap) goto fail_assemble;
      uint8_t *blob_out = out + wpos;
      uint32_t unc_be = bswap32((uint32_t)e->override_size);
      memcpy(blob_out, &unc_be, 4);
      memcpy(blob_out + 4, slots[sj].gz_buf, gz_len);
      arc_xor((int)wpos, (int)blob_len, blob_out);
      new_offsets[i] = (uint32_t)wpos;
      new_lengths[i] = (uint32_t)blob_len;
      wpos += blob_len;
    }

    assembled_bytes += (e->override_data ? new_lengths[i] : e->entry_length);
    if (assembled_bytes >= next_redraw_at || i == n - 1) {
      next_redraw_at = assembled_bytes + assemble_redraw_step;
      double af = (double)assembled_bytes / (double)(a->file_size ? a->file_size : 1);
      if (af > 1.0) af = 1.0;
      cache_progress_update(bp1 + (bp2 - bp1) * (float)af);
    }
  }
  cache_progress_update(bp2);

  /* Build + compress + encode the new index. */
  {
    size_t pso          = a->path_section_off;
    size_t strings_size = (a->idx_size > pso) ? (a->idx_size - pso) : 0;
    size_t idx_body_sz  = 4 + (size_t)n * 12 + strings_size;
    uint8_t *idx_body   = (uint8_t *)malloc(idx_body_sz);
    if (!idx_body) goto fail_assemble;
    uint32_t ec = (uint32_t)n;
    memcpy(idx_body, &ec, 4);
    size_t tbl = 4;
    for (int i = 0; i < n; i++) {
      uint32_t pno = a->entries[i].path_name_offset;
      memcpy(idx_body + tbl,     &pno,            4);
      memcpy(idx_body + tbl + 4, &new_offsets[i], 4);
      memcpy(idx_body + tbl + 8, &new_lengths[i], 4);
      tbl += 12;
    }
    memcpy(idx_body + tbl, a->idx_buf + pso, strings_size);

    if (wpos + 4 + compressBound(idx_body_sz) + 32 > cap) { free(idx_body); goto fail_assemble; }
    uint32_t new_hdr_off  = (uint32_t)wpos;
    uint8_t *idx_out      = out + wpos;
    uLong gz_idx_len = gz_compress_into(idx_body, (uLong)idx_body_sz,
                                         idx_out + 4, cap - wpos - 4);
    free(idx_body);
    if (!gz_idx_len) goto fail_assemble;
    uint32_t unc_idx_be = bswap32((uint32_t)idx_body_sz);
    memcpy(idx_out, &unc_idx_be, 4);
    size_t idx_blob_len = 4 + (size_t)gz_idx_len;
    arc_xor((int)wpos, (int)idx_blob_len, idx_out);
    wpos += idx_blob_len;

    uint8_t hdr[16];
    memcpy(hdr, "ARC1", 4);
    uint32_t fsz = (uint32_t)wpos, comp_len = (uint32_t)idx_blob_len;
    memcpy(hdr+4, &fsz,          4);
    memcpy(hdr+8, &new_hdr_off,  4);
    memcpy(hdr+12,&comp_len,     4);
    arc_xor(0, 16, hdr);
    memcpy(out, hdr, 16);
  }

  for (int i = 0; i < n_overrides; i++) free(slots[i].gz_buf);
  free(slots);
  free(entry_to_slot);
  free(new_offsets); free(new_lengths);
  *out_size = wpos;
  *out_save_start_frac = bp2;
  return out;

fail_assemble:
  debugPrintf("modpack: arc_rebuild_threaded assemble failed\n");
  cache_progress_finish();
  for (int i = 0; i < n_overrides; i++) free(slots[i].gz_buf);
  free(slots);
  free(entry_to_slot);
  free(new_offsets); free(new_lengths); free(out);
  return NULL;
}

/* ==========================================================================
 * Persistent cache
 *
 * Cache file layout:
 *   [0..3]   magic "CTMC"
 *   [4..7]   version (uint32 LE) = 2
 *   [8..11]  num_sources (uint32 LE): number of SrcDesc pairs that follow
 *   [12..12+num_sources*16-1]: source descriptors, each:
 *     [0..7]  fingerprint (uint64 LE)
 *       .ctp files : st_size cast to uint64
 *       folder mods: recursive XOR of (st_size ^ (uint32)st_mtime) over all
 *                    regular files inside the folder
 *     [8..15] mtime (int64 LE) -- st_mtime for .ctp; 0 for folders
 *   [12+N*16..end]: raw patched resources.bin bytes
 *
 * Sources recorded: [0] = resources.bin, then one entry per mod source
 * (sorted by path).  Empty folders (fingerprint == 0) are excluded entirely.
 *
 * All fingerprints are computed ONCE during the candidate scan in the public
 * API and stored in SrcPath.desc.  cache_load and cache_save read them from
 * there directly -- no repeated stat or directory walk on cache hit.
 * ========================================================================== */

#define CACHE_MAGIC     "CTMC"
#define CACHE_VER       2u
#define CACHE_HDR_FIXED 12   /* magic(4) + version(4) + num_sources(4) */
#define CACHE_SRC_SZ    16   /* fingerprint(8) + mtime(8) */

typedef struct {
  uint64_t fingerprint;
  int64_t  mtime;       /* st_mtime for files; 0 for dirs */
} SrcDesc;

/* --------------------------------------------------------------------------
 * folder_fingerprint: recursively XOR (st_size ^ (uint32)st_mtime) for every
 * regular file inside dir_path.  Returns 0 for empty/missing directories.
 * -------------------------------------------------------------------------- */
static uint64_t folder_fingerprint(const char *dir_path) {
  DIR *d = opendir(dir_path);
  if (!d) return 0;
  uint64_t fp = 0;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (ent->d_name[0] == '.') continue;
    char full[1024];
    snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);
    struct stat st;
    if (stat(full, &st) != 0) continue;
    if (S_ISREG(st.st_mode)) {
      fp ^= (uint64_t)st.st_size ^ (uint64_t)(uint32_t)st.st_mtime;
    } else if (S_ISDIR(st.st_mode)) {
      fp ^= folder_fingerprint(full);
    }
  }
  closedir(d);
  return fp;
}

/* --------------------------------------------------------------------------
 * SrcPath: one candidate mod source with its fingerprint pre-computed once
 * during the scan phase.  cache_load and cache_save read .desc directly
 * without touching the filesystem again.
 * -------------------------------------------------------------------------- */
typedef struct {
  char    path[1024];
  int     is_dir;
  SrcDesc desc;   /* pre-computed in scan; valid for the lifetime of the call */
} SrcPath;

static int srcpath_cmp(const void *a, const void *b) {
  return strcmp(((const SrcPath *)a)->path, ((const SrcPath *)b)->path);
}

/* --------------------------------------------------------------------------
 * cache_load: validate header against pre-computed SrcPath.desc values.
 * No stat or directory walk happens here -- all fingerprints come from desc.
 * -------------------------------------------------------------------------- */
static uint8_t *cache_load(const char *cache_path,
                             const SrcDesc *arc_desc,
                             const SrcPath *srcs, int nsrcs,
                             size_t *out_size) {
  FILE *f = fopen(cache_path, "rb");
  if (!f) return NULL;

  uint8_t hdr[CACHE_HDR_FIXED];
  if (fread(hdr, 1, CACHE_HDR_FIXED, f) != CACHE_HDR_FIXED) goto miss;
  if (memcmp(hdr, CACHE_MAGIC, 4) != 0) goto miss;
  uint32_t ver, num_sources;
  memcpy(&ver,        hdr+4, 4);
  memcpy(&num_sources,hdr+8, 4);
  if (ver != CACHE_VER) goto miss;
  if ((int)num_sources != nsrcs + 1) goto miss;

  /* Compare resources.bin descriptor (pre-computed, no stat here). */
  {
    uint8_t sd[CACHE_SRC_SZ];
    if (fread(sd, 1, CACHE_SRC_SZ, f) != CACHE_SRC_SZ) goto miss;
    uint64_t c_fp; int64_t c_mtime;
    memcpy(&c_fp,    sd+0, 8);
    memcpy(&c_mtime, sd+8, 8);
    if (c_fp != arc_desc->fingerprint || c_mtime != arc_desc->mtime) goto miss;
  }

  /* Compare each mod source descriptor (pre-computed, no stat/walk here). */
  for (int i = 0; i < nsrcs; i++) {
    uint8_t sd[CACHE_SRC_SZ];
    if (fread(sd, 1, CACHE_SRC_SZ, f) != CACHE_SRC_SZ) goto miss;
    uint64_t c_fp; int64_t c_mtime;
    memcpy(&c_fp,    sd+0, 8);
    memcpy(&c_mtime, sd+8, 8);
    if (c_fp != srcs[i].desc.fingerprint || c_mtime != srcs[i].desc.mtime) goto miss;
  }

  /* Header matched -- load the cached archive. */
  fseek(f, 0, SEEK_END);
  long fsz = ftell(f);
  long data_off = (long)(CACHE_HDR_FIXED + (nsrcs + 1) * CACHE_SRC_SZ);
  long data_len = fsz - data_off;
  if (data_len <= 0) goto miss;

  uint8_t *buf = (uint8_t *)malloc((size_t)data_len);
  if (!buf) goto miss;
  fseek(f, data_off, SEEK_SET);
  if ((long)fread(buf, 1, (size_t)data_len, f) != data_len) { free(buf); goto miss; }
  fclose(f);
  *out_size = (size_t)data_len;
  debugPrintf("modpack: cache hit (%zu bytes)\n", (size_t)data_len);
  return buf;

miss:
  fclose(f);
  return NULL;
}

/* --------------------------------------------------------------------------
 * cache_save: write header using pre-computed SrcPath.desc values.
 * No stat or directory walk happens here either.
 *
 * progress_start_frac is where arc_rebuild_threaded's splash left off
 * (end of assemble); this carries it the rest of the way to 1.0 across the
 * actual SD card write, which on a cache-miss rebuild is often the single
 * slowest step and was previously invisible (the bar would sit frozen at
 * 100% while this ran). Always finishes the splash before returning,
 * success or not, since this is the last step of the rebuild.
 * -------------------------------------------------------------------------- */
static void cache_save(const char *cache_path,
                        const SrcDesc *arc_desc,
                        const SrcPath *srcs, int nsrcs,
                        const uint8_t *data, size_t data_len,
                        float progress_start_frac) {
  FILE *f = fopen(cache_path, "wb");
  if (!f) { cache_progress_finish(); return; }

  uint8_t hdr[CACHE_HDR_FIXED];
  memcpy(hdr, CACHE_MAGIC, 4);
  uint32_t ver = CACHE_VER;
  uint32_t num = (uint32_t)(nsrcs + 1);
  memcpy(hdr+4, &ver, 4);
  memcpy(hdr+8, &num, 4);
  fwrite(hdr, 1, CACHE_HDR_FIXED, f);

  /* Write resources.bin descriptor (pre-computed). */
  {
    uint8_t sdbuf[CACHE_SRC_SZ];
    memcpy(sdbuf+0, &arc_desc->fingerprint, 8);
    memcpy(sdbuf+8, &arc_desc->mtime,       8);
    fwrite(sdbuf, 1, CACHE_SRC_SZ, f);
  }

  /* Write mod source descriptors (pre-computed). */
  for (int i = 0; i < nsrcs; i++) {
    uint8_t sdbuf[CACHE_SRC_SZ];
    memcpy(sdbuf+0, &srcs[i].desc.fingerprint, 8);
    memcpy(sdbuf+8, &srcs[i].desc.mtime,       8);
    fwrite(sdbuf, 1, CACHE_SRC_SZ, f);
  }

  /* The header/descriptors above are a few hundred bytes at most -- not
   * worth tracking. The payload is everything, so it's written in chunks
   * with a splash redraw between each one. */
  const size_t CHUNK = 2u << 20; /* 2MB */
  size_t written = 0;
  while (written < data_len) {
    size_t n = data_len - written;
    if (n > CHUNK) n = CHUNK;
    if (fwrite(data + written, 1, n, f) != n) break;
    written += n;
    double sf = (double)written / (double)data_len;
    cache_progress_update(progress_start_frac + (1.0f - progress_start_frac) * (float)sf);
  }

  fclose(f);
  cache_progress_update(1.0f);
  cache_progress_finish();
  debugPrintf("modpack: cache saved (%zu bytes)\n", data_len);
}

/* ==========================================================================
 * Public API
 * ========================================================================== */

int modpack_get_patched_resources(const char *real_path,
                                  uint8_t   **out_buf,
                                  size_t     *out_len) {
  if (!config.mods_dir[0]) return 0;

  /* -------------------------------------------------------------------------
   * Phase 1: Scan mods_dir, compute fingerprints for all candidates upfront.
   *
   * Each SrcPath gets its .desc filled here and ONLY here.  All subsequent
   * operations (cache_load, cache_save) read from .desc without touching the
   * filesystem again.  This means on a cache hit the total filesystem cost is:
   *   stat(resources.bin) + stat each .ctp + folder_fingerprint each folder
   * all done exactly once, then one sequential read of the cache file.
   * No repeated stat or walk happens inside cache_load or cache_save.
   * ---------------------------------------------------------------------- */
  SrcPath *candidates = (SrcPath *)malloc(256 * sizeof(SrcPath));
  if (!candidates) return 0;
  int n_candidates = 0;

  {
    DIR *d = opendir(config.mods_dir);
    if (!d) { free(candidates); return 0; }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && n_candidates < 256) {
      if (ent->d_name[0] == '.') continue;
      char full[1024];
      snprintf(full, sizeof(full), "%s/%s", config.mods_dir, ent->d_name);
      struct stat st;
      if (stat(full, &st) != 0) continue;
      size_t nl = strlen(ent->d_name);
      int is_ctp = (nl >= 5 && strcmp(ent->d_name + nl - 4, ".ctp") == 0);
      int is_dir = S_ISDIR(st.st_mode);
      if (!is_ctp && !is_dir) continue;

      SrcDesc desc;
      if (is_dir) {
        desc.fingerprint = folder_fingerprint(full);
        desc.mtime       = 0;
        /* Skip empty directories entirely -- they have no effect on output
         * and should never cause a cache miss or rebuild. */
        if (desc.fingerprint == 0) {
          debugPrintf("modpack: skipping empty folder '%s'\n", ent->d_name);
          continue;
        }
      } else {
        desc.fingerprint = (uint64_t)st.st_size;
        desc.mtime       = (int64_t)st.st_mtime;
      }

      snprintf(candidates[n_candidates].path, sizeof(candidates[0].path), "%s", full);
      candidates[n_candidates].is_dir = is_dir;
      candidates[n_candidates].desc   = desc;
      n_candidates++;
    }
    closedir(d);
    if (n_candidates == 0) { free(candidates); return 0; }
    qsort(candidates, n_candidates, sizeof(SrcPath), srcpath_cmp);
  }

  /* Compute resources.bin descriptor once. */
  SrcDesc arc_desc;
  {
    struct stat st;
    if (stat(real_path, &st) != 0) { free(candidates); return 0; }
    arc_desc.fingerprint = (uint64_t)st.st_size;
    arc_desc.mtime       = (int64_t)st.st_mtime;
  }

  /* Build cache path. */
  char cache_path[1280];
  snprintf(cache_path, sizeof(cache_path), "%s/.modcache", config.mods_dir);

  /* -------------------------------------------------------------------------
   * Phase 2: Try cache.  No filesystem work beyond what Phase 1 already did.
   * ---------------------------------------------------------------------- */
  size_t cached_size = 0;
  uint8_t *cached = cache_load(cache_path, &arc_desc,
                                candidates, n_candidates, &cached_size);
  if (cached) {
    free(candidates);
    *out_buf = cached;
    *out_len = cached_size;
    return 1;
  }

  /* -------------------------------------------------------------------------
   * Phase 3: Cache miss -- full rebuild.
   * ---------------------------------------------------------------------- */
  debugPrintf("modpack: cache miss -- rebuilding\n");

  Arc a;
  if (!arc_load(&a, real_path)) {
    debugPrintf("modpack: failed to load '%s'\n", real_path);
    free(candidates); return 0;
  }

  arc_apply_mods(&a, config.mods_dir);

  int any = 0;
  for (int i = 0; i < a.entry_count; i++)
    if (a.entries[i].override_data) { any = 1; break; }
  if (!any) {
    debugPrintf("modpack: no entries matched -- no patch applied\n");
    arc_free(&a); free(candidates); return 0;
  }

  size_t built_size = 0;
  float save_start_frac = 0.0f;
  uint8_t *built = arc_rebuild_threaded(&a, &built_size, &save_start_frac);
  arc_free(&a);

  if (!built) {
    debugPrintf("modpack: rebuild failed\n");
    free(candidates); return 0;
  }

  cache_save(cache_path, &arc_desc, candidates, n_candidates, built, built_size, save_start_frac);
  free(candidates);

  *out_buf = built;
  *out_len = built_size;
  debugPrintf("modpack: ready (%zu bytes)\n", built_size);
  return 1;
}