#ifndef SLICE_COMMON_H
#define SLICE_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SLICE_DEFAULT_DPI 300.0
#define SLICE_DEFAULT_LAYER_HEIGHT_NM 27000.0
#define SLICE_NM_PER_INCH 25400000.0
#define SLICE_DEFAULT_MULTIPLIER 1.0
#define SLICE_BASE_X_IN 2.5
#define SLICE_BASE_Y_IN 1.0
#define SLICE_BASE_Z_IN 0.75
#define SLICE_DEFAULT_VOXEL_RADIUS_INCHES 0.01
#define SLICE_DEFAULT_PADDING_RATIO 0.0
#define SLICE_DEFAULT_LONGEST_SIDE_IN 3.0
#define SLICE_DEFAULT_LOG_INTERVAL UINT64_C(100000)

typedef struct {
  const char *input_path;
  const char *output_dir;
  const char *config_path;
  const char *kd_cache_path;
  double dpi;
  double layer_height_nm;
  double multiplier;
  double x_in;
  double y_in;
  double z_in;
  bool x_in_set;
  bool y_in_set;
  bool z_in_set;
  double longest_side_in;
  double voxel_radius_x_positive_inches;
  double voxel_radius_x_negative_inches;
  double voxel_radius_y_positive_inches;
  double voxel_radius_y_negative_inches;
  double voxel_radius_z_positive_inches;
  double voxel_radius_z_negative_inches;
  double padding_ratio;
  uint64_t log_interval;
} SliceOptions;

typedef struct {
  double x;
  double y;
  double z;
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t a;
  bool has_color;
  bool has_alpha;
} SlicePoint;

int parse_slice_options(int argc, char **argv, SliceOptions *options_out);
int run_slice(const SliceOptions *options);

#endif
