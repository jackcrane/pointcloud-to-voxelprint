#include "fillRegion_pipeline.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
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
  double x;
  double y;
} Point2D;

typedef struct {
  double z;
  Point2D *points;
  size_t point_count;
} PolygonKeyframe;

static int mkdir_recursive(const char *path);
static int collect_layer_files(const char *directory, LayerFile **layers_out, size_t *count_out);
static void free_layer_files(LayerFile *layers, size_t count);
static int compare_layer_files(const void *lhs, const void *rhs);
static bool parse_layer_file_name(const char *name, uint32_t *index_out);
static int init_image(Image *image, uint32_t width, uint32_t height);
static void free_image(Image *image);
static void set_pixel(Image *image, int32_t x, int32_t y, const FillRegionColor *color);
static int read_png(const char *path, Image *image_out);
static int write_png(const char *path, const Image *image);
static int copy_file(const char *src_path, const char *dst_path);
static int build_output_path(const char *directory, const char *name, char **path_out);
static int build_xy_polygon(const FillRegionOptions *options, Point2D **points_out, size_t *count_out);
static int build_xyz_keyframes(
    const FillRegionOptions *options,
    PolygonKeyframe **keyframes_out,
    size_t *count_out);
static void free_keyframes(PolygonKeyframe *keyframes, size_t count);
static void free_layer_polygon(
    FillRegionMode mode,
    Point2D *layer_polygon,
    Point2D *shared_xy_polygon);
static int polygon_for_layer(
    const FillRegionOptions *options,
    const PolygonKeyframe *keyframes,
    size_t keyframe_count,
    uint32_t layer_index,
    Point2D **points_out,
    size_t *count_out,
    bool *affected_out);
static bool layer_is_in_fill_range(const FillRegionOptions *options, uint32_t layer_index);
static void fill_polygon(Image *image, const Point2D *points, size_t count, const FillRegionColor *color);
static int compare_doubles(const void *lhs, const void *rhs);
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
static double now_seconds(void);
static void format_duration(double seconds, char *buffer, size_t buffer_size);

