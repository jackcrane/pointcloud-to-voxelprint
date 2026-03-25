#ifndef ROTATE_COMMON_H
#define ROTATE_COMMON_H

#include <stdint.h>

typedef enum {
  ROTATE_AXIS_X = 0,
  ROTATE_AXIS_Y = 1,
  ROTATE_AXIS_Z = 2,
} RotateAxis;

typedef struct {
  const char *input_path;
  const char *output_path;
  double centroid_x;
  double centroid_y;
  double centroid_z;
  double angle_degrees;
  RotateAxis axis;
  uint64_t log_interval;
} RotateOptions;

#endif
