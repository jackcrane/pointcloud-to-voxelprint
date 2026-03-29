#include "hollow_pipeline.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

typedef struct {
  uint32_t width;
  uint32_t height;
  size_t stride;
  uint8_t *pixels;
} Image;

typedef struct {
  uint32_t layer_index;
  char *name;
  char *path;
} LayerFile;

typedef struct {
  uint64_t interval;
  uint64_t next_tick;
  uint64_t completed;
  uint64_t total;
  struct timespec start_time;
} ProgressTracker;

static int mkdir_recursive(const char *path);
static int collect_layer_files(const char *directory, LayerFile **layers_out, size_t *count_out);
static void free_layer_files(LayerFile *layers, size_t count);
static int compare_layer_files(const void *lhs, const void *rhs);
static bool parse_layer_file_name(const char *name, uint32_t *index_out);
static int build_output_path(const char *directory, const char *name, char **path_out);
static int init_image(Image *image, uint32_t width, uint32_t height);
static void free_image(Image *image);
static bool colors_match(const uint8_t *pixel, const HollowColor *color);
static bool color_is_removable(const uint8_t *pixel, const HollowOptions *options);
static void set_pixel(uint8_t *pixel, const HollowColor *color);
static size_t voxel_index(
    size_t layer,
    uint32_t y,
    uint32_t x,
    uint32_t width,
    uint32_t height);
static void progress_begin(ProgressTracker *tracker, uint64_t interval, uint64_t total);
static void progress_advance(ProgressTracker *tracker, uint64_t amount);
static void progress_finish(ProgressTracker *tracker);
static double elapsed_seconds_since(const struct timespec *start_time);
static void format_duration(double seconds, char *buffer, size_t buffer_size);
static void log_progress(const ProgressTracker *tracker);
static void run_positive_x_pass(
    const uint8_t *removable_map,
    uint8_t *keep_map,
    size_t layer_count,
    uint32_t width,
    uint32_t height,
    uint32_t threshold,
    bool treat_edges_as_blockers,
    ProgressTracker *progress);
static void run_negative_x_pass(
    const uint8_t *removable_map,
    uint8_t *keep_map,
    size_t layer_count,
    uint32_t width,
    uint32_t height,
    uint32_t threshold,
    bool treat_edges_as_blockers,
    ProgressTracker *progress);
static void run_positive_y_pass(
    const uint8_t *removable_map,
    uint8_t *keep_map,
    size_t layer_count,
    uint32_t width,
    uint32_t height,
    uint32_t threshold,
    bool treat_edges_as_blockers,
    ProgressTracker *progress);
static void run_negative_y_pass(
    const uint8_t *removable_map,
    uint8_t *keep_map,
    size_t layer_count,
    uint32_t width,
    uint32_t height,
    uint32_t threshold,
    bool treat_edges_as_blockers,
    ProgressTracker *progress);
static void run_positive_z_pass(
    const uint8_t *removable_map,
    uint8_t *keep_map,
    size_t layer_count,
    uint32_t width,
    uint32_t height,
    uint32_t threshold,
    bool treat_edges_as_blockers,
    ProgressTracker *progress);
static void run_negative_z_pass(
    const uint8_t *removable_map,
    uint8_t *keep_map,
    size_t layer_count,
    uint32_t width,
    uint32_t height,
    uint32_t threshold,
    bool treat_edges_as_blockers,
    ProgressTracker *progress);
static bool diagonal_pass_enabled(uint32_t distance_a, uint32_t distance_b);
static uint64_t diagonal_threshold_sq(uint32_t distance_a, uint32_t distance_b);
static bool diagonal_step_within_threshold(size_t step_count, uint64_t threshold_sq);
static void process_diagonal_line(
    const uint8_t *removable_map,
    uint8_t *keep_map,
    const size_t *line_indices,
    size_t line_length,
    uint64_t threshold_sq,
    bool treat_edges_as_blockers);
static void run_xy_diagonal_pass(
    const uint8_t *removable_map,
    uint8_t *keep_map,
    size_t layer_count,
    uint32_t width,
    uint32_t height,
    uint32_t distance_x,
    uint32_t distance_y,
    bool invert_y,
    bool reverse_line_direction,
    bool treat_edges_as_blockers,
    ProgressTracker *progress);
static void run_xz_diagonal_pass(
    const uint8_t *removable_map,
    uint8_t *keep_map,
    size_t layer_count,
    uint32_t width,
    uint32_t height,
    uint32_t distance_x,
    uint32_t distance_z,
    bool invert_z,
    bool reverse_line_direction,
    bool treat_edges_as_blockers,
    ProgressTracker *progress);
static void run_yz_diagonal_pass(
    const uint8_t *removable_map,
    uint8_t *keep_map,
    size_t layer_count,
    uint32_t width,
    uint32_t height,
    uint32_t distance_y,
    uint32_t distance_z,
    bool invert_y,
    bool reverse_line_direction,
    bool treat_edges_as_blockers,
    ProgressTracker *progress);
static uint32_t read_u32_be(const uint8_t *bytes);
static void write_u32_be(uint8_t *out, uint32_t value);
static uint32_t crc32_bytes(const uint8_t *bytes, size_t length);
static uint32_t adler32_bytes(const uint8_t *bytes, size_t length);
static int append_chunk(
    FILE *file,
    const char type[4],
    const uint8_t *data,
    uint32_t length);
static int inflate_stored_blocks(
    const uint8_t *compressed,
    size_t compressed_size,
    uint8_t *raw_out,
    size_t raw_size);
static int read_png(const char *path, Image *image_out);
static int write_png(const char *path, const Image *image);

