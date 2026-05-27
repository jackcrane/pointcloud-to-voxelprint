#ifndef ADD_IMAGES_COMMON_H
#define ADD_IMAGES_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  const char *src_path;
  uint32_t x;
  uint32_t y;
  bool has_width;
  bool has_height;
  uint32_t width;
  uint32_t height;
  bool invert;
} AddImagesOverlay;

typedef struct {
  const char *config_path;
  const char *input_dir;
  const char *output_dir;
  bool has_layer_first;
  bool has_layer_last;
  uint32_t layer_first;
  uint32_t layer_last;
  AddImagesOverlay *overlays;
  size_t overlay_count;
} AddImagesOptions;

#ifdef __cplusplus
extern "C" {
#endif

void free_add_images_options(AddImagesOptions *options);
int parse_add_images_options(int argc, char **argv, AddImagesOptions *options_out);
int run_add_images(const AddImagesOptions *options);

#ifdef __cplusplus
}
#endif

#endif
