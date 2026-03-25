#include "slice_pipeline.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define SLICE_BACKGROUND_R 247
#define SLICE_BACKGROUND_G 247
#define SLICE_BACKGROUND_B 247
#define SLICE_BACKGROUND_A 128
#define SLICE_MAX_VERTEX_PROPERTIES 64

typedef enum {
  PLY_FORMAT_UNKNOWN = 0,
  PLY_FORMAT_ASCII,
  PLY_FORMAT_BINARY_LE,
} PlyFormat;

typedef enum {
  PLY_TYPE_INVALID = 0,
  PLY_TYPE_INT8,
  PLY_TYPE_UINT8,
  PLY_TYPE_INT16,
  PLY_TYPE_UINT16,
  PLY_TYPE_INT32,
  PLY_TYPE_UINT32,
  PLY_TYPE_FLOAT32,
  PLY_TYPE_FLOAT64,
} PlyScalarType;

typedef struct {
  char name[32];
  PlyScalarType type;
  size_t offset;
} PlyProperty;

typedef struct {
  PlyFormat format;
  uint64_t vertex_count;
  size_t property_count;
  size_t stride;
  long data_offset;
  int x_index;
  int y_index;
  int z_index;
  int r_index;
  int g_index;
  int b_index;
  int a_index;
  PlyProperty properties[SLICE_MAX_VERTEX_PROPERTIES];
} PlyHeader;

typedef struct {
  SlicePoint *points;
  size_t count;
  double min_x;
  double min_y;
  double min_z;
  double max_x;
  double max_y;
  double max_z;
} PointCloud;

typedef struct KdNode {
  SlicePoint *point;
  int axis;
  struct KdNode *left;
  struct KdNode *right;
} KdNode;

typedef struct {
  uint32_t width;
  uint32_t height;
  size_t stride;
  uint8_t *pixels;
} Image;

typedef struct {
  double x;
  double y;
  double z;
} Vec3;

typedef struct {
  Vec3 target;
  const SlicePoint *best_point;
  double best_nd2;
  double best_euclid_d2;
  double inv_sx;
  double inv_sy;
  double inv_sz;
  bool cutoff_active;
  bool exclude_self;
  unsigned axes_mask;
} SearchState;

typedef struct {
  const char *label;
  uint64_t total;
  uint64_t interval;
  uint64_t next_log;
  uint64_t last_logged;
  double started_at;
} SliceProgressLogger;

