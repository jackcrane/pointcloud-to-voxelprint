#ifndef FILL_REGION_COMMON_H
#define FILL_REGION_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  double x;
  double y;
  double z;
  bool has_z;
} FillRegionPoint;

typedef struct {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t a;
} FillRegionColor;

typedef enum {
  FILL_REGION_MODE_XY = 0,
  FILL_REGION_MODE_XYZ = 1,
} FillRegionMode;

typedef struct {
  const char *config_path;
  const char *input_dir;
  const char *output_dir;
  bool has_layer_first;
  bool has_layer_last;
  uint32_t layer_first;
  uint32_t layer_last;
  FillRegionColor color;
  FillRegionPoint *points;
  size_t point_count;
  FillRegionMode mode;
} FillRegionOptions;

#ifdef __cplusplus
extern "C" {
#endif

void free_fill_region_options(FillRegionOptions *options);
int parse_fill_region_options(int argc, char **argv, FillRegionOptions *options_out);
int run_fill_region(const FillRegionOptions *options);

#ifdef __cplusplus
}
#endif

#endif
