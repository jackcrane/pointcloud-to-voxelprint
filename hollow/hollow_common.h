#ifndef HOLLOW_COMMON_H
#define HOLLOW_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HOLLOW_DEFAULT_LOG_INTERVAL UINT64_C(100000)

typedef struct {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t a;
} HollowColor;

typedef struct {
  const char *config_path;
  const char *input_dir;
  const char *output_dir;
  uint32_t dist_positive_x;
  uint32_t dist_negative_x;
  uint32_t dist_positive_y;
  uint32_t dist_negative_y;
  uint32_t dist_positive_z;
  uint32_t dist_negative_z;
  HollowColor *colors_for_removal;
  size_t colors_for_removal_count;
  HollowColor destination_color;
  bool treat_edges_as_blockers;
  uint64_t log_interval;
} HollowOptions;

#ifdef __cplusplus
extern "C" {
#endif

void free_hollow_options(HollowOptions *options);
int parse_hollow_options(int argc, char **argv, HollowOptions *options_out);
int run_hollow(const HollowOptions *options);

#ifdef __cplusplus
}
#endif

#endif