static int ensure_input_file(const char *path);
static int mkdir_recursive(const char *path);
static int load_ply(const char *path, uint64_t log_interval, PointCloud *cloud_out);
static void free_point_cloud(PointCloud *cloud);
static int init_image(Image *image, uint32_t width, uint32_t height);
static void free_image(Image *image);
static void fill_image(Image *image, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
static void set_pixel(
    Image *image,
    uint32_t x,
    uint32_t y,
    uint8_t r,
    uint8_t g,
    uint8_t b,
    uint8_t a);
static int write_png(const char *path, const Image *image);
static KdNode *build_kd_tree(SlicePoint *points, size_t count);
static void free_kd_tree(KdNode *node);
static const SlicePoint *nearest_point(
    const KdNode *root,
    Vec3 target,
    double sx,
    double sy,
    double sz,
    double *distance_out);
static uint8_t output_alpha_for_point(const SlicePoint *point);

static PlyScalarType parse_ply_scalar_type(const char *type_name);
static size_t ply_scalar_type_size(PlyScalarType type);
static double read_scalar_ascii(const char *token);
static double read_scalar_binary(const uint8_t *record, size_t offset, PlyScalarType type);
static uint8_t normalize_color_value(double value, PlyScalarType type);
static bool trim_line(char *line);
static int parse_ply_header(FILE *file, PlyHeader *header_out);
static double slice_now_seconds(void);
static const char *slice_format_duration(double total_seconds, char *buffer, size_t buffer_size);
static void slice_progress_logger_init(
    SliceProgressLogger *progress,
    const char *label,
    uint64_t total,
    uint64_t interval,
    double started_at);
static void slice_progress_logger_log(
    SliceProgressLogger *progress,
    uint64_t processed,
    bool force);

static void update_progress(size_t layer_index, size_t depth) {
  const double percent = depth == 0 ? 100.0 : (100.0 * (double) layer_index) / (double) depth;
  fprintf(
      stdout,
      "\rSlicing | %6.2f%% | %zu/%zu layers",
      percent,
      layer_index,
      depth);
  fflush(stdout);
}

int run_slice(const SliceOptions *options) {
  if (ensure_input_file(options->input_path) != 0) {
    return -1;
  }
  if (mkdir_recursive(options->output_dir) != 0) {
    return -1;
  }

  printf("Loading PLY into memory...\n");

  PointCloud cloud;
  memset(&cloud, 0, sizeof(cloud));
  const double load_started_at = slice_now_seconds();
  if (load_ply(options->input_path, options->log_interval, &cloud) != 0) {
    free_point_cloud(&cloud);
    return -1;
  }
  char load_elapsed[64];
  printf(
      "Load PLY into memory finished in %s\n",
      slice_format_duration(
          slice_now_seconds() - load_started_at,
          load_elapsed,
          sizeof(load_elapsed)));

  printf(
      "Bounds min=(%.6f, %.6f, %.6f) max=(%.6f, %.6f, %.6f)\n",
      cloud.min_x,
      cloud.min_y,
      cloud.min_z,
      cloud.max_x,
      cloud.max_y,
      cloud.max_z);

  double min_x = cloud.min_x;
  double min_y = cloud.min_y;
  double min_z = cloud.min_z;
  double max_x = cloud.max_x;
  double max_y = cloud.max_y;
  double max_z = cloud.max_z;

  const double x_pad = (max_x - min_x) * options->padding_ratio;
  const double y_pad = (max_y - min_y) * options->padding_ratio;
  const double z_pad = 0.0;

  min_x -= x_pad;
  min_y -= y_pad;
  min_z -= z_pad;
  max_x += x_pad;
  max_y += y_pad;
  max_z += z_pad;

  const double x_size = max_x - min_x;
  const double y_size = max_y - min_y;
  const double z_size = max_z - min_z;

  double x_in = options->x_in;
  double y_in = options->y_in;
  double z_in = options->z_in;

  if (!options->x_in_set || !options->y_in_set || !options->z_in_set) {
    const double max_model = fmax(x_size, fmax(y_size, z_size));
    if (max_model <= 0.0) {
      fprintf(
          stderr,
          "Cannot derive auto dimensions from a degenerate point cloud.\n");
      free_point_cloud(&cloud);
      return -1;
    }

    const double scale = options->longest_side_in / max_model;
    if (!options->x_in_set) {
      x_in = x_size * scale;
    }
    if (!options->y_in_set) {
      y_in = y_size * scale;
    }
    if (!options->z_in_set) {
      z_in = z_size * scale;
    }
  }

  if (x_in <= 0.0 || y_in <= 0.0 || z_in <= 0.0) {
    fprintf(stderr, "Physical dimensions must be greater than zero.\n");
    free_point_cloud(&cloud);
    return -1;
  }

  const uint32_t width = (uint32_t) llround(fmax(1.0, round(x_in * options->dpi)));
  const uint32_t height = (uint32_t) llround(fmax(1.0, round(y_in * options->dpi)));
  const uint32_t depth = (uint32_t) llround(
      fmax(1.0, round((z_in * SLICE_NM_PER_INCH) / options->layer_height_nm)));

  printf("Physical (in): %.3f x %.3f x %.3f\n", x_in, y_in, z_in);
  printf(
      "Pixels/Layers: %u x %u x %u  @ %.0f dpi, %.0f nm\n",
      width,
      height,
      depth,
      options->dpi,
      options->layer_height_nm);

  const double model_units_per_inch_x = x_size / x_in;
  const double model_units_per_inch_y = y_size / y_in;
  const double model_units_per_inch_z = z_size / z_in;
  const double model_units_per_inch =
      (model_units_per_inch_x + model_units_per_inch_y + model_units_per_inch_z) /
      3.0;

  const double voxel_radius = options->voxel_radius_inches * model_units_per_inch;
  printf(
      "Voxel radius: %.4f in  ->  %.2f model units\n",
      options->voxel_radius_inches,
      voxel_radius);

  KdNode *root = build_kd_tree(cloud.points, cloud.count);
  if (root == NULL && cloud.count > 0) {
    fprintf(stderr, "Failed to build the kd-tree.\n");
    free_point_cloud(&cloud);
    return -1;
  }

  Image image;
  memset(&image, 0, sizeof(image));
  if (init_image(&image, width, height) != 0) {
    free_kd_tree(root);
    free_point_cloud(&cloud);
    return -1;
  }

  const double layer_thickness = depth == 0 ? 0.0 : z_size / (double) depth;
  update_progress(0, depth);

  for (uint32_t z = 0; z < depth; ++z) {
    const double z_world = min_z + (double) z * layer_thickness;
    fill_image(
        &image,
        SLICE_BACKGROUND_R,
        SLICE_BACKGROUND_G,
        SLICE_BACKGROUND_B,
        SLICE_BACKGROUND_A);

    for (uint32_t column = 0; column < width; ++column) {
      const double x = min_x + (((double) column + 0.5) / (double) width) * x_size;

      for (uint32_t row = 0; row < height; ++row) {
        const double y = min_y + (((double) row + 0.5) / (double) height) * y_size;
        double distance = 0.0;
        const SlicePoint *point = nearest_point(
            root,
            (Vec3) {x, y, z_world},
            voxel_radius,
            voxel_radius,
            voxel_radius,
            &distance);
        if (point == NULL || distance > voxel_radius) {
          continue;
        }

        set_pixel(
            &image,
            column,
            row,
            point->r,
            point->g,
            point->b,
            output_alpha_for_point(point));
      }
    }

    const size_t out_path_len =
        strlen(options->output_dir) + 32;
    char *out_path = malloc(out_path_len);
    if (out_path == NULL) {
      fprintf(stderr, "Failed to allocate the output path.\n");
      free_image(&image);
      free_kd_tree(root);
      free_point_cloud(&cloud);
      return -1;
    }

    snprintf(out_path, out_path_len, "%s/out_%u.png", options->output_dir, z);
    if (write_png(out_path, &image) != 0) {
      free(out_path);
      free_image(&image);
      free_kd_tree(root);
      free_point_cloud(&cloud);
      return -1;
    }
    free(out_path);

    update_progress((size_t) z + 1, depth);
  }

  fprintf(stdout, "\nDone.\n");

  free_image(&image);
  free_kd_tree(root);
  free_point_cloud(&cloud);
  return 0;
}

static int ensure_input_file(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) {
    fprintf(stderr, "Input file not found: %s\n", path);
    return -1;
  }
  return 0;
}

