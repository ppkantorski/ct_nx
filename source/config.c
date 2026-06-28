/* config.c -- lightweight single-section INI configuration parser
 *
 * Reads/writes config.ini as:
 *   [config]
 *   key = value
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "config.h"

#define CONFIG_VARS \
  CONFIG_VAR_INT(screen_width_handheld); \
  CONFIG_VAR_INT(screen_height_handheld); \
  CONFIG_VAR_INT(screen_width_docked); \
  CONFIG_VAR_INT(screen_height_docked); \
  CONFIG_VAR_STR(language); \
  CONFIG_VAR_INT(gl_threaded); \
  CONFIG_VAR_INT(gl_no_error); \
  CONFIG_VAR_INT(game_font); \
  CONFIG_VAR_STR(game_font_path); \
  CONFIG_VAR_FLOAT(game_font_scale); \
  CONFIG_VAR_FLOAT(game_font_xscale); \
  CONFIG_VAR_INT(text_shadow); \
  CONFIG_VAR_INT(text_shadow_alpha); \
  CONFIG_VAR_FLOAT(text_shadow_scale);

Config config;

// actual screen size that is in use right now
int screen_width = 1280;
int screen_height = 720;

static inline void parse_var(const char *name, const char *value) {
  #define CONFIG_VAR_INT(var) if (!strcmp(name, #var)) { config.var = atoi(value); return; }
  #define CONFIG_VAR_FLOAT(var) if (!strcmp(name, #var)) { config.var = atof(value); return; }
  #define CONFIG_VAR_STR(var) if (!strcmp(name, #var)) { strlcpy(config.var, value, sizeof(config.var)); return; }
  CONFIG_VARS
  #undef CONFIG_VAR_INT
  #undef CONFIG_VAR_FLOAT
  #undef CONFIG_VAR_STR
}

// Trims leading/trailing whitespace (and a trailing \r, for files saved with
// CRLF line endings) in place. Returns the start of the trimmed string -- the
// original pointer may no longer point at it, so always use the return value.
static char *trim(char *s) {
  while (*s && isspace((unsigned char)*s)) ++s;
  if (!*s) return s;
  char *end = s + strlen(s) - 1;
  while (end > s && (isspace((unsigned char)*end) || *end == '\r')) *end-- = 0;
  return s;
}

static void set_defaults(void) {
  memset(&config, 0, sizeof(Config));
  config.screen_width_handheld = -1;  // auto: 1280x720
  config.screen_height_handheld = -1;
  config.screen_width_docked = -1;    // auto: 1920x1080
  config.screen_height_docked = -1;
  strlcpy(config.language, LANG_DEFAULT, sizeof(config.language));
  config.gl_threaded = 1;   // offload GL submission to a second core by default
  config.gl_no_error = 1;   // skip mesa's GL call validation by default
  config.game_font = 1;     // try the in-game font for system-font labels (testing)
  config.game_font_path[0] = 0; // empty: probe common asset locations
  config.game_font_scale = 1.0f; // 1.0 = auto-fit cap height to the shared font
  config.game_font_xscale = 1.0f; // 1.0 = no horizontal squeeze
  config.text_shadow = 1;         // crisp CT-style drop shadow on system-font labels
  config.text_shadow_alpha = 230; // shadow opacity 0..255 (black)
  config.text_shadow_scale = 1.0f; // offset multiplier (auto offset scales w/ size)
}

// One-time migration from the old flat "key value" config.txt, for anyone
// upgrading from before config.ini existed. Deliberately separate from the
// INI parser below -- it's the old format's own simpler whitespace-split
// rule, not '='-split, so it has to be read on its own terms. Returns 0 if a
// legacy file was found and at least opened (regardless of how many lines in
// it parsed), -1 if there was nothing to migrate.
static int read_legacy_txt(const char *file) {
  char line[1024];
  FILE *f = fopen(file, "r");
  if (f == NULL)
    return -1;

  while (fgets(line, sizeof(line), f) != NULL) {
    char *name = trim(line);
    if (!*name || *name == '#')
      continue; // blank line or comment

    char *tmp = name;
    while (*tmp && !isspace((unsigned char)*tmp)) ++tmp;
    if (!*tmp) continue; // no value on this line
    *tmp = 0;
    char *value = trim(tmp + 1);
    parse_var(name, value);
  }

  fclose(f);
  return 0;
}

// Lightweight single-section INI reader: only lines inside "[config]" are
// honoured (anything in another section, or before any header at all, is
// ignored rather than guessed at). This project only ever has the one
// section, so it deliberately doesn't build a generic section/key map the
// way a multi-document INI library would -- known keys are dispatched
// straight into the Config struct via parse_var, same as the old format did.
int read_config(const char *file) {
  char line[1024];
  char section[32] = { 0 };

  set_defaults();

  FILE *f = fopen(file, "r");
  if (f == NULL) {
    // config.ini doesn't exist yet -- if there's an old config.txt, migrate
    // its values forward instead of silently resetting everything to
    // defaults, then retire it so it's never read again.
    if (read_legacy_txt(CONFIG_LEGACY_NAME) == 0) {
      rename(CONFIG_LEGACY_NAME, CONFIG_LEGACY_NAME ".bak");
      return 0;
    }
    return -1;
  }

  while (fgets(line, sizeof(line), f) != NULL) {
    char *l = trim(line);
    if (!*l || *l == ';' || *l == '#')
      continue; // blank line or comment

    const size_t len = strlen(l);
    if (l[0] == '[' && l[len - 1] == ']') {
      l[len - 1] = 0;
      strlcpy(section, l + 1, sizeof(section));
      continue;
    }

    if (strcmp(section, "config") != 0)
      continue; // not our section -- ignore (forward-compat with anything
                // else that might one day share this file)

    char *eq = strchr(l, '=');
    if (!eq) continue; // no '=' -- malformed line, skip rather than guess
    *eq = 0;
    char *key = trim(l);
    char *value = trim(eq + 1);
    if (*key) parse_var(key, value);
  }

  fclose(f);

  // language must never be empty (a malformed line could blank it); the rest of
  // the engine indexes localisation tables with it.
  if (!config.language[0])
    strlcpy(config.language, LANG_DEFAULT, sizeof(config.language));

  return 0;
}

int write_config(const char *file) {
  FILE *f = fopen(file, "w");
  if (f == NULL)
    return -1;

  fprintf(f, "[config]\n");

  #define CONFIG_VAR_INT(var) fprintf(f, "%s = %d\n", #var, config.var)
  #define CONFIG_VAR_FLOAT(var) fprintf(f, "%s = %g\n", #var, config.var)
  // write strings verbatim (empty stays empty); language is guaranteed non-empty
  // by read_config, so it never needs the old LANG_DEFAULT fallback here.
  #define CONFIG_VAR_STR(var) fprintf(f, "%s = %s\n", #var, config.var)
  CONFIG_VARS
  #undef CONFIG_VAR_INT
  #undef CONFIG_VAR_FLOAT
  #undef CONFIG_VAR_STR

  fclose(f);

  return 0;
}