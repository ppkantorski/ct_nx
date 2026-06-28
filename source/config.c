/* config.c -- simple configuration parser
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
  CONFIG_VAR_INT(screen_width); \
  CONFIG_VAR_INT(screen_height); \
  CONFIG_VAR_STR(language); \
  CONFIG_VAR_INT(gl_threaded); \
  CONFIG_VAR_INT(game_font); \
  CONFIG_VAR_STR(game_font_path); \
  CONFIG_VAR_FLOAT(game_font_scale); \
  CONFIG_VAR_FLOAT(game_font_xscale); \
  CONFIG_VAR_INT(text_shadow); \
  CONFIG_VAR_INT(text_shadow_alpha); \
  CONFIG_VAR_FLOAT(text_shadow_scale);

Config config;
static int config_needs_rewrite = 0;

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

int read_config(const char *file) {
  char line[1024] = { 0 };

  memset(&config, 0, sizeof(Config));
  config_needs_rewrite = 0;
  config.screen_width = -1; // auto
  config.screen_height = -1;
  strlcpy(config.language, LANG_DEFAULT, sizeof(config.language));
  config.gl_threaded = 1;   // offload GL submission to a second core by default
  config.game_font = 1;     // try the in-game font for system-font labels (testing)
  config.game_font_path[0] = 0; // empty: probe common asset locations
  config.game_font_scale = 1.0f; // 1.0 = auto-fit cap height to the shared font
  config.game_font_xscale = 1.0f; // 1.0 = no horizontal squeeze
  config.text_shadow = 1;         // crisp CT-style drop shadow on system-font labels
  config.text_shadow_alpha = 230; // shadow opacity 0..255 (black)
  config.text_shadow_scale = 1.0f; // offset multiplier (auto offset scales w/ size)

  FILE *f = fopen(file, "r");
  if (f == NULL)
    return -1;

  // parse lines of the forms
  // <spaces> # <whatever> \n
  // <spaces> NAME <spaces> VALUE <spaces> \n
  do {
    char *name = NULL, *value = NULL, *tmp = NULL;
    if (fgets(line, sizeof(line), f) != NULL) {
      name = line;
      // trim name
      while (*name && isspace((int)*name)) ++name;
      if (name[0] == '#') continue; // skip comments
      for (tmp = name; *tmp && !isspace((int)*tmp); ++tmp);
      // if tmp points to the end of the string, there's no value to parse
      if (*tmp != 0) {
        *tmp = 0;
        // value is next; trim value
        for (value = tmp + 1; *value && isspace((int)*value); ++value);
        for (tmp = value + strlen(value) - 1; tmp >= value && isspace((int)*tmp); --tmp) *tmp = 0;
        // got key value pair
        parse_var(name, value);
      }
    }
  } while (!feof(f));

  fclose(f);

  // language must never be empty (a malformed line could blank it); the rest of
  // the engine indexes localisation tables with it.
  if (!config.language[0])
    strlcpy(config.language, LANG_DEFAULT, sizeof(config.language));

  return config_needs_rewrite ? 1 : 0;
}

int write_config(const char *file) {
  FILE *f = fopen(file, "w");
  if (f == NULL)
    return -1;

  #define CONFIG_VAR_INT(var) fprintf(f, "%s %d\n", #var, config.var)
  #define CONFIG_VAR_FLOAT(var) fprintf(f, "%s %g\n", #var, config.var)
  // write strings verbatim (empty stays empty); language is guaranteed non-empty
  // by read_config, so it never needs the old LANG_DEFAULT fallback here.
  #define CONFIG_VAR_STR(var) fprintf(f, "%s %s\n", #var, config.var)
  CONFIG_VARS
  #undef CONFIG_VAR_INT
  #undef CONFIG_VAR_FLOAT
  #undef CONFIG_VAR_STR

  fclose(f);

  return 0;
}