static int mkdir_recursive(const char *path) {
  if (path == NULL || *path == '\0') {
    fprintf(stderr, "Output directory path is empty.\n");
    return -1;
  }

  char *mutable_path = strdup(path);
  if (mutable_path == NULL) {
    fprintf(stderr, "Failed to allocate the output directory path.\n");
    return -1;
  }

  const size_t length = strlen(mutable_path);
  if (length == 0) {
    free(mutable_path);
    return -1;
  }

  for (size_t i = 1; i <= length; ++i) {
    if (mutable_path[i] != '/' && mutable_path[i] != '\0') {
      continue;
    }

    const char saved = mutable_path[i];
    mutable_path[i] = '\0';

    if (mutable_path[0] != '\0') {
      if (mkdir(mutable_path, 0777) != 0 && errno != EEXIST) {
        fprintf(
            stderr,
            "Failed to create directory %s: %s\n",
            mutable_path,
            strerror(errno));
        free(mutable_path);
        return -1;
      }
    }

    mutable_path[i] = saved;
  }

  free(mutable_path);
  return 0;
}

static void free_point_cloud(PointCloud *cloud) {
  if (cloud == NULL) {
    return;
  }
  free(cloud->points);
  memset(cloud, 0, sizeof(*cloud));
}

static bool trim_line(char *line) {
  if (line == NULL) {
    return false;
  }

  size_t length = strlen(line);
  while (length > 0 &&
         (line[length - 1] == '\n' || line[length - 1] == '\r')) {
    line[--length] = '\0';
  }

  size_t start = 0;
  while (line[start] != '\0' && isspace((unsigned char) line[start])) {
    ++start;
  }

  if (start > 0) {
    memmove(line, line + start, strlen(line + start) + 1);
  }

  if (line[0] == '\0') {
    return false;
  }

  length = strlen(line);
  while (length > 0 && isspace((unsigned char) line[length - 1])) {
    line[--length] = '\0';
  }

  return line[0] != '\0';
}

static double slice_now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

static const char *slice_format_duration(
    double total_seconds,
    char *buffer,
    size_t buffer_size) {
  if (!isfinite(total_seconds) || total_seconds < 0.0) {
    snprintf(buffer, buffer_size, "n/a");
    return buffer;
  }

  long long rounded_seconds = llround(total_seconds);
  long long days = rounded_seconds / 86400;
  long long hours = (rounded_seconds % 86400) / 3600;
  long long minutes = (rounded_seconds % 3600) / 60;
  long long seconds = rounded_seconds % 60;

  if (days > 0) {
    snprintf(buffer, buffer_size, "%lldd %lldh %lldm %llds", days, hours, minutes, seconds);
    return buffer;
  }
  if (hours > 0) {
    snprintf(buffer, buffer_size, "%lldh %lldm %llds", hours, minutes, seconds);
    return buffer;
  }
  if (minutes > 0) {
    snprintf(buffer, buffer_size, "%lldm %llds", minutes, seconds);
    return buffer;
  }
  if (rounded_seconds > 0) {
    snprintf(buffer, buffer_size, "%llds", rounded_seconds);
    return buffer;
  }

  snprintf(buffer, buffer_size, "%.1fms", total_seconds * 1000.0);
  return buffer;
}

static void slice_progress_logger_init(
    SliceProgressLogger *progress,
    const char *label,
    uint64_t total,
    uint64_t interval,
    double started_at) {
  if (progress == NULL) {
    return;
  }

  progress->label = label;
  progress->total = total;
  progress->interval = interval;
  progress->next_log = interval > 0 ? interval : UINT64_MAX;
  progress->last_logged = 0;
  progress->started_at = started_at;
}