int run_fill_region(const FillRegionOptions *options) {
  LayerFile *layers = NULL;
  size_t layer_count = 0;
  Point2D *xy_polygon = NULL;
  size_t xy_count = 0;
  PolygonKeyframe *keyframes = NULL;
  size_t keyframe_count = 0;
  const double start_time = now_seconds();

  if (options == NULL) {
    fprintf(stderr, "Missing fillRegion options.\n");
    return -1;
  }

  if (collect_layer_files(options->input_dir, &layers, &layer_count) != 0) {
    return -1;
  }
  if (layer_count == 0) {
    fprintf(stderr, "No layer PNGs were found in %s\n", options->input_dir);
    free_layer_files(layers, layer_count);
    return -1;
  }
  if (mkdir_recursive(options->output_dir) != 0) {
    free_layer_files(layers, layer_count);
    return -1;
  }

  if (options->mode == FILL_REGION_MODE_XY) {
    if (build_xy_polygon(options, &xy_polygon, &xy_count) != 0) {
      free_layer_files(layers, layer_count);
      return -1;
    }
  } else if (build_xyz_keyframes(options, &keyframes, &keyframe_count) != 0) {
    free_layer_files(layers, layer_count);
    return -1;
  }

  printf(
      "fillRegion: %s -> %s (%zu layers, mode=%s)\n",
      options->input_dir,
      options->output_dir,
      layer_count,
      options->mode == FILL_REGION_MODE_XY ? "xy" : "xyz");

  for (size_t i = 0; i < layer_count; ++i) {
    char *output_path = NULL;
    Point2D *layer_polygon = NULL;
    size_t layer_point_count = 0;
    bool affected = false;
    bool wrote_copy = false;

    if (build_output_path(options->output_dir, layers[i].name, &output_path) != 0) {
      free(xy_polygon);
      free_keyframes(keyframes, keyframe_count);
      free_layer_files(layers, layer_count);
      return -1;
    }

    if (!layer_is_in_fill_range(options, layers[i].layer_index)) {
      affected = false;
    } else if (options->mode == FILL_REGION_MODE_XY) {
      layer_polygon = xy_polygon;
      layer_point_count = xy_count;
      affected = true;
    } else if (polygon_for_layer(
                   options,
                   keyframes,
                   keyframe_count,
                   layers[i].layer_index,
                   &layer_polygon,
                   &layer_point_count,
                   &affected) != 0) {
      free(output_path);
      free(xy_polygon);
      free_keyframes(keyframes, keyframe_count);
      free_layer_files(layers, layer_count);
      return -1;
    }

    if (!affected) {
      if (copy_file(layers[i].path, output_path) != 0) {
        free_layer_polygon(options->mode, layer_polygon, xy_polygon);
        free(output_path);
        free(xy_polygon);
        free_keyframes(keyframes, keyframe_count);
        free_layer_files(layers, layer_count);
        return -1;
      }
      wrote_copy = true;
      free_layer_polygon(options->mode, layer_polygon, xy_polygon);
    } else {
      Image image;
      memset(&image, 0, sizeof(image));
      if (read_png(layers[i].path, &image) != 0) {
        free_layer_polygon(options->mode, layer_polygon, xy_polygon);
        free(output_path);
        free(xy_polygon);
        free_keyframes(keyframes, keyframe_count);
        free_layer_files(layers, layer_count);
        return -1;
      }

      fill_polygon(&image, layer_polygon, layer_point_count, &options->color);
      if (write_png(output_path, &image) != 0) {
        free_image(&image);
        free_layer_polygon(options->mode, layer_polygon, xy_polygon);
        free(output_path);
        free(xy_polygon);
        free_keyframes(keyframes, keyframe_count);
        free_layer_files(layers, layer_count);
        return -1;
      }

      free_image(&image);
    }
    free_layer_polygon(options->mode, layer_polygon, xy_polygon);
    free(output_path);

    {
      const size_t completed = i + 1u;
      const double elapsed = now_seconds() - start_time;
      const double average_per_layer = completed > 0u ? elapsed / (double) completed : 0.0;
      const double remaining = average_per_layer * (double) (layer_count - completed);
      char elapsed_text[32];
      char remaining_text[32];
      format_duration(elapsed, elapsed_text, sizeof(elapsed_text));
      format_duration(remaining, remaining_text, sizeof(remaining_text));
      printf(
          "Layer %zu/%zu (%s, %s)  elapsed=%s  eta=%s\n",
          completed,
          layer_count,
          layers[i].name,
          wrote_copy ? "copied" : "filled",
          elapsed_text,
          remaining_text);
      fflush(stdout);
    }
  }

  free(xy_polygon);
  free_keyframes(keyframes, keyframe_count);
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
      const size_t new_capacity = capacity == 0 ? 64u : capacity * 2u;
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
  unsigned long parsed = strtoul(name + 4, &end, 10);
  if (errno != 0 || end == name + 4 || (*end != '\0' && strcmp(end, ".png") != 0)) {
    return false;
  }
  if (parsed > UINT32_MAX || strcmp(end, ".png") != 0) {
    return false;
  }

  *index_out = (uint32_t) parsed;
  return true;
}