int run_hollow(const HollowOptions *options) {
  LayerFile *layers = NULL;
  Image *images = NULL;
  uint8_t *removable_map = NULL;
  uint8_t *keep_map = NULL;
  size_t layer_count = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  size_t total_voxels = 0;
  uint64_t total_progress_units = 0;
  uint64_t active_pass_count = 0;
  ProgressTracker progress;
  memset(&progress, 0, sizeof(progress));

  if (options == NULL) {
    fprintf(stderr, "Missing hollow options.\n");
    return -1;
  }

  if (collect_layer_files(options->input_dir, &layers, &layer_count) != 0) {
    return -1;
  }
  if (layer_count == 0u) {
    fprintf(stderr, "No layer PNGs were found in %s\n", options->input_dir);
    free_layer_files(layers, layer_count);
    return -1;
  }
  if (mkdir_recursive(options->output_dir) != 0) {
    free_layer_files(layers, layer_count);
    return -1;
  }

  images = calloc(layer_count, sizeof(*images));
  if (images == NULL) {
    fprintf(stderr, "Failed to allocate image table.\n");
    free_layer_files(layers, layer_count);
    return -1;
  }

  for (size_t i = 0; i < layer_count; ++i) {
    if (read_png(layers[i].path, &images[i]) != 0) {
      for (size_t j = 0; j < i; ++j) {
        free_image(&images[j]);
      }
      free(images);
      free_layer_files(layers, layer_count);
      return -1;
    }

    if (i == 0u) {
      width = images[i].width;
      height = images[i].height;
    } else if (images[i].width != width || images[i].height != height) {
      fprintf(
          stderr,
          "Layer dimensions do not match. Expected %ux%u but %s is %ux%u.\n",
          width,
          height,
          layers[i].name,
          images[i].width,
          images[i].height);
      for (size_t j = 0; j <= i; ++j) {
        free_image(&images[j]);
      }
      free(images);
      free_layer_files(layers, layer_count);
      return -1;
    }
  }

  if (width == 0u || height == 0u) {
    fprintf(stderr, "Input layers must not be empty.\n");
    for (size_t i = 0; i < layer_count; ++i) {
      free_image(&images[i]);
    }
    free(images);
    free_layer_files(layers, layer_count);
    return -1;
  }

  if ((size_t) width > SIZE_MAX / (size_t) height ||
      (size_t) width * (size_t) height > SIZE_MAX / layer_count) {
    fprintf(stderr, "Voxel stack is too large to index in memory.\n");
    for (size_t i = 0; i < layer_count; ++i) {
      free_image(&images[i]);
    }
    free(images);
    free_layer_files(layers, layer_count);
    return -1;
  }
  total_voxels = layer_count * (size_t) width * (size_t) height;

  removable_map = calloc(total_voxels, sizeof(*removable_map));
  keep_map = calloc(total_voxels, sizeof(*keep_map));
  if (removable_map == NULL || keep_map == NULL) {
    fprintf(stderr, "Failed to allocate hollow voxel maps.\n");
    free(removable_map);
    free(keep_map);
    for (size_t i = 0; i < layer_count; ++i) {
      free_image(&images[i]);
    }
    free(images);
    free_layer_files(layers, layer_count);
    return -1;
  }

  active_pass_count += options->dist_positive_x > 0u ? 1u : 0u;
  active_pass_count += options->dist_negative_x > 0u ? 1u : 0u;
  active_pass_count += options->dist_positive_y > 0u ? 1u : 0u;
  active_pass_count += options->dist_negative_y > 0u ? 1u : 0u;
  active_pass_count += options->dist_positive_z > 0u ? 1u : 0u;
  active_pass_count += options->dist_negative_z > 0u ? 1u : 0u;
  active_pass_count += diagonal_pass_enabled(options->dist_positive_x, options->dist_positive_y) ? 1u : 0u;
  active_pass_count += diagonal_pass_enabled(options->dist_negative_x, options->dist_negative_y) ? 1u : 0u;
  active_pass_count += diagonal_pass_enabled(options->dist_positive_x, options->dist_negative_y) ? 1u : 0u;
  active_pass_count += diagonal_pass_enabled(options->dist_negative_x, options->dist_positive_y) ? 1u : 0u;
  active_pass_count += diagonal_pass_enabled(options->dist_positive_x, (uint32_t) options->dist_positive_z) ? 1u : 0u;
  active_pass_count += diagonal_pass_enabled(options->dist_negative_x, (uint32_t) options->dist_negative_z) ? 1u : 0u;
  active_pass_count += diagonal_pass_enabled(options->dist_positive_x, (uint32_t) options->dist_negative_z) ? 1u : 0u;
  active_pass_count += diagonal_pass_enabled(options->dist_negative_x, (uint32_t) options->dist_positive_z) ? 1u : 0u;
  active_pass_count += diagonal_pass_enabled(options->dist_positive_y, (uint32_t) options->dist_positive_z) ? 1u : 0u;
  active_pass_count += diagonal_pass_enabled(options->dist_negative_y, (uint32_t) options->dist_negative_z) ? 1u : 0u;
  active_pass_count += diagonal_pass_enabled(options->dist_negative_y, (uint32_t) options->dist_positive_z) ? 1u : 0u;
  active_pass_count += diagonal_pass_enabled(options->dist_positive_y, (uint32_t) options->dist_negative_z) ? 1u : 0u;
  total_progress_units = (uint64_t) total_voxels * (3u + active_pass_count);
  progress_begin(&progress, options->log_interval, total_progress_units);

  printf(
      "hollow: %s -> %s (%zu layers, %ux%u)\n",
      options->input_dir,
      options->output_dir,
      layer_count,
      width,
      height);
  fflush(stdout);

  for (size_t layer = 0; layer < layer_count; ++layer) {
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        const size_t index = voxel_index(layer, y, x, width, height);
        const uint8_t *pixel = images[layer].pixels + (size_t) y * images[layer].stride + x * 4u;
        removable_map[index] = color_is_removable(pixel, options) ? 1u : 0u;
      }
      progress_advance(&progress, width);
    }
  }

  if (options->dist_positive_x > 0u) {
    run_positive_x_pass(removable_map,
                        keep_map,
                        layer_count,
                        width,
                        height,
                        options->dist_positive_x,
                        options->treat_edges_as_blockers,
                        &progress);
  }
  if (options->dist_negative_x > 0u) {
    run_negative_x_pass(removable_map,
                        keep_map,
                        layer_count,
                        width,
                        height,
                        options->dist_negative_x,
                        options->treat_edges_as_blockers,
                        &progress);
  }
  if (options->dist_positive_y > 0u) {
    run_positive_y_pass(removable_map,
                        keep_map,
                        layer_count,
                        width,
                        height,
                        options->dist_positive_y,
                        options->treat_edges_as_blockers,
                        &progress);
  }
  if (options->dist_negative_y > 0u) {
    run_negative_y_pass(removable_map,
                        keep_map,
                        layer_count,
                        width,
                        height,
                        options->dist_negative_y,
                        options->treat_edges_as_blockers,
                        &progress);
  }
  if (options->dist_positive_z > 0u) {
    run_positive_z_pass(removable_map,
                        keep_map,
                        layer_count,
                        width,
                        height,
                        options->dist_positive_z,
                        options->treat_edges_as_blockers,
                        &progress);
  }
  if (options->dist_negative_z > 0u) {
    run_negative_z_pass(removable_map,
                        keep_map,
                        layer_count,
                        width,
                        height,
                        options->dist_negative_z,
                        options->treat_edges_as_blockers,
                        &progress);
  }
  if (diagonal_pass_enabled(options->dist_positive_x, options->dist_positive_y)) {
    run_xy_diagonal_pass(removable_map,
                         keep_map,
                         layer_count,
                         width,
                         height,
                         options->dist_positive_x,
                         options->dist_positive_y,
                         false,
                         true,
                         options->treat_edges_as_blockers,
                         &progress);
  }
  if (diagonal_pass_enabled(options->dist_negative_x, options->dist_negative_y)) {
    run_xy_diagonal_pass(removable_map,
                         keep_map,
                         layer_count,
                         width,
                         height,
                         options->dist_negative_x,
                         options->dist_negative_y,
                         false,
                         false,
                         options->treat_edges_as_blockers,
                         &progress);
  }
  if (diagonal_pass_enabled(options->dist_positive_x, options->dist_negative_y)) {
    run_xy_diagonal_pass(removable_map,
                         keep_map,
                         layer_count,
                         width,
                         height,
                         options->dist_positive_x,
                         options->dist_negative_y,
                         true,
                         true,
                         options->treat_edges_as_blockers,
                         &progress);
  }
  if (diagonal_pass_enabled(options->dist_negative_x, options->dist_positive_y)) {
    run_xy_diagonal_pass(removable_map,
                         keep_map,
                         layer_count,
                         width,
                         height,
                         options->dist_negative_x,
                         options->dist_positive_y,
                         true,
                         false,
                         options->treat_edges_as_blockers,
                         &progress);
  }
  if (diagonal_pass_enabled(options->dist_positive_x, options->dist_positive_z)) {
    run_xz_diagonal_pass(removable_map,
                         keep_map,
                         layer_count,
                         width,
                         height,
                         options->dist_positive_x,
                         options->dist_positive_z,
                         false,
                         true,
                         options->treat_edges_as_blockers,
                         &progress);
  }
  if (diagonal_pass_enabled(options->dist_negative_x, options->dist_negative_z)) {
    run_xz_diagonal_pass(removable_map,
                         keep_map,
                         layer_count,
                         width,
                         height,
                         options->dist_negative_x,
                         options->dist_negative_z,
                         false,
                         false,
                         options->treat_edges_as_blockers,
                         &progress);
  }
  if (diagonal_pass_enabled(options->dist_positive_x, options->dist_negative_z)) {
    run_xz_diagonal_pass(removable_map,
                         keep_map,
                         layer_count,
                         width,
                         height,
                         options->dist_positive_x,
                         options->dist_negative_z,
                         true,
                         true,
                         options->treat_edges_as_blockers,
                         &progress);
  }
  if (diagonal_pass_enabled(options->dist_negative_x, options->dist_positive_z)) {
    run_xz_diagonal_pass(removable_map,
                         keep_map,
                         layer_count,
                         width,
                         height,
                         options->dist_negative_x,
                         options->dist_positive_z,
                         true,
                         false,
                         options->treat_edges_as_blockers,
                         &progress);
  }
  if (diagonal_pass_enabled(options->dist_positive_y, options->dist_positive_z)) {
    run_yz_diagonal_pass(removable_map,
                         keep_map,
                         layer_count,
                         width,
                         height,
                         options->dist_positive_y,
                         options->dist_positive_z,
                         false,
                         true,
                         options->treat_edges_as_blockers,
                         &progress);
  }
  if (diagonal_pass_enabled(options->dist_negative_y, options->dist_negative_z)) {
    run_yz_diagonal_pass(removable_map,
                         keep_map,
                         layer_count,
                         width,
                         height,
                         options->dist_negative_y,
                         options->dist_negative_z,
                         false,
                         false,
                         options->treat_edges_as_blockers,
                         &progress);
  }
  if (diagonal_pass_enabled(options->dist_negative_y, options->dist_positive_z)) {
    run_yz_diagonal_pass(removable_map,
                         keep_map,
                         layer_count,
                         width,
                         height,
                         options->dist_negative_y,
                         options->dist_positive_z,
                         true,
                         true,
                         options->treat_edges_as_blockers,
                         &progress);
  }
  if (diagonal_pass_enabled(options->dist_positive_y, options->dist_negative_z)) {
    run_yz_diagonal_pass(removable_map,
                         keep_map,
                         layer_count,
                         width,
                         height,
                         options->dist_positive_y,
                         options->dist_negative_z,
                         true,
                         false,
                         options->treat_edges_as_blockers,
                         &progress);
  }

  for (size_t layer = 0; layer < layer_count; ++layer) {
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        const size_t index = voxel_index(layer, y, x, width, height);
        if (removable_map[index] != 0u && keep_map[index] == 0u) {
          uint8_t *pixel = images[layer].pixels + (size_t) y * images[layer].stride + x * 4u;
          set_pixel(pixel, &options->destination_color);
        }
      }
      progress_advance(&progress, width);
    }
  }

  for (size_t layer = 0; layer < layer_count; ++layer) {
    char *output_path = NULL;
    if (build_output_path(options->output_dir, layers[layer].name, &output_path) != 0) {
      free(keep_map);
      free(removable_map);
      for (size_t i = 0; i < layer_count; ++i) {
        free_image(&images[i]);
      }
      free(images);
      free_layer_files(layers, layer_count);
      return -1;
    }

    if (write_png(output_path, &images[layer]) != 0) {
      free(output_path);
      free(keep_map);
      free(removable_map);
      for (size_t i = 0; i < layer_count; ++i) {
        free_image(&images[i]);
      }
      free(images);
      free_layer_files(layers, layer_count);
      return -1;
    }

    free(output_path);
    progress_advance(&progress, (uint64_t) width * (uint64_t) height);
  }

  progress_finish(&progress);

  free(keep_map);
  free(removable_map);
  for (size_t i = 0; i < layer_count; ++i) {
    free_image(&images[i]);
  }
  free(images);
  free_layer_files(layers, layer_count);
  return 0;
}