static void slice_progress_logger_log(
    SliceProgressLogger *progress,
    uint64_t processed,
    bool force) {
  if (progress == NULL || progress->interval == 0) {
    return;
  }
  if (force && processed == progress->last_logged) {
    return;
  }
  if (!force && processed < progress->next_log) {
    return;
  }

  char elapsed[64];
  double elapsed_seconds = slice_now_seconds() - progress->started_at;

  if (progress->total > 0) {
    double percent = ((double) processed * 100.0) / (double) progress->total;
    char eta[64];
    const char *eta_text = "n/a";
    if (processed > 0 && processed < progress->total) {
      double seconds_per_point = elapsed_seconds / (double) processed;
      double eta_seconds = seconds_per_point * (double) (progress->total - processed);
      eta_text = slice_format_duration(eta_seconds, eta, sizeof(eta));
    } else if (processed >= progress->total) {
      eta_text = slice_format_duration(0.0, eta, sizeof(eta));
    }

    printf(
        "%s progress: %" PRIu64 "/%" PRIu64 " (%.2f%%, elapsed %s, estimated remaining %s)\n",
        progress->label,
        processed,
        progress->total,
        percent,
        slice_format_duration(elapsed_seconds, elapsed, sizeof(elapsed)),
        eta_text);
  } else {
    printf(
        "%s progress: %" PRIu64 " points (elapsed %s)\n",
        progress->label,
        processed,
        slice_format_duration(elapsed_seconds, elapsed, sizeof(elapsed)));
  }

  fflush(stdout);
  progress->last_logged = processed;

  if (force) {
    while (progress->next_log <= processed) {
      if (UINT64_MAX - progress->next_log < progress->interval) {
        progress->next_log = UINT64_MAX;
        break;
      }
      progress->next_log += progress->interval;
    }
    return;
  }

  while (progress->next_log <= processed) {
    if (UINT64_MAX - progress->next_log < progress->interval) {
      progress->next_log = UINT64_MAX;
      break;
    }
    progress->next_log += progress->interval;
  }
}

static PlyScalarType parse_ply_scalar_type(const char *type_name) {
  if (strcasecmp(type_name, "char") == 0 || strcasecmp(type_name, "int8") == 0) {
    return PLY_TYPE_INT8;
  }
  if (strcasecmp(type_name, "uchar") == 0 || strcasecmp(type_name, "uint8") == 0) {
    return PLY_TYPE_UINT8;
  }
  if (strcasecmp(type_name, "short") == 0 || strcasecmp(type_name, "int16") == 0) {
    return PLY_TYPE_INT16;
  }
  if (strcasecmp(type_name, "ushort") == 0 || strcasecmp(type_name, "uint16") == 0) {
    return PLY_TYPE_UINT16;
  }
  if (strcasecmp(type_name, "int") == 0 || strcasecmp(type_name, "int32") == 0) {
    return PLY_TYPE_INT32;
  }
  if (strcasecmp(type_name, "uint") == 0 || strcasecmp(type_name, "uint32") == 0) {
    return PLY_TYPE_UINT32;
  }
  if (strcasecmp(type_name, "float") == 0 || strcasecmp(type_name, "float32") == 0) {
    return PLY_TYPE_FLOAT32;
  }
  if (strcasecmp(type_name, "double") == 0 || strcasecmp(type_name, "float64") == 0) {
    return PLY_TYPE_FLOAT64;
  }
  return PLY_TYPE_INVALID;
}

static size_t ply_scalar_type_size(PlyScalarType type) {
  switch (type) {
    case PLY_TYPE_INT8:
    case PLY_TYPE_UINT8:
      return 1;
    case PLY_TYPE_INT16:
    case PLY_TYPE_UINT16:
      return 2;
    case PLY_TYPE_INT32:
    case PLY_TYPE_UINT32:
    case PLY_TYPE_FLOAT32:
      return 4;
    case PLY_TYPE_FLOAT64:
      return 8;
    default:
      return 0;
  }
}