static int init_image(Image *image, uint32_t width, uint32_t height) {
  if (image == NULL || width == 0 || height == 0) {
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

static void set_pixel(Image *image, int32_t x, int32_t y, const FillRegionColor *color) {
  if (image == NULL || image->pixels == NULL || color == NULL ||
      x < 0 || y < 0 || (uint32_t) x >= image->width || (uint32_t) y >= image->height) {
    return;
  }

  const size_t offset = (size_t) y * image->stride + (size_t) x * 4u;
  image->pixels[offset] = color->r;
  image->pixels[offset + 1] = color->g;
  image->pixels[offset + 2] = color->b;
  image->pixels[offset + 3] = color->a;
}

static int copy_file(const char *src_path, const char *dst_path) {
  FILE *src = fopen(src_path, "rb");
  if (src == NULL) {
    fprintf(stderr, "Failed to open %s: %s\n", src_path, strerror(errno));
    return -1;
  }

  FILE *dst = fopen(dst_path, "wb");
  if (dst == NULL) {
    fprintf(stderr, "Failed to open %s for writing: %s\n", dst_path, strerror(errno));
    fclose(src);
    return -1;
  }

  uint8_t buffer[65536];
  while (true) {
    const size_t read_count = fread(buffer, 1, sizeof(buffer), src);
    if (read_count > 0 && fwrite(buffer, 1, read_count, dst) != read_count) {
      fprintf(stderr, "Failed to write %s.\n", dst_path);
      fclose(dst);
      fclose(src);
      return -1;
    }
    if (read_count < sizeof(buffer)) {
      if (ferror(src)) {
        fprintf(stderr, "Failed to read %s.\n", src_path);
        fclose(dst);
        fclose(src);
        return -1;
      }
      break;
    }
  }

  if (fclose(dst) != 0) {
    fprintf(stderr, "Failed to close %s.\n", dst_path);
    fclose(src);
    return -1;
  }
  if (fclose(src) != 0) {
    fprintf(stderr, "Failed to close %s.\n", src_path);
    return -1;
  }
  return 0;
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

static int build_xy_polygon(const FillRegionOptions *options, Point2D **points_out, size_t *count_out) {
  Point2D *polygon = calloc(options->point_count, sizeof(*polygon));
  if (polygon == NULL) {
    fprintf(stderr, "Failed to allocate XY polygon.\n");
    return -1;
  }

  for (size_t i = 0; i < options->point_count; ++i) {
    polygon[i].x = options->points[i].x;
    polygon[i].y = options->points[i].y;
  }

  *points_out = polygon;
  *count_out = options->point_count;
  return 0;
}

static int compare_points_by_z(const void *lhs, const void *rhs) {
  const FillRegionPoint *a = lhs;
  const FillRegionPoint *b = rhs;
  if (a->z < b->z) {
    return -1;
  }
  if (a->z > b->z) {
    return 1;
  }
  return 0;
}

static int build_xyz_keyframes(
    const FillRegionOptions *options,
    PolygonKeyframe **keyframes_out,
    size_t *count_out) {
  FillRegionPoint *sorted = calloc(options->point_count, sizeof(*sorted));
  PolygonKeyframe *keyframes = NULL;
  size_t keyframe_count = 0;
  size_t expected_count = 0;

  if (sorted == NULL) {
    fprintf(stderr, "Failed to allocate XYZ point buffer.\n");
    return -1;
  }
  memcpy(sorted, options->points, options->point_count * sizeof(*sorted));
  qsort(sorted, options->point_count, sizeof(*sorted), compare_points_by_z);

  for (size_t i = 0; i < options->point_count;) {
    size_t start = i;
    while (i < options->point_count && fabs(sorted[i].z - sorted[start].z) <= 1e-9) {
      ++i;
    }

    const size_t point_count = i - start;
    if (point_count < 3u) {
      fprintf(stderr, "Each z keyframe needs at least three points.\n");
      free(sorted);
      free_keyframes(keyframes, keyframe_count);
      return -1;
    }
    if (expected_count == 0u) {
      expected_count = point_count;
    } else if (expected_count != point_count) {
      fprintf(
          stderr,
          "All z keyframes must have the same number of points to interpolate cleanly.\n");
      free(sorted);
      free_keyframes(keyframes, keyframe_count);
      return -1;
    }

    PolygonKeyframe *resized = realloc(keyframes, (keyframe_count + 1u) * sizeof(*keyframes));
    if (resized == NULL) {
      fprintf(stderr, "Failed to allocate keyframes.\n");
      free(sorted);
      free_keyframes(keyframes, keyframe_count);
      return -1;
    }
    keyframes = resized;

    keyframes[keyframe_count].z = sorted[start].z;
    keyframes[keyframe_count].point_count = point_count;
    keyframes[keyframe_count].points = calloc(point_count, sizeof(*keyframes[keyframe_count].points));
    if (keyframes[keyframe_count].points == NULL) {
      fprintf(stderr, "Failed to allocate keyframe points.\n");
      free(sorted);
      free_keyframes(keyframes, keyframe_count + 1u);
      return -1;
    }

    for (size_t j = 0; j < point_count; ++j) {
      keyframes[keyframe_count].points[j].x = sorted[start + j].x;
      keyframes[keyframe_count].points[j].y = sorted[start + j].y;
    }
    ++keyframe_count;
  }

  free(sorted);
  *keyframes_out = keyframes;
  *count_out = keyframe_count;
  return 0;
}

static void free_keyframes(PolygonKeyframe *keyframes, size_t count) {
  if (keyframes == NULL) {
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    free(keyframes[i].points);
  }
  free(keyframes);
}

static void free_layer_polygon(
    FillRegionMode mode,
    Point2D *layer_polygon,
    Point2D *shared_xy_polygon) {
  if (layer_polygon == NULL) {
    return;
  }
  if (mode == FILL_REGION_MODE_XY && layer_polygon == shared_xy_polygon) {
    return;
  }
  free(layer_polygon);
}

static bool layer_is_in_fill_range(const FillRegionOptions *options, uint32_t layer_index) {
  if (options == NULL) {
    return false;
  }
  if (options->has_layer_first && layer_index < options->layer_first) {
    return false;
  }
  if (options->has_layer_last && layer_index > options->layer_last) {
    return false;
  }
  return true;
}

static int polygon_for_layer(
    const FillRegionOptions *options,
    const PolygonKeyframe *keyframes,
    size_t keyframe_count,
    uint32_t layer_index,
    Point2D **points_out,
    size_t *count_out,
    bool *affected_out) {
  (void) options;
  *points_out = NULL;
  *count_out = 0;
  *affected_out = false;

  if (keyframe_count == 0u) {
    return 0;
  }

  const double z = (double) layer_index;
  if (z < keyframes[0].z || z > keyframes[keyframe_count - 1u].z) {
    return 0;
  }

  for (size_t i = 0; i < keyframe_count; ++i) {
    if (fabs(z - keyframes[i].z) <= 1e-9) {
      Point2D *polygon = calloc(keyframes[i].point_count, sizeof(*polygon));
      if (polygon == NULL) {
        fprintf(stderr, "Failed to allocate polygon points.\n");
        return -1;
      }
      memcpy(polygon, keyframes[i].points, keyframes[i].point_count * sizeof(*polygon));
      *points_out = polygon;
      *count_out = keyframes[i].point_count;
      *affected_out = true;
      return 0;
    }
  }

  for (size_t i = 0; i + 1u < keyframe_count; ++i) {
    const PolygonKeyframe *lower = &keyframes[i];
    const PolygonKeyframe *upper = &keyframes[i + 1u];
    if (z < lower->z || z > upper->z) {
      continue;
    }

    const double span = upper->z - lower->z;
    if (span <= 0.0) {
      continue;
    }
    const double t = (z - lower->z) / span;
    Point2D *polygon = calloc(lower->point_count, sizeof(*polygon));
    if (polygon == NULL) {
      fprintf(stderr, "Failed to allocate interpolated polygon.\n");
      return -1;
    }

    for (size_t j = 0; j < lower->point_count; ++j) {
      polygon[j].x = lower->points[j].x + (upper->points[j].x - lower->points[j].x) * t;
      polygon[j].y = lower->points[j].y + (upper->points[j].y - lower->points[j].y) * t;
    }

    *points_out = polygon;
    *count_out = lower->point_count;
    *affected_out = true;
    return 0;
  }

  return 0;
}

static void fill_polygon(Image *image, const Point2D *points, size_t count, const FillRegionColor *color) {
  if (image == NULL || points == NULL || count < 3u || color == NULL) {
    return;
  }

  double min_y = points[0].y;
  double max_y = points[0].y;
  for (size_t i = 1; i < count; ++i) {
    if (points[i].y < min_y) {
      min_y = points[i].y;
    }
    if (points[i].y > max_y) {
      max_y = points[i].y;
    }
  }

  int32_t start_y = (int32_t) ceil(min_y - 0.5);
  int32_t end_y = (int32_t) floor(max_y - 0.5);
  if (start_y < 0) {
    start_y = 0;
  }
  if (end_y >= (int32_t) image->height) {
    end_y = (int32_t) image->height - 1;
  }

  double *intersections = calloc(count, sizeof(*intersections));
  if (intersections == NULL) {
    fprintf(stderr, "Failed to allocate polygon scanline intersections.\n");
    return;
  }

  for (int32_t y = start_y; y <= end_y; ++y) {
    const double scan_y = (double) y + 0.5;
    size_t intersection_count = 0;

    for (size_t i = 0; i < count; ++i) {
      const Point2D a = points[i];
      const Point2D b = points[(i + 1u) % count];
      const bool crosses =
          (a.y <= scan_y && b.y > scan_y) || (b.y <= scan_y && a.y > scan_y);
      if (!crosses) {
        continue;
      }

      intersections[intersection_count++] =
          a.x + (scan_y - a.y) * (b.x - a.x) / (b.y - a.y);
    }

    qsort(intersections, intersection_count, sizeof(*intersections), compare_doubles);
    for (size_t i = 0; i + 1u < intersection_count; i += 2u) {
      const int32_t start_x = (int32_t) ceil(intersections[i] - 0.5);
      const int32_t end_x = (int32_t) floor(intersections[i + 1u] - 0.5);
      const int32_t clipped_start = start_x < 0 ? 0 : start_x;
      const int32_t clipped_end =
          end_x >= (int32_t) image->width ? (int32_t) image->width - 1 : end_x;
      for (int32_t x = clipped_start; x <= clipped_end; ++x) {
        set_pixel(image, x, y, color);
      }
    }
  }

  free(intersections);
}

static int compare_doubles(const void *lhs, const void *rhs) {
  const double a = *(const double *) lhs;
  const double b = *(const double *) rhs;
  if (a < b) {
    return -1;
  }
  if (a > b) {
    return 1;
  }
  return 0;
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
  static bool initialized = false;
  static uint32_t table[256];
  if (!initialized) {
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int bit = 0; bit < 8; ++bit) {
        c = (c & 1u) != 0u ? 0xedb88320u ^ (c >> 1) : (c >> 1);
      }
      table[n] = c;
    }
    initialized = true;
  }

  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < length; ++i) {
    crc = table[(crc ^ bytes[i]) & 0xffu] ^ (crc >> 8);
  }
  return crc ^ 0xffffffffu;
}

