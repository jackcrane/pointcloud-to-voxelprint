#ifndef SHADOW_COMMON_H
#define SHADOW_COMMON_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t a;
} ShadowColor;

typedef enum {
  SHADOW_FROM_BOTTOM = 0,
  SHADOW_FROM_TOP = 1,
} ShadowDirection;

typedef struct {
  const char *config_path;
  const char *input_dir;
  const char *output_dir;
  ShadowColor set_color;
  ShadowColor replace_color;
  ShadowDirection direction;
} ShadowOptions;

#ifdef __cplusplus
extern "C" {
#endif

void free_shadow_options(ShadowOptions *options);
int parse_shadow_options(int argc, char **argv, ShadowOptions *options_out);
int run_shadow(const ShadowOptions *options);

#ifdef __cplusplus
}
#endif

#endif
