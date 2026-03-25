#ifndef CROP_COMMON_H
#define CROP_COMMON_H

#include <stdint.h>

typedef struct {
  const char *input_path;
  const char *output_path;
  double min_x;
  double max_x;
  double min_y;
  double max_y;
  double min_z;
  double max_z;
  uint64_t log_interval;
} CropOptions;

#endif