static int mkdir_recursive(const char *path) {
  if (path == NULL || *path == '\0') {
    fprintf(stderr, "Output directory path is empty.\n");
    return -1;
  }

  char *mutable_path = strdup(path);
  if (mutable_path == NULL) {
    fprintf(stderr, "Failed to allocate output directory path.\n");
    return -1;
  }

  const size_t length = strlen(mutable_path);
  for (size_t i = 1; i <= length; ++i) {
    if (mutable_path[i] != '/' && mutable_path[i] != '\0') {
      continue;
    }

    const char saved = mutable_path[i];
    mutable_path[i] = '\0';
    if (mutable_path[0] != '\0' && mkdir(mutable_path, 0777) != 0 && errno != EEXIST) {
      fprintf(stderr, "Failed to create directory %s: %s\n", mutable_path, strerror(errno));
      free(mutable_path);
      return -1;
    }
    mutable_path[i] = saved;
  }

  free(mutable_path);
  return 0;
}

static int collect_layer_files(const char *directory, LayerFile **layers_out, size_t *count_out) {
  DIR *dir = opendir(directory);
  if (dir == NULL) {
    fprintf(stderr, "Failed to open %s: %s\n", directory, strerror(errno));
    return -1;
  }

  LayerFile *layers = NULL;
  size_t count = 0;
  size_t capacity = 0;
  struct dirent *entry = NULL;
  while ((entry = readdir(dir)) != NULL) {
    uint32_t layer_index = 0;
    if (!parse_layer_file_name(entry->d_name, &layer_index)) {
      continue;
    }

    if (count == capacity) {
      const size_t new_capacity = capacity == 0u ? 64u : capacity * 2u;
      LayerFile *resized = realloc(layers, new_capacity * sizeof(*layers));
      if (resized == NULL) {
        fprintf(stderr, "Failed to allocate layer list.\n");
        free_layer_files(layers, count);
        closedir(dir);
        return -1;
      }
      layers = resized;
      capacity = new_capacity;
    }

    const size_t path_len = strlen(directory) + strlen(entry->d_name) + 2u;
    layers[count].path = malloc(path_len);
    layers[count].name = strdup(entry->d_name);
    if (layers[count].path == NULL || layers[count].name == NULL) {
      fprintf(stderr, "Failed to allocate layer path.\n");
      free_layer_files(layers, count + 1u);
      closedir(dir);
      return -1;
    }

    snprintf(layers[count].path, path_len, "%s/%s", directory, entry->d_name);
    layers[count].layer_index = layer_index;
    ++count;
  }

  closedir(dir);
  qsort(layers, count, sizeof(*layers), compare_layer_files);
  *layers_out = layers;
  *count_out = count;
  return 0;
}