static int parse_ply_header(FILE *file, PlyHeader *header_out) {
  PlyHeader header;
  memset(&header, 0, sizeof(header));
  header.x_index = -1;
  header.y_index = -1;
  header.z_index = -1;
  header.r_index = -1;
  header.g_index = -1;
  header.b_index = -1;
  header.a_index = -1;

  char *line = NULL;
  size_t line_capacity = 0;
  ssize_t line_length = 0;
  bool saw_ply = false;
  bool in_vertex_element = false;

  while ((line_length = getline(&line, &line_capacity, file)) != -1) {
    (void) line_length;
    if (!trim_line(line)) {
      continue;
    }

    if (!saw_ply) {
      if (strcasecmp(line, "ply") != 0) {
        fprintf(stderr, "Not a PLY file.\n");
        free(line);
        return -1;
      }
      saw_ply = true;
      continue;
    }

    if (strcasecmp(line, "end_header") == 0) {
      header.data_offset = ftell(file);
      break;
    }

    if (strncmp(line, "comment", 7) == 0) {
      continue;
    }

    if (strncmp(line, "format ", 7) == 0) {
      if (strstr(line, "ascii") != NULL) {
        header.format = PLY_FORMAT_ASCII;
      } else if (strstr(line, "binary_little_endian") != NULL) {
        header.format = PLY_FORMAT_BINARY_LE;
      } else {
        fprintf(stderr, "Unsupported PLY format: %s\n", line + 7);
        free(line);
        return -1;
      }
      continue;
    }

    if (strncmp(line, "element ", 8) == 0) {
      char name[64];
      unsigned long long count = 0;
      if (sscanf(line, "element %63s %llu", name, &count) != 2) {
        fprintf(stderr, "Invalid PLY element declaration.\n");
        free(line);
        return -1;
      }

      in_vertex_element = strcmp(name, "vertex") == 0;
      if (in_vertex_element) {
        header.vertex_count = (uint64_t) count;
      }
      continue;
    }

    if (!in_vertex_element || strncmp(line, "property ", 9) != 0) {
      continue;
    }

    char type_name[64];
    char property_name[64];
    if (sscanf(line, "property list %63s %63s", type_name, property_name) == 2) {
      fprintf(
          stderr,
          "PLY vertex list properties are not supported by this slicer.\n");
      free(line);
      return -1;
    }

    if (sscanf(line, "property %63s %63s", type_name, property_name) != 2) {
      fprintf(stderr, "Invalid PLY property declaration.\n");
      free(line);
      return -1;
    }

    if (header.property_count >= SLICE_MAX_VERTEX_PROPERTIES) {
      fprintf(stderr, "PLY has too many vertex properties.\n");
      free(line);
      return -1;
    }

    PlyScalarType scalar_type = parse_ply_scalar_type(type_name);
    if (scalar_type == PLY_TYPE_INVALID) {
      fprintf(stderr, "Unsupported PLY property type: %s\n", type_name);
      free(line);
      return -1;
    }

    PlyProperty *property = &header.properties[header.property_count];
    memset(property, 0, sizeof(*property));
    const size_t property_name_len = strlen(property_name);
    if (property_name_len >= sizeof(property->name)) {
      fprintf(stderr, "PLY property name is too long: %s\n", property_name);
      free(line);
      return -1;
    }
    memcpy(property->name, property_name, property_name_len + 1);
    property->type = scalar_type;
    property->offset = header.stride;
    header.stride += ply_scalar_type_size(scalar_type);

    if (strcmp(property_name, "x") == 0) {
      header.x_index = (int) header.property_count;
    } else if (strcmp(property_name, "y") == 0) {
      header.y_index = (int) header.property_count;
    } else if (strcmp(property_name, "z") == 0) {
      header.z_index = (int) header.property_count;
    } else if (strcmp(property_name, "red") == 0 || strcmp(property_name, "r") == 0) {
      header.r_index = (int) header.property_count;
    } else if (
        strcmp(property_name, "green") == 0 || strcmp(property_name, "g") == 0) {
      header.g_index = (int) header.property_count;
    } else if (
        strcmp(property_name, "blue") == 0 || strcmp(property_name, "b") == 0) {
      header.b_index = (int) header.property_count;
    } else if (
        strcmp(property_name, "alpha") == 0 || strcmp(property_name, "a") == 0) {
      header.a_index = (int) header.property_count;
    }

    ++header.property_count;
  }

  free(line);

  if (!saw_ply || header.data_offset == 0 || header.format == PLY_FORMAT_UNKNOWN) {
    fprintf(stderr, "Invalid PLY header.\n");
    return -1;
  }

  if (header.vertex_count == 0) {
    fprintf(stderr, "PLY has no vertex element.\n");
    return -1;
  }

  if (header.x_index < 0 || header.y_index < 0 || header.z_index < 0) {
    fprintf(stderr, "PLY vertex must include x, y, and z.\n");
    return -1;
  }

  *header_out = header;
  return 0;
}

static double read_scalar_ascii(const char *token) {
  if (token == NULL) {
    return 0.0;
  }
  return strtod(token, NULL);
}

static double read_scalar_binary(const uint8_t *record, size_t offset, PlyScalarType type) {
  const uint8_t *ptr = record + offset;
  switch (type) {
    case PLY_TYPE_INT8:
      return (double) *((const int8_t *) ptr);
    case PLY_TYPE_UINT8:
      return (double) *ptr;
    case PLY_TYPE_INT16: {
      int16_t value;
      memcpy(&value, ptr, sizeof(value));
      return (double) value;
    }
    case PLY_TYPE_UINT16: {
      uint16_t value;
      memcpy(&value, ptr, sizeof(value));
      return (double) value;
    }
    case PLY_TYPE_INT32: {
      int32_t value;
      memcpy(&value, ptr, sizeof(value));
      return (double) value;
    }
    case PLY_TYPE_UINT32: {
      uint32_t value;
      memcpy(&value, ptr, sizeof(value));
      return (double) value;
    }
    case PLY_TYPE_FLOAT32: {
      float value;
      memcpy(&value, ptr, sizeof(value));
      return (double) value;
    }
    case PLY_TYPE_FLOAT64: {
      double value;
      memcpy(&value, ptr, sizeof(value));
      return value;
    }
    default:
      return 0.0;
  }
}