static uint32_t adler32_bytes(const uint8_t *bytes, size_t length) {
  uint32_t a = 1;
  uint32_t b = 0;
  const uint32_t mod = 65521;
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
  if (length > 0 && data != NULL) {
    memcpy(crc_buffer + 4, data, length);
  }
  write_u32_be(crc_be, crc32_bytes(crc_buffer, crc_buffer_size));

  const int failed =
      fwrite(length_be, 1, sizeof(length_be), file) != sizeof(length_be) ||
      fwrite(type, 1, 4, file) != 4 ||
      (length > 0 && fwrite(data, 1, length, file) != length) ||
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

  size_t src = 0;
  size_t dst = 0;
  for (uint32_t y = 0; y < image->height; ++y) {
    raw[dst++] = 0;
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

  size_t zoff = 0;
  zlib_stream[zoff++] = 0x78;
  zlib_stream[zoff++] = 0x01;

  size_t raw_offset = 0;
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
      0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
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
    ihdr[8] = 8;
    ihdr[9] = 6;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;

    if (append_chunk(file, "IHDR", ihdr, sizeof(ihdr)) != 0 ||
        append_chunk(file, "IDAT", zlib_stream, (uint32_t) zoff) != 0 ||
        append_chunk(file, "IEND", NULL, 0) != 0) {
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
      0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
  if (file_size < sizeof(signature) || memcmp(bytes, signature, sizeof(signature)) != 0) {
    fprintf(stderr, "Unsupported PNG signature: %s\n", path);
    free(bytes);
    return -1;
  }

  size_t offset = 8u;
  bool saw_ihdr = false;
  bool saw_iend = false;
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t *idat = NULL;
  size_t idat_size = 0;

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

    if (memcmp(chunk_type, "IHDR", 4) == 0) {
      if (chunk_length != 13u) {
        fprintf(stderr, "Invalid IHDR chunk in %s\n", path);
        free(idat);
        free(bytes);
        return -1;
      }
      width = read_u32_be(chunk_data);
      height = read_u32_be(chunk_data + 4);
      if (chunk_data[8] != 8 || chunk_data[9] != 6 ||
          chunk_data[10] != 0 || chunk_data[11] != 0 || chunk_data[12] != 0) {
        fprintf(stderr, "Unsupported PNG format in %s\n", path);
        free(idat);
        free(bytes);
        return -1;
      }
      saw_ihdr = true;
    } else if (memcmp(chunk_type, "IDAT", 4) == 0) {
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
    } else if (memcmp(chunk_type, "IEND", 4) == 0) {
      saw_iend = true;
      break;
    }
  }

  if (!saw_ihdr || !saw_iend || width == 0 || height == 0 || idat_size < 6u) {
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
  size_t raw_offset = 0;
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
        (uint16_t) compressed[offset] | (uint16_t) ((uint16_t) compressed[offset + 1] << 8);
    offset += 2u;
    const uint16_t nlen =
        (uint16_t) compressed[offset] | (uint16_t) ((uint16_t) compressed[offset + 1] << 8);
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

static double now_seconds(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0.0;
  }
  return (double) ts.tv_sec + (double) ts.tv_nsec / 1000000000.0;
}

static void format_duration(double seconds, char *buffer, size_t buffer_size) {
  if (buffer == NULL || buffer_size == 0u) {
    return;
  }

  if (seconds < 0.0) {
    seconds = 0.0;
  }

  const unsigned long total_seconds = (unsigned long) (seconds + 0.5);
  const unsigned long hours = total_seconds / 3600ul;
  const unsigned long minutes = (total_seconds % 3600ul) / 60ul;
  const unsigned long secs = total_seconds % 60ul;
  if (hours > 0ul) {
    snprintf(buffer, buffer_size, "%luh%02lum%02lus", hours, minutes, secs);
    return;
  }
  snprintf(buffer, buffer_size, "%lum%02lus", minutes, secs);
}