static void free_layer_files(LayerFile *layers, size_t count) {
  if (layers == NULL) {
    return;
  }

  for (size_t i = 0; i < count; ++i) {
    free(layers[i].name);
    free(layers[i].path);
  }
  free(layers);
}

static int compare_layer_files(const void *lhs, const void *rhs) {
  const LayerFile *a = lhs;
  const LayerFile *b = rhs;
  if (a->layer_index < b->layer_index) {
    return -1;
  }
  if (a->layer_index > b->layer_index) {
    return 1;
  }
  return strcmp(a->path, b->path);
}

static bool parse_layer_file_name(const char *name, uint32_t *index_out) {
  if (strncmp(name, "out_", 4) != 0) {
    return false;
  }

  errno = 0;
  char *end = NULL;
  const unsigned long parsed = strtoul(name + 4, &end, 10);
  if (errno != 0 || end == name + 4 || (*end != '\0' && strcmp(end, ".png") != 0)) {
    return false;
  }
  if (parsed > UINT32_MAX || strcmp(end, ".png") != 0) {
    return false;
  }

  *index_out = (uint32_t) parsed;
  return true;
}

static int build_output_path(const char *directory, const char *name, char **path_out) {
  const size_t length = strlen(directory) + strlen(name) + 2u;
  char *path = malloc(length);
  if (path == NULL) {
    fprintf(stderr, "Failed to allocate output path.\n");
    return -1;
  }
  snprintf(path, length, "%s/%s", directory, name);
  *path_out = path;
  return 0;
}

static int init_image(Image *image, uint32_t width, uint32_t height) {
  if (image == NULL || width == 0u || height == 0u) {
    fprintf(stderr, "Invalid image dimensions.\n");
    return -1;
  }

  image->width = width;
  image->height = height;
  image->stride = (size_t) width * 4u;
  image->pixels = calloc(image->stride, height);
  if (image->pixels == NULL) {
    fprintf(stderr, "Failed to allocate image memory.\n");
    memset(image, 0, sizeof(*image));
    return -1;
  }

  return 0;
}

static void free_image(Image *image) {
  if (image == NULL) {
    return;
  }

  free(image->pixels);
  memset(image, 0, sizeof(*image));
}

static bool colors_match(const uint8_t *pixel, const HollowColor *color) {
  return pixel[0] == color->r && pixel[1] == color->g && pixel[2] == color->b &&
         pixel[3] == color->a;
}

static bool color_is_removable(const uint8_t *pixel, const HollowOptions *options) {
  for (size_t i = 0; i < options->colors_for_removal_count; ++i) {
    if (colors_match(pixel, &options->colors_for_removal[i])) {
      return true;
    }
  }
  return false;
}

static void set_pixel(uint8_t *pixel, const HollowColor *color) {
  pixel[0] = color->r;
  pixel[1] = color->g;
  pixel[2] = color->b;
  pixel[3] = color->a;
}

static size_t voxel_index(
    size_t layer,
    uint32_t y,
    uint32_t x,
    uint32_t width,
    uint32_t height) {
  return layer * (size_t) width * (size_t) height + (size_t) y * (size_t) width + (size_t) x;
}

static void progress_begin(ProgressTracker *tracker, uint64_t interval, uint64_t total) {
  tracker->interval = interval;
  tracker->next_tick = interval;
  tracker->completed = 0u;
  tracker->total = total;
  clock_gettime(CLOCK_MONOTONIC, &tracker->start_time);
}

static void progress_advance(ProgressTracker *tracker, uint64_t amount) {
  if (tracker == NULL || amount == 0u) {
    return;
  }

  tracker->completed += amount;
  if (tracker->completed > tracker->total) {
    tracker->completed = tracker->total;
  }

  if (tracker->interval == 0u) {
    return;
  }

  while (tracker->completed >= tracker->next_tick && tracker->next_tick <= tracker->total) {
    log_progress(tracker);
    tracker->next_tick += tracker->interval;
  }
}

static void progress_finish(ProgressTracker *tracker) {
  if (tracker == NULL) {
    return;
  }

  if (tracker->completed == tracker->total && tracker->next_tick > tracker->total) {
    return;
  }

  tracker->completed = tracker->total;
  log_progress(tracker);
}

static double elapsed_seconds_since(const struct timespec *start_time) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  const double seconds = (double) (now.tv_sec - start_time->tv_sec);
  const double nanos = (double) (now.tv_nsec - start_time->tv_nsec) / 1000000000.0;
  return seconds + nanos;
}