static uint8_t normalize_color_value(double value, PlyScalarType type) {
  if (!isfinite(value)) {
    return 0;
  }

  switch (type) {
    case PLY_TYPE_FLOAT32:
    case PLY_TYPE_FLOAT64:
      if (value >= 0.0 && value <= 1.0) {
        value = round(value * 255.0);
      } else {
        value = round(value);
      }
      break;
    case PLY_TYPE_UINT16:
      value = round(fmax(0.0, fmin(65535.0, value)) * 255.0 / 65535.0);
      break;
    case PLY_TYPE_UINT32:
      value = round(fmax(0.0, fmin(4294967295.0, value)) * 255.0 / 4294967295.0);
      break;
    default:
      if (value >= 0.0 && value <= 1.0) {
        value = round(value * 255.0);
      } else {
        value = round(value);
      }
      break;
  }

  if (value < 0.0) {
    value = 0.0;
  }
  if (value > 255.0) {
    value = 255.0;
  }
  return (uint8_t) value;
}

static int load_ply(const char *path, uint64_t log_interval, PointCloud *cloud_out) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
    return -1;
  }

  PlyHeader header;
  if (parse_ply_header(file, &header) != 0) {
    fclose(file);
    return -1;
  }

  if (header.vertex_count > SIZE_MAX / sizeof(SlicePoint)) {
    fprintf(stderr, "PLY vertex count is too large.\n");
    fclose(file);
    return -1;
  }

  SlicePoint *points = calloc((size_t) header.vertex_count, sizeof(SlicePoint));
  if (points == NULL) {
    fprintf(stderr, "Failed to allocate point cloud storage.\n");
    fclose(file);
    return -1;
  }

  double min_x = INFINITY;
  double min_y = INFINITY;
  double min_z = INFINITY;
  double max_x = -INFINITY;
  double max_y = -INFINITY;
  double max_z = -INFINITY;
  SliceProgressLogger progress;
  slice_progress_logger_init(
      &progress,
      "Load PLY into memory",
      header.vertex_count,
      log_interval,
      slice_now_seconds());

  if (header.format == PLY_FORMAT_ASCII) {
    char *line = NULL;
    size_t line_capacity = 0;
    ssize_t line_length = 0;
    size_t processed = 0;

    while (processed < (size_t) header.vertex_count &&
           (line_length = getline(&line, &line_capacity, file)) != -1) {
      (void) line_length;
      if (!trim_line(line) || strncmp(line, "comment", 7) == 0) {
        continue;
      }

      double values[SLICE_MAX_VERTEX_PROPERTIES];
      size_t value_count = 0;
      char *cursor = line;
      char *token = NULL;
      while ((token = strtok_r(cursor, " \t", &cursor)) != NULL) {
        if (value_count >= header.property_count) {
          break;
        }
        values[value_count++] = read_scalar_ascii(token);
      }

      if (value_count < header.property_count) {
        fprintf(stderr, "PLY ASCII parse error near vertex %zu.\n", processed);
        free(line);
        free(points);
        fclose(file);
        return -1;
      }

      SlicePoint point;
      memset(&point, 0, sizeof(point));
      point.x = values[header.x_index];
      point.y = values[header.y_index];
      point.z = values[header.z_index];

      if (header.r_index >= 0 && header.g_index >= 0 && header.b_index >= 0) {
        point.has_color = true;
        point.r = normalize_color_value(
            values[header.r_index], header.properties[header.r_index].type);
        point.g = normalize_color_value(
            values[header.g_index], header.properties[header.g_index].type);
        point.b = normalize_color_value(
            values[header.b_index], header.properties[header.b_index].type);
        if (header.a_index >= 0) {
          point.has_alpha = true;
          point.a = normalize_color_value(
              values[header.a_index], header.properties[header.a_index].type);
        }
      }

      points[processed++] = point;
      slice_progress_logger_log(&progress, (uint64_t) processed, false);

      if (point.x < min_x) min_x = point.x;
      if (point.y < min_y) min_y = point.y;
      if (point.z < min_z) min_z = point.z;
      if (point.x > max_x) max_x = point.x;
      if (point.y > max_y) max_y = point.y;
      if (point.z > max_z) max_z = point.z;
    }

    free(line);

    if (feof(file) == 0 && ferror(file) != 0) {
      fprintf(stderr, "Failed while reading %s.\n", path);
      free(points);
      fclose(file);
      return -1;
    }

    if (processed != (size_t) header.vertex_count) {
      fprintf(
          stderr,
          "PLY ASCII parse error: expected %" PRIu64 " vertices, got %zu\n",
          header.vertex_count,
          processed);
      free(points);
      fclose(file);
      return -1;
    }
  } else {
    uint8_t *record = malloc(header.stride);
    if (record == NULL) {
      fprintf(stderr, "Failed to allocate a binary record buffer.\n");
      free(points);
      fclose(file);
      return -1;
    }

    for (uint64_t i = 0; i < header.vertex_count; ++i) {
      if (fread(record, 1, header.stride, file) != header.stride) {
        fprintf(
            stderr,
            "PLY binary parse error: expected %" PRIu64 " vertices, got %" PRIu64 "\n",
            header.vertex_count,
            i);
        free(record);
        free(points);
        fclose(file);
        return -1;
      }

      SlicePoint point;
      memset(&point, 0, sizeof(point));
      point.x = read_scalar_binary(
          record,
          header.properties[header.x_index].offset,
          header.properties[header.x_index].type);
      point.y = read_scalar_binary(
          record,
          header.properties[header.y_index].offset,
          header.properties[header.y_index].type);
      point.z = read_scalar_binary(
          record,
          header.properties[header.z_index].offset,
          header.properties[header.z_index].type);

      if (header.r_index >= 0 && header.g_index >= 0 && header.b_index >= 0) {
        point.has_color = true;
        point.r = normalize_color_value(
            read_scalar_binary(
                record,
                header.properties[header.r_index].offset,
                header.properties[header.r_index].type),
            header.properties[header.r_index].type);
        point.g = normalize_color_value(
            read_scalar_binary(
                record,
                header.properties[header.g_index].offset,
                header.properties[header.g_index].type),
            header.properties[header.g_index].type);
        point.b = normalize_color_value(
            read_scalar_binary(
                record,
                header.properties[header.b_index].offset,
                header.properties[header.b_index].type),
            header.properties[header.b_index].type);
        if (header.a_index >= 0) {
          point.has_alpha = true;
          point.a = normalize_color_value(
              read_scalar_binary(
                  record,
                  header.properties[header.a_index].offset,
                  header.properties[header.a_index].type),
              header.properties[header.a_index].type);
        }
      }

      points[i] = point;
      slice_progress_logger_log(&progress, i + 1, false);

      if (point.x < min_x) min_x = point.x;
      if (point.y < min_y) min_y = point.y;
      if (point.z < min_z) min_z = point.z;
      if (point.x > max_x) max_x = point.x;
      if (point.y > max_y) max_y = point.y;
      if (point.z > max_z) max_z = point.z;
    }

    free(record);
  }

  if (progress.interval > 0 &&
      (header.vertex_count % progress.interval) != 0) {
    slice_progress_logger_log(&progress, header.vertex_count, true);
  }

  fclose(file);

  cloud_out->points = points;
  cloud_out->count = (size_t) header.vertex_count;
  cloud_out->min_x = min_x;
  cloud_out->min_y = min_y;
  cloud_out->min_z = min_z;
  cloud_out->max_x = max_x;
  cloud_out->max_y = max_y;
  cloud_out->max_z = max_z;
  return 0;
}

