#ifndef XSECTION_COMMON_H
#define XSECTION_COMMON_H

#include <stdint.h>

#include "slice_common.h"

typedef enum {
  XSECTION_PLANE_XZ = 0,
  XSECTION_PLANE_YZ = 1,
} XSectionPlane;

typedef struct {
  SliceOptions slice_options;
  XSectionPlane plane;
  uint32_t dist;
  const char *output_path;
} XSectionOptions;

int parse_xsection_options(int argc, char **argv, XSectionOptions *options_out);
int run_xsection(const XSectionOptions *options);

#endif