static void format_duration(double seconds, char *buffer, size_t buffer_size) {
  if (seconds < 0.0) {
    seconds = 0.0;
  }

  uint64_t rounded = (uint64_t) (seconds + 0.5);
  const uint64_t hours = rounded / 3600u;
  const uint64_t minutes = (rounded % 3600u) / 60u;
  const uint64_t secs = rounded % 60u;
  snprintf(buffer, buffer_size, "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64, hours, minutes, secs);
}

static void log_progress(const ProgressTracker *tracker) {
  char elapsed_text[32];
  char remaining_text[32];
  const double elapsed = elapsed_seconds_since(&tracker->start_time);
  double remaining = 0.0;
  double percent = 100.0;

  if (tracker->total > 0u) {
    percent = 100.0 * (double) tracker->completed / (double) tracker->total;
  }
  if (tracker->completed > 0u && tracker->completed < tracker->total) {
    remaining = elapsed * (double) (tracker->total - tracker->completed) /
                (double) tracker->completed;
  }

  format_duration(elapsed, elapsed_text, sizeof(elapsed_text));
  format_duration(remaining, remaining_text, sizeof(remaining_text));
  printf(
      "progress: %.2f%% (%" PRIu64 "/%" PRIu64 "), elapsed %s, remaining %s\n",
      percent,
      tracker->completed,
      tracker->total,
      elapsed_text,
      remaining_text);
  fflush(stdout);
}

static void run_positive_x_pass(
    const uint8_t *removable_map,
    uint8_t *keep_map,
    size_t layer_count,
    uint32_t width,
    uint32_t height,
    uint32_t threshold,
    bool treat_edges_as_blockers,
    ProgressTracker *progress) {
  for (size_t layer = 0; layer < layer_count; ++layer) {
    for (uint32_t y = 0; y < height; ++y) {
      uint32_t next_blocker = treat_edges_as_blockers ? width : UINT32_MAX;
      for (uint32_t x = width; x-- > 0u;) {
        const size_t index = voxel_index(layer, y, x, width, height);
        if (removable_map[index] == 0u) {
          next_blocker = x;
          continue;
        }
        if (next_blocker != UINT32_MAX && next_blocker - x <= threshold) {
          keep_map[index] = 1u;
        }
      }
      progress_advance(progress, width);
    }
  }
}

static void run_negative_x_pass(
    const uint8_t *removable_map,
    uint8_t *keep_map,
    size_t layer_count,
    uint32_t width,
    uint32_t height,
    uint32_t threshold,
    bool treat_edges_as_blockers,
    ProgressTracker *progress) {
  for (size_t layer = 0; layer < layer_count; ++layer) {
    for (uint32_t y = 0; y < height; ++y) {
      int64_t previous_blocker = treat_edges_as_blockers ? -1 : INT64_MIN;
      for (uint32_t x = 0; x < width; ++x) {
        const size_t index = voxel_index(layer, y, x, width, height);
        if (removable_map[index] == 0u) {
          previous_blocker = (int64_t) x;
          continue;
        }
        if (previous_blocker != INT64_MIN &&
            (uint64_t) ((int64_t) x - previous_blocker) <= threshold) {
          keep_map[index] = 1u;
        }
      }
      progress_advance(progress, width);
    }
  }
}

static void run_positive_y_pass(
    const uint8_t *removable_map,
    uint8_t *keep_map,
    size_t layer_count,
    uint32_t width,
    uint32_t height,
    uint32_t threshold,
    bool treat_edges_as_blockers,
    ProgressTracker *progress) {
  for (size_t layer = 0; layer < layer_count; ++layer) {
    for (uint32_t x = 0; x < width; ++x) {
      uint32_t next_blocker = treat_edges_as_blockers ? height : UINT32_MAX;
      for (uint32_t y = height; y-- > 0u;) {
        const size_t index = voxel_index(layer, y, x, width, height);
        if (removable_map[index] == 0u) {
          next_blocker = y;
          continue;
        }
        if (next_blocker != UINT32_MAX && next_blocker - y <= threshold) {
          keep_map[index] = 1u;
        }
      }
      progress_advance(progress, height);
    }
  }
}

static void run_negative_y_pass(
    const uint8_t *removable_map,
    uint8_t *keep_map,
    size_t layer_count,
    uint32_t width,
    uint32_t height,
    uint32_t threshold,
    bool treat_edges_as_blockers,
    ProgressTracker *progress) {
  for (size_t layer = 0; layer < layer_count; ++layer) {
    for (uint32_t x = 0; x < width; ++x) {
      int64_t previous_blocker = treat_edges_as_blockers ? -1 : INT64_MIN;
      for (uint32_t y = 0; y < height; ++y) {
        const size_t index = voxel_index(layer, y, x, width, height);
        if (removable_map[index] == 0u) {
          previous_blocker = (int64_t) y;
          continue;
        }
        if (previous_blocker != INT64_MIN &&
            (uint64_t) ((int64_t) y - previous_blocker) <= threshold) {
          keep_map[index] = 1u;
        }
      }
      progress_advance(progress, height);
    }
  }
}

static void run_positive_z_pass(
    const uint8_t *removable_map,
    uint8_t *keep_map,
    size_t layer_count,
    uint32_t width,
    uint32_t height,
    uint32_t threshold,
    bool treat_edges_as_blockers,
    ProgressTracker *progress) {
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      size_t next_blocker = treat_edges_as_blockers ? layer_count : SIZE_MAX;
      for (size_t layer = layer_count; layer-- > 0u;) {
        const size_t index = voxel_index(layer, y, x, width, height);
        if (removable_map[index] == 0u) {
          next_blocker = layer;
          continue;
        }
        if (next_blocker != SIZE_MAX && next_blocker - layer <= threshold) {
          keep_map[index] = 1u;
        }
      }
      progress_advance(progress, (uint64_t) layer_count);
    }
  }
}

static void run_negative_z_pass(
    const uint8_t *removable_map,
    uint8_t *keep_map,
    size_t layer_count,
    uint32_t width,
    uint32_t height,
    uint32_t threshold,
    bool treat_edges_as_blockers,
    ProgressTracker *progress) {
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      int64_t previous_blocker = treat_edges_as_blockers ? -1 : INT64_MIN;
      for (size_t layer = 0; layer < layer_count; ++layer) {
        const size_t index = voxel_index(layer, y, x, width, height);
        if (removable_map[index] == 0u) {
          previous_blocker = (int64_t) layer;
          continue;
        }
        if (previous_blocker != INT64_MIN &&
            (uint64_t) ((int64_t) layer - previous_blocker) <= threshold) {
          keep_map[index] = 1u;
        }
      }
      progress_advance(progress, (uint64_t) layer_count);
    }
  }
}

static bool diagonal_pass_enabled(uint32_t distance_a, uint32_t distance_b) {
  return distance_a > 0u && distance_b > 0u;
}

static uint64_t diagonal_threshold_sq(uint32_t distance_a, uint32_t distance_b) {
  return (uint64_t) distance_a * (uint64_t) distance_a +
         (uint64_t) distance_b * (uint64_t) distance_b;
}

static bool diagonal_step_within_threshold(size_t step_count, uint64_t threshold_sq) {
  const uint64_t step = (uint64_t) step_count;
  return 2u * step * step <= threshold_sq;
}

static void process_diagonal_line(
    const uint8_t *removable_map,
    uint8_t *keep_map,
    const size_t *line_indices,
    size_t line_length,
    uint64_t threshold_sq,
    bool treat_edges_as_blockers) {
  if (line_length == 0u) {
    return;
  }

  int64_t previous_blocker = treat_edges_as_blockers ? -1 : INT64_MIN;
  for (size_t pos = 0; pos < line_length; ++pos) {
    const size_t index = line_indices[pos];
    if (removable_map[index] == 0u) {
      previous_blocker = (int64_t) pos;
      continue;
    }
    if (previous_blocker != INT64_MIN &&
        diagonal_step_within_threshold((size_t) ((int64_t) pos - previous_blocker), threshold_sq)) {
      keep_map[index] = 1u;
    }
  }
}