static int init_image(Image *image, uint32_t width, uint32_t height) {
  image->width = width;
  image->height = height;
  image->stride = (size_t) width * 4u;
  if (height > 0 && image->stride > SIZE_MAX / height) {
    fprintf(stderr, "Requested image dimensions are too large.\n");
    return -1;
  }

  image->pixels = calloc(image->stride * height, 1);
  if (image->pixels == NULL) {
    fprintf(stderr, "Failed to allocate image pixels.\n");
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

static void fill_image(Image *image, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  for (uint32_t y = 0; y < image->height; ++y) {
    for (uint32_t x = 0; x < image->width; ++x) {
      const size_t offset = (size_t) y * image->stride + (size_t) x * 4u;
      image->pixels[offset] = r;
      image->pixels[offset + 1] = g;
      image->pixels[offset + 2] = b;
      image->pixels[offset + 3] = a;
    }
  }
}

static void set_pixel(
    Image *image,
    uint32_t x,
    uint32_t y,
    uint8_t r,
    uint8_t g,
    uint8_t b,
    uint8_t a) {
  if (x >= image->width || y >= image->height) {
    return;
  }
  const size_t offset = (size_t) y * image->stride + (size_t) x * 4u;
  image->pixels[offset] = r;
  image->pixels[offset + 1] = g;
  image->pixels[offset + 2] = b;
  image->pixels[offset + 3] = a;
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

  size_t crc_buffer_size = 4u + (size_t) length;
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

  int failed =
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
  const size_t raw_size =
      (size_t) image->height * (1u + image->stride);
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
    const uint16_t block_size =
        (uint16_t) (remaining > 65535u ? 65535u : remaining);
    const uint8_t final_block = (raw_offset + block_size >= raw_size) ? 1u : 0u;
    zlib_stream[zoff++] = final_block;
    zlib_stream[zoff++] = (uint8_t) (block_size & 0xffu);
    zlib_stream[zoff++] = (uint8_t) ((block_size >> 8) & 0xffu);
    const uint16_t nlen = (uint16_t) ~block_size;
    zlib_stream[zoff++] = (uint8_t) (nlen & 0xffu);
    zlib_stream[zoff++] = (uint8_t) ((nlen >> 8) & 0xffu);
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

static int kd_compare_axis = 0;

static int compare_point_ptrs(const void *lhs, const void *rhs) {
  const SlicePoint *const *a = lhs;
  const SlicePoint *const *b = rhs;
  const double av = kd_compare_axis == 0 ? (*a)->x : kd_compare_axis == 1 ? (*a)->y : (*a)->z;
  const double bv = kd_compare_axis == 0 ? (*b)->x : kd_compare_axis == 1 ? (*b)->y : (*b)->z;
  if (av < bv) {
    return -1;
  }
  if (av > bv) {
    return 1;
  }
  return 0;
}

static KdNode *build_kd_subtree(SlicePoint **point_ptrs, size_t count, int depth) {
  if (count == 0) {
    return NULL;
  }

  const int axis = depth % 3;
  kd_compare_axis = axis;
  qsort(point_ptrs, count, sizeof(*point_ptrs), compare_point_ptrs);

  const size_t mid = count / 2u;
  KdNode *node = calloc(1, sizeof(*node));
  if (node == NULL) {
    return NULL;
  }

  node->point = point_ptrs[mid];
  node->axis = axis;
  node->left = build_kd_subtree(point_ptrs, mid, depth + 1);
  node->right = build_kd_subtree(point_ptrs + mid + 1, count - mid - 1, depth + 1);
  return node;
}

static KdNode *build_kd_tree(SlicePoint *points, size_t count) {
  if (count == 0) {
    return NULL;
  }

  SlicePoint **point_ptrs = malloc(count * sizeof(*point_ptrs));
  if (point_ptrs == NULL) {
    return NULL;
  }

  for (size_t i = 0; i < count; ++i) {
    point_ptrs[i] = &points[i];
  }

  KdNode *root = build_kd_subtree(point_ptrs, count, 0);
  free(point_ptrs);
  return root;
}

static void free_kd_tree(KdNode *node) {
  if (node == NULL) {
    return;
  }
  free_kd_tree(node->left);
  free_kd_tree(node->right);
  free(node);
}

static void kd_search(const KdNode *node, SearchState *state) {
  if (node == NULL) {
    return;
  }

  const SlicePoint *point = node->point;
  if (!(state->exclude_self &&
        point->x == state->target.x &&
        point->y == state->target.y &&
        point->z == state->target.z)) {
    const double dx = (state->axes_mask & 1u) != 0u ? point->x - state->target.x : 0.0;
    const double dy = (state->axes_mask & 2u) != 0u ? point->y - state->target.y : 0.0;
    const double dz = (state->axes_mask & 4u) != 0u ? point->z - state->target.z : 0.0;

    const double d2e = dx * dx + dy * dy + dz * dz;
    const double ndx = dx * state->inv_sx;
    const double ndy = dy * state->inv_sy;
    const double ndz = dz * state->inv_sz;
    const double d2n = ndx * ndx + ndy * ndy + ndz * ndz;

    if ((!state->cutoff_active || d2n <= 1.0) &&
        (d2n < state->best_nd2 ||
         (!state->cutoff_active && d2e < state->best_euclid_d2))) {
      state->best_nd2 = d2n;
      state->best_euclid_d2 = d2e;
      state->best_point = point;
    }
  }

  const unsigned axis_bit = 1u << (unsigned) node->axis;
  if ((state->axes_mask & axis_bit) == 0u) {
    kd_search(node->left, state);
    kd_search(node->right, state);
    return;
  }

  const double diff =
      node->axis == 0 ? state->target.x - point->x
      : node->axis == 1 ? state->target.y - point->y
                        : state->target.z - point->z;

  const KdNode *near = diff < 0.0 ? node->left : node->right;
  const KdNode *far = diff < 0.0 ? node->right : node->left;
  kd_search(near, state);

  if (far == NULL) {
    return;
  }

  if (!state->cutoff_active) {
    if (diff * diff < state->best_euclid_d2) {
      kd_search(far, state);
    }
    return;
  }

  const double diff_norm =
      node->axis == 0 ? diff * state->inv_sx
      : node->axis == 1 ? diff * state->inv_sy
                        : diff * state->inv_sz;
  if (diff_norm * diff_norm < state->best_nd2) {
    kd_search(far, state);
  }
}

static const SlicePoint *nearest_point(
    const KdNode *root,
    Vec3 target,
    double sx,
    double sy,
    double sz,
    double *distance_out) {
  if (root == NULL) {
    return NULL;
  }

  SearchState state;
  memset(&state, 0, sizeof(state));
  state.target = target;
  state.best_point = NULL;
  state.best_nd2 = 1.0;
  state.best_euclid_d2 = INFINITY;
  state.inv_sx = 1.0 / sx;
  state.inv_sy = 1.0 / sy;
  state.inv_sz = 1.0 / sz;
  state.cutoff_active = true;
  state.axes_mask = 1u | 2u | 4u;

  kd_search(root, &state);
  if (state.best_point == NULL || state.best_nd2 > 1.0) {
    return NULL;
  }

  if (distance_out != NULL) {
    *distance_out = sqrt(state.best_euclid_d2);
  }
  return state.best_point;
}

static uint8_t output_alpha_for_point(const SlicePoint *point) {
  const double source_alpha = point->has_alpha ? (double) point->a : 1.0;
  double alpha = 255.0 - source_alpha * 0.5;
  if (alpha > 200.0) {
    alpha = 200.0;
  }
  if (alpha < 0.0) {
    alpha = 0.0;
  }
  return (uint8_t) alpha;
}