static void run_xy_diagonal_pass(
    const uint8_t *removable_map,
    uint8_t *keep_map,
    size_t layer_count,
    uint32_t width,
    uint32_t height,
    uint32_t distance_x,
    uint32_t distance_y,
    bool invert_y,
    bool reverse_line_direction,
    bool treat_edges_as_blockers,
    ProgressTracker *progress) {
  const size_t max_line_length = width > height ? width : height;
  size_t *line_indices = malloc(max_line_length * sizeof(*line_indices));
  if (line_indices == NULL) {
    fprintf(stderr, "Failed to allocate XY diagonal line buffer.\n");
    return;
  }

  const uint64_t threshold_sq = diagonal_threshold_sq(distance_x, distance_y);
  for (size_t layer = 0; layer < layer_count; ++layer) {
    for (uint32_t start_x = 0; start_x < width; ++start_x) {
      size_t line_length = 0u;
      for (uint32_t x = start_x, y = invert_y ? (height - 1u) : 0u; x < width;) {
        line_indices[line_length++] = voxel_index(layer, y, x, width, height);
        ++x;
        if (invert_y) {
          if (y == 0u) {
            break;
          }
          --y;
        } else {
          ++y;
          if (y >= height) {
            break;
          }
        }
      }
      if (reverse_line_direction) {
        for (size_t i = 0; i < line_length / 2u; ++i) {
          const size_t tmp = line_indices[i];
          line_indices[i] = line_indices[line_length - 1u - i];
          line_indices[line_length - 1u - i] = tmp;
        }
      }
      process_diagonal_line(
          removable_map, keep_map, line_indices, line_length, threshold_sq, treat_edges_as_blockers);
      progress_advance(progress, line_length);
    }

    for (uint32_t start_y = 1; start_y < height; ++start_y) {
      size_t line_length = 0u;
      const uint32_t initial_y = invert_y ? (height - 1u - start_y) : start_y;
      for (uint32_t x = 0u, y = initial_y; x < width;) {
        line_indices[line_length++] = voxel_index(layer, y, x, width, height);
        ++x;
        if (invert_y) {
          if (y == 0u) {
            break;
          }
          --y;
        } else {
          ++y;
          if (y >= height) {
            break;
          }
        }
      }
      if (reverse_line_direction) {
        for (size_t i = 0; i < line_length / 2u; ++i) {
          const size_t tmp = line_indices[i];
          line_indices[i] = line_indices[line_length - 1u - i];
          line_indices[line_length - 1u - i] = tmp;
        }
      }
      process_diagonal_line(
          removable_map, keep_map, line_indices, line_length, threshold_sq, treat_edges_as_blockers);
      progress_advance(progress, line_length);
    }
  }

  free(line_indices);
}

static void run_xz_diagonal_pass(
    const uint8_t *removable_map,
    uint8_t *keep_map,
    size_t layer_count,
    uint32_t width,
    uint32_t height,
    uint32_t distance_x,
    uint32_t distance_z,
    bool invert_z,
    bool reverse_line_direction,
    bool treat_edges_as_blockers,
    ProgressTracker *progress) {
  const size_t max_line_length = width > layer_count ? width : layer_count;
  size_t *line_indices = malloc(max_line_length * sizeof(*line_indices));
  if (line_indices == NULL) {
    fprintf(stderr, "Failed to allocate XZ diagonal line buffer.\n");
    return;
  }

  const uint64_t threshold_sq = diagonal_threshold_sq(distance_x, distance_z);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t start_x = 0; start_x < width; ++start_x) {
      size_t line_length = 0u;
      size_t layer = invert_z ? (layer_count - 1u) : 0u;
      for (size_t x = start_x; x < width;) {
        line_indices[line_length++] = voxel_index(layer, y, (uint32_t) x, width, height);
        ++x;
        if (invert_z) {
          if (layer == 0u) {
            break;
          }
          --layer;
        } else {
          ++layer;
          if (layer >= layer_count) {
            break;
          }
        }
      }
      if (reverse_line_direction) {
        for (size_t i = 0; i < line_length / 2u; ++i) {
          const size_t tmp = line_indices[i];
          line_indices[i] = line_indices[line_length - 1u - i];
          line_indices[line_length - 1u - i] = tmp;
        }
      }
      process_diagonal_line(
          removable_map, keep_map, line_indices, line_length, threshold_sq, treat_edges_as_blockers);
      progress_advance(progress, line_length);
    }

    for (size_t start_layer = 1u; start_layer < layer_count; ++start_layer) {
      size_t line_length = 0u;
      size_t layer = invert_z ? (layer_count - 1u - start_layer) : start_layer;
      for (size_t x = 0u; x < width;) {
        line_indices[line_length++] = voxel_index(layer, y, (uint32_t) x, width, height);
        ++x;
        if (invert_z) {
          if (layer == 0u) {
            break;
          }
          --layer;
        } else {
          ++layer;
          if (layer >= layer_count) {
            break;
          }
        }
      }
      if (reverse_line_direction) {
        for (size_t i = 0; i < line_length / 2u; ++i) {
          const size_t tmp = line_indices[i];
          line_indices[i] = line_indices[line_length - 1u - i];
          line_indices[line_length - 1u - i] = tmp;
        }
      }
      process_diagonal_line(
          removable_map, keep_map, line_indices, line_length, threshold_sq, treat_edges_as_blockers);
      progress_advance(progress, line_length);
    }
  }

  free(line_indices);
}

static void run_yz_diagonal_pass(
    const uint8_t *removable_map,
    uint8_t *keep_map,
    size_t layer_count,
    uint32_t width,
    uint32_t height,
    uint32_t distance_y,
    uint32_t distance_z,
    bool invert_y,
    bool reverse_line_direction,
    bool treat_edges_as_blockers,
    ProgressTracker *progress) {
  const size_t max_line_length = height > layer_count ? height : layer_count;
  size_t *line_indices = malloc(max_line_length * sizeof(*line_indices));
  if (line_indices == NULL) {
    fprintf(stderr, "Failed to allocate YZ diagonal line buffer.\n");
    return;
  }

  const uint64_t threshold_sq = diagonal_threshold_sq(distance_y, distance_z);
  for (uint32_t x = 0; x < width; ++x) {
    for (uint32_t start_y = 0; start_y < height; ++start_y) {
      size_t line_length = 0u;
      uint32_t y = invert_y ? (height - 1u - start_y) : start_y;
      for (size_t layer = 0u; layer < layer_count; ++layer) {
        line_indices[line_length++] = voxel_index(layer, y, x, width, height);
        if (invert_y) {
          if (y == 0u) {
            break;
          }
          --y;
        } else {
          ++y;
          if (y >= height) {
            break;
          }
        }
      }
      if (reverse_line_direction) {
        for (size_t i = 0; i < line_length / 2u; ++i) {
          const size_t tmp = line_indices[i];
          line_indices[i] = line_indices[line_length - 1u - i];
          line_indices[line_length - 1u - i] = tmp;
        }
      }
      process_diagonal_line(
          removable_map, keep_map, line_indices, line_length, threshold_sq, treat_edges_as_blockers);
      progress_advance(progress, line_length);
    }

    for (size_t start_layer = 1u; start_layer < layer_count; ++start_layer) {
      size_t line_length = 0u;
      uint32_t y = invert_y ? (height - 1u) : 0u;
      for (size_t layer = start_layer; layer < layer_count; ++layer) {
        line_indices[line_length++] = voxel_index(layer, y, x, width, height);
        if (invert_y) {
          if (y == 0u) {
            break;
          }
          --y;
        } else {
          ++y;
          if (y >= height) {
            break;
          }
        }
      }
      if (reverse_line_direction) {
        for (size_t i = 0; i < line_length / 2u; ++i) {
          const size_t tmp = line_indices[i];
          line_indices[i] = line_indices[line_length - 1u - i];
          line_indices[line_length - 1u - i] = tmp;
        }
      }
      process_diagonal_line(
          removable_map, keep_map, line_indices, line_length, threshold_sq, treat_edges_as_blockers);
      progress_advance(progress, line_length);
    }
  }

  free(line_indices);
}

static uint32_t read_u32_be(const uint8_t *bytes) {
  return ((uint32_t) bytes[0] << 24) |
         ((uint32_t) bytes[1] << 16) |
         ((uint32_t) bytes[2] << 8) |
         (uint32_t) bytes[3];
}

static void write_u32_be(uint8_t *out, uint32_t value) {
  out[0] = (uint8_t) ((value >> 24) & 0xffu);
  out[1] = (uint8_t) ((value >> 16) & 0xffu);
  out[2] = (uint8_t) ((value >> 8) & 0xffu);
  out[3] = (uint8_t) (value & 0xffu);
}

static uint32_t crc32_bytes(const uint8_t *bytes, size_t length) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = (uint32_t) -((int32_t) (crc & 1u));
      crc = (crc >> 1) ^ (0xedb88320u & mask);
    }
  }
  return crc ^ 0xffffffffu;
}

static uint32_t adler32_bytes(const uint8_t *bytes, size_t length) {
  uint32_t a = 1u;
  uint32_t b = 0u;
  const uint32_t mod = 65521u;
  for (size_t i = 0; i < length; ++i) {
    a = (a + bytes[i]) % mod;
    b = (b + a) % mod;
  }
  return (b << 16) | a;
}

static int append_chunk(
    FILE *file,
    const char type[4],
    const uint8_t *data,
    uint32_t length) {
  uint8_t length_be[4];
  uint8_t crc_be[4];
  write_u32_be(length_be, length);

  const size_t crc_buffer_size = 4u + (size_t) length;
  uint8_t *crc_buffer = malloc(crc_buffer_size);
  if (crc_buffer == NULL) {
    fprintf(stderr, "Failed to allocate PNG chunk memory.\n");
    return -1;
  }

  memcpy(crc_buffer, type, 4);
  if (length > 0u && data != NULL) {
    memcpy(crc_buffer + 4, data, length);
  }
  write_u32_be(crc_be, crc32_bytes(crc_buffer, crc_buffer_size));

  const int failed =
      fwrite(length_be, 1, sizeof(length_be), file) != sizeof(length_be) ||
      fwrite(type, 1, 4u, file) != 4u ||
      (length > 0u && fwrite(data, 1, length, file) != length) ||
      fwrite(crc_be, 1, sizeof(crc_be), file) != sizeof(crc_be);
  free(crc_buffer);
  if (failed) {
    fprintf(stderr, "Failed to write a PNG chunk.\n");
    return -1;
  }

  return 0;
}

static int write_png(const char *path, const Image *image) {
  const size_t raw_size = (size_t) image->height * (1u + image->stride);
  uint8_t *raw = malloc(raw_size);
  if (raw == NULL) {
    fprintf(stderr, "Failed to allocate PNG scanlines.\n");
    return -1;
  }

  size_t src = 0u;
  size_t dst = 0u;
  for (uint32_t y = 0; y < image->height; ++y) {
    raw[dst++] = 0u;
    memcpy(raw + dst, image->pixels + src, image->stride);
    dst += image->stride;
    src += image->stride;
  }

  const size_t block_count = (raw_size + 65534u) / 65535u;
  const size_t zlib_size = 2u + block_count * 5u + raw_size + 4u;
  uint8_t *zlib_stream = malloc(zlib_size);
  if (zlib_stream == NULL) {
    fprintf(stderr, "Failed to allocate PNG zlib stream.\n");
    free(raw);
    return -1;
  }

  size_t zoff = 0u;
  zlib_stream[zoff++] = 0x78u;
  zlib_stream[zoff++] = 0x01u;

  size_t raw_offset = 0u;
  while (raw_offset < raw_size) {
    const size_t remaining = raw_size - raw_offset;
    const uint16_t block_size = (uint16_t) (remaining > 65535u ? 65535u : remaining);
    const uint8_t final_block = (raw_offset + block_size >= raw_size) ? 1u : 0u;
    zlib_stream[zoff++] = final_block;
    zlib_stream[zoff++] = (uint8_t) (block_size & 0xffu);
    zlib_stream[zoff++] = (uint8_t) ((block_size >> 8) & 0xffu);
    {
      const uint16_t nlen = (uint16_t) ~block_size;
      zlib_stream[zoff++] = (uint8_t) (nlen & 0xffu);
      zlib_stream[zoff++] = (uint8_t) ((nlen >> 8) & 0xffu);
    }
    memcpy(zlib_stream + zoff, raw + raw_offset, block_size);
    zoff += block_size;
    raw_offset += block_size;
  }

  write_u32_be(zlib_stream + zoff, adler32_bytes(raw, raw_size));
  zoff += 4u;

  FILE *file = fopen(path, "wb");
  if (file == NULL) {
    fprintf(stderr, "Failed to open %s for writing: %s\n", path, strerror(errno));
    free(zlib_stream);
    free(raw);
    return -1;
  }

  static const uint8_t png_signature[8] = {
      0x89u, 0x50u, 0x4eu, 0x47u, 0x0du, 0x0au, 0x1au, 0x0au};
  if (fwrite(png_signature, 1, sizeof(png_signature), file) != sizeof(png_signature)) {
    fprintf(stderr, "Failed to write the PNG signature.\n");
    fclose(file);
    free(zlib_stream);
    free(raw);
    return -1;
  }

  {
    uint8_t ihdr[13];
    write_u32_be(ihdr, image->width);
    write_u32_be(ihdr + 4, image->height);
    ihdr[8] = 8u;
    ihdr[9] = 6u;
    ihdr[10] = 0u;
    ihdr[11] = 0u;
    ihdr[12] = 0u;

    if (append_chunk(file, "IHDR", ihdr, sizeof(ihdr)) != 0 ||
        append_chunk(file, "IDAT", zlib_stream, (uint32_t) zoff) != 0 ||
        append_chunk(file, "IEND", NULL, 0u) != 0) {
      fclose(file);
      free(zlib_stream);
      free(raw);
      return -1;
    }
  }

  if (fclose(file) != 0) {
    fprintf(stderr, "Failed to close %s.\n", path);
    free(zlib_stream);
    free(raw);
    return -1;
  }

  free(zlib_stream);
  free(raw);
  return 0;
}

static int read_png(const char *path, Image *image_out) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
    return -1;
  }

  if (fseek(file, 0, SEEK_END) != 0) {
    fprintf(stderr, "Failed to seek %s.\n", path);
    fclose(file);
    return -1;
  }

  const long file_size_long = ftell(file);
  if (file_size_long < 0) {
    fprintf(stderr, "Failed to measure %s.\n", path);
    fclose(file);
    return -1;
  }
  const size_t file_size = (size_t) file_size_long;
  if (fseek(file, 0, SEEK_SET) != 0) {
    fprintf(stderr, "Failed to rewind %s.\n", path);
    fclose(file);
    return -1;
  }

  uint8_t *bytes = malloc(file_size);
  if (bytes == NULL) {
    fprintf(stderr, "Failed to allocate PNG input buffer.\n");
    fclose(file);
    return -1;
  }
  if (fread(bytes, 1, file_size, file) != file_size) {
    fprintf(stderr, "Failed to read %s.\n", path);
    free(bytes);
    fclose(file);
    return -1;
  }
  fclose(file);

  static const uint8_t signature[8] = {
      0x89u, 0x50u, 0x4eu, 0x47u, 0x0du, 0x0au, 0x1au, 0x0au};
  if (file_size < sizeof(signature) || memcmp(bytes, signature, sizeof(signature)) != 0) {
    fprintf(stderr, "Unsupported PNG signature: %s\n", path);
    free(bytes);
    return -1;
  }

  size_t offset = 8u;
  bool saw_ihdr = false;
  bool saw_iend = false;
  uint32_t width = 0u;
  uint32_t height = 0u;
  uint8_t *idat = NULL;
  size_t idat_size = 0u;

  while (offset + 12u <= file_size) {
    const uint32_t chunk_length = read_u32_be(bytes + offset);
    offset += 4u;
    const uint8_t *chunk_type = bytes + offset;
    offset += 4u;
    if (offset + (size_t) chunk_length + 4u > file_size) {
      fprintf(stderr, "Truncated PNG chunk in %s\n", path);
      free(idat);
      free(bytes);
      return -1;
    }

    const uint8_t *chunk_data = bytes + offset;
    offset += chunk_length;
    offset += 4u;

    if (memcmp(chunk_type, "IHDR", 4u) == 0) {
      if (chunk_length != 13u) {
        fprintf(stderr, "Invalid IHDR chunk in %s\n", path);
        free(idat);
        free(bytes);
        return -1;
      }
      width = read_u32_be(chunk_data);
      height = read_u32_be(chunk_data + 4u);
      if (chunk_data[8] != 8u || chunk_data[9] != 6u ||
          chunk_data[10] != 0u || chunk_data[11] != 0u || chunk_data[12] != 0u) {
        fprintf(stderr, "Unsupported PNG format in %s\n", path);
        free(idat);
        free(bytes);
        return -1;
      }
      saw_ihdr = true;
    } else if (memcmp(chunk_type, "IDAT", 4u) == 0) {
      uint8_t *resized = realloc(idat, idat_size + (size_t) chunk_length);
      if (resized == NULL) {
        fprintf(stderr, "Failed to grow PNG IDAT buffer.\n");
        free(idat);
        free(bytes);
        return -1;
      }
      idat = resized;
      memcpy(idat + idat_size, chunk_data, chunk_length);
      idat_size += chunk_length;
    } else if (memcmp(chunk_type, "IEND", 4u) == 0) {
      saw_iend = true;
      break;
    }
  }

  if (!saw_ihdr || !saw_iend || width == 0u || height == 0u || idat_size < 6u) {
    fprintf(stderr, "Incomplete PNG data in %s\n", path);
    free(idat);
    free(bytes);
    return -1;
  }

  Image image;
  memset(&image, 0, sizeof(image));
  if (init_image(&image, width, height) != 0) {
    free(idat);
    free(bytes);
    return -1;
  }

  const size_t raw_size_hint = (size_t) height * (1u + (size_t) width * 4u);
  uint8_t *raw = malloc(raw_size_hint);
  if (raw == NULL) {
    fprintf(stderr, "Failed to allocate PNG raw scanlines.\n");
    free_image(&image);
    free(idat);
    free(bytes);
    return -1;
  }

  if (inflate_stored_blocks(idat, idat_size, raw, raw_size_hint) != 0) {
    fprintf(stderr, "Unsupported PNG compression in %s\n", path);
    free(raw);
    free_image(&image);
    free(idat);
    free(bytes);
    return -1;
  }

  for (uint32_t y = 0; y < height; ++y) {
    const size_t row_offset = (size_t) y * (1u + image.stride);
    if (raw[row_offset] != 0u) {
      fprintf(stderr, "Unsupported PNG filter in %s\n", path);
      free(raw);
      free_image(&image);
      free(idat);
      free(bytes);
      return -1;
    }
    memcpy(image.pixels + (size_t) y * image.stride, raw + row_offset + 1u, image.stride);
  }

  free(raw);
  free(idat);
  free(bytes);
  *image_out = image;
  return 0;
}

static int inflate_stored_blocks(
    const uint8_t *compressed,
    size_t compressed_size,
    uint8_t *raw_out,
    size_t raw_size) {
  size_t offset = 2u;
  size_t raw_offset = 0u;
  bool final_block = false;
  if (compressed_size < 6u) {
    return -1;
  }

  while (!final_block) {
    if (offset + 5u > compressed_size - 4u) {
      return -1;
    }

    const uint8_t header = compressed[offset++];
    final_block = (header & 0x01u) != 0u;
    if ((header & 0x06u) != 0u) {
      return -1;
    }

    const uint16_t len =
        (uint16_t) compressed[offset] | (uint16_t) ((uint16_t) compressed[offset + 1u] << 8);
    offset += 2u;
    const uint16_t nlen =
        (uint16_t) compressed[offset] | (uint16_t) ((uint16_t) compressed[offset + 1u] << 8);
    offset += 2u;
    if ((uint16_t) ~len != nlen) {
      return -1;
    }
    if (offset + len > compressed_size - 4u || raw_offset + len > raw_size) {
      return -1;
    }

    memcpy(raw_out + raw_offset, compressed + offset, len);
    raw_offset += len;
    offset += len;
  }

  if (raw_offset != raw_size) {
    return -1;
  }

  return adler32_bytes(raw_out, raw_size) == read_u32_be(compressed + compressed_size - 4u)
             ? 0
             : -1;
}
