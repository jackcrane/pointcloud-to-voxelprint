#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
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

enum {
  SIZE_X_IN = 10,
  SIZE_Y_IN = 10,
  SIZE_Z_IN = 4,
  DPI_X = 300,
  DPI_Y = 300,
  DPI_Z = 300,
  SHARD_COUNT = 128,
  OUTPUT_RECORD_BYTES = 16,
  SHARD_BUFFER_BYTES = 1024 * 1024,
  STREAM_CHUNK_BYTES = 1024 * 1024,
};

static const uint64_t ESTIMATE_POINT_COUNT = 1239896640ULL;
static const uint64_t DEFAULT_LOG_INTERVAL = 10000000ULL;
static const size_t SHARD_TEXT_RECORD_ESTIMATE = 96;

typedef enum {
  PLY_FORMAT_ASCII = 0,
  PLY_FORMAT_BINARY_LE = 1,
} PlyFormat;

typedef enum {
  PLY_TYPE_FLOAT32 = 0,
  PLY_TYPE_FLOAT64 = 1,
  PLY_TYPE_U8 = 2,
  PLY_TYPE_I8 = 3,
  PLY_TYPE_U16 = 4,
  PLY_TYPE_I16 = 5,
  PLY_TYPE_U32 = 6,
  PLY_TYPE_I32 = 7,
  PLY_TYPE_UNKNOWN = 8,
} PlyType;

typedef struct {
  char *name;
  PlyType type;
} PlyProperty;

typedef struct {
  PlyFormat format;
  uint64_t vertex_count;
  PlyProperty *properties;
  size_t property_count;
  off_t header_end_offset;
} PlyHeader;

typedef struct {
  int x;
  int y;
  int z;
  int r;
  int g;
  int b;
  int a;
  PlyType r_type;
  PlyType g_type;
  PlyType b_type;
  PlyType a_type;
} AsciiFieldIndices;

typedef struct {
  bool present;
  size_t offset;
  PlyType type;
} BinaryField;

typedef struct {
  size_t stride;
  BinaryField x;
  BinaryField y;
  BinaryField z;
  BinaryField r;
  BinaryField g;
  BinaryField b;
  BinaryField a;
} BinaryLayout;

typedef struct {
  double x;
  double y;
  double z;
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t a;
} Vertex;

typedef struct {
  double min_x;
  double min_y;
  double min_z;
  double max_x;
  double max_y;
  double max_z;
} Bounds;

typedef struct {
  uint32_t x;
  uint32_t y;
  uint32_t z;
} Grid;

typedef struct {
  Grid grid;
  double min_x;
  double min_y;
  double min_z;
  double range_x;
  double range_y;
  double range_z;
  uint64_t grid_x_u64;
  uint64_t grid_y_u64;
} Scaler;

typedef struct {
  FILE *fp;
  unsigned char *buffer;
  size_t capacity;
  size_t used;
} BufferedWriter;

typedef struct {
  uint64_t cell_id;
  uint32_t color;
} ShardRecord;

typedef struct {
  Bounds *bounds;
} BoundsScanContext;

typedef struct {
  const Scaler *scaler;
  BufferedWriter *writers;
  uint64_t sharded_point_count;
} ShardPassContext;

typedef struct {
  const char *input_path;
  const char *output_path;
  char *temp_dir;
  char *output_data_path;
  char *shard_paths[SHARD_COUNT];
  BufferedWriter shard_writers[SHARD_COUNT];
  bool shard_writers_open;
} TempPaths;

typedef struct {
  const char *label;
  uint64_t total;
  uint64_t interval;
  uint64_t next_log;
  double started_at;
} ProgressLogger;

typedef int (*VertexVisitor)(const Vertex *vertex, void *ctx);

static void free_ply_header(PlyHeader *header);
static int read_ply_header(const char *file_path, PlyHeader *header_out);
static int for_each_vertex(
    const char *file_path,
    const PlyHeader *header,
    VertexVisitor visitor,
    void *ctx,
    ProgressLogger *progress,
    uint64_t *processed_out);
static int stream_ascii_vertices(
    const char *file_path,
    const PlyHeader *header,
    VertexVisitor visitor,
    void *ctx,
    ProgressLogger *progress,
    uint64_t *processed_out);
static int stream_binary_vertices(
    const char *file_path,
    const PlyHeader *header,
    VertexVisitor visitor,
    void *ctx,
    ProgressLogger *progress,
    uint64_t *processed_out);
static int write_empty_ply(const char *output_path);
static int write_final_ply(
    const char *vertex_data_path,
    const char *output_path,
    uint64_t point_count,
    ProgressLogger *progress);
static int reduce_shard(
    const char *shard_path,
    const Scaler *scaler,
    BufferedWriter *output_writer,
    ProgressLogger *progress,
    uint64_t *output_point_count);
static int ensure_parent_dir(const char *path);
static void cleanup_temp_paths(TempPaths *temp_paths);
static int parse_shard_record_line(const char *line, uint64_t *cell_id_out, uint32_t *color_out);
static void format_ascii_float(double value, char *buffer, size_t buffer_size);

static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static const char *format_duration(double total_seconds, char *buffer, size_t buffer_size);

static void progress_logger_init(
    ProgressLogger *progress,
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
  progress->started_at = started_at;
}

static void progress_logger_maybe_log(ProgressLogger *progress, uint64_t processed) {
  if (progress == NULL || progress->interval == 0 || processed < progress->next_log) {
    return;
  }

  char elapsed[64];
  double elapsed_seconds = now_seconds() - progress->started_at;

  if (progress->total > 0) {
    double percent = ((double)processed * 100.0) / (double)progress->total;
    char eta[64];
    const char *eta_text = "n/a";
    if (processed > 0 && processed < progress->total) {
      double seconds_per_record = elapsed_seconds / (double)processed;
      double eta_seconds = seconds_per_record * (double)(progress->total - processed);
      eta_text = format_duration(eta_seconds, eta, sizeof(eta));
    } else if (processed >= progress->total) {
      eta_text = format_duration(0.0, eta, sizeof(eta));
    }
    printf(
        "%s progress: %" PRIu64 "/%" PRIu64 " (%.2f%%, elapsed %s, estimated remaining %s)\n",
        progress->label,
        processed,
        progress->total,
        percent,
        format_duration(elapsed_seconds, elapsed, sizeof(elapsed)),
        eta_text);
  } else {
    printf(
        "%s progress: %" PRIu64 " records (elapsed %s)\n",
        progress->label,
        processed,
        format_duration(elapsed_seconds, elapsed, sizeof(elapsed)));
  }

  fflush(stdout);

  while (progress->next_log <= processed) {
    if (UINT64_MAX - progress->next_log < progress->interval) {
      progress->next_log = UINT64_MAX;
      break;
    }
    progress->next_log += progress->interval;
  }
}

static char *trim_in_place(char *text) {
  while (*text != '\0' && isspace((unsigned char)*text)) {
    text++;
  }

  char *end = text + strlen(text);
  while (end > text && isspace((unsigned char)end[-1])) {
    end--;
  }
  *end = '\0';
  return text;
}

static bool starts_with(const char *text, const char *prefix) {
  return strncmp(text, prefix, strlen(prefix)) == 0;
}

static char *dup_path_join(const char *dir, const char *name) {
  size_t dir_len = strlen(dir);
  size_t name_len = strlen(name);
  bool need_slash = dir_len > 0 && dir[dir_len - 1] != '/';
  size_t total = dir_len + (need_slash ? 1 : 0) + name_len + 1;
  char *out = malloc(total);
  if (out == NULL) {
    return NULL;
  }

  memcpy(out, dir, dir_len);
  if (need_slash) {
    out[dir_len] = '/';
    memcpy(out + dir_len + 1, name, name_len);
    out[dir_len + 1 + name_len] = '\0';
  } else {
    memcpy(out + dir_len, name, name_len);
    out[dir_len + name_len] = '\0';
  }

  return out;
}

static int mkdir_p(const char *dir_path) {
  if (dir_path == NULL || dir_path[0] == '\0') {
    return 0;
  }

  char *copy = strdup(dir_path);
  if (copy == NULL) {
    fprintf(stderr, "Failed to allocate memory for path.\n");
    return -1;
  }

  size_t len = strlen(copy);
  if (len > 1 && copy[len - 1] == '/') {
    copy[len - 1] = '\0';
  }

  for (char *p = copy + 1; *p != '\0'; ++p) {
    if (*p != '/') {
      continue;
    }

    *p = '\0';
    if (mkdir(copy, 0777) != 0 && errno != EEXIST) {
      fprintf(stderr, "Failed to create directory '%s': %s\n", copy, strerror(errno));
      free(copy);
      return -1;
    }
    *p = '/';
  }

  if (mkdir(copy, 0777) != 0 && errno != EEXIST) {
    fprintf(stderr, "Failed to create directory '%s': %s\n", copy, strerror(errno));
    free(copy);
    return -1;
  }

  free(copy);
  return 0;
}

static int ensure_parent_dir(const char *path) {
  char *copy = strdup(path);
  if (copy == NULL) {
    fprintf(stderr, "Failed to allocate memory for parent path.\n");
    return -1;
  }

  char *slash = strrchr(copy, '/');
  if (slash == NULL) {
    free(copy);
    return 0;
  }
  if (slash == copy) {
    free(copy);
    return 0;
  }

  *slash = '\0';
  int rc = mkdir_p(copy);
  free(copy);
  return rc;
}

static PlyType parse_ply_type(const char *name) {
  if (strcasecmp(name, "float") == 0 || strcasecmp(name, "float32") == 0) {
    return PLY_TYPE_FLOAT32;
  }
  if (strcasecmp(name, "double") == 0 || strcasecmp(name, "float64") == 0) {
    return PLY_TYPE_FLOAT64;
  }
  if (strcasecmp(name, "uchar") == 0 || strcasecmp(name, "uint8") == 0) {
    return PLY_TYPE_U8;
  }
  if (strcasecmp(name, "char") == 0 || strcasecmp(name, "int8") == 0) {
    return PLY_TYPE_I8;
  }
  if (strcasecmp(name, "ushort") == 0 || strcasecmp(name, "uint16") == 0) {
    return PLY_TYPE_U16;
  }
  if (strcasecmp(name, "short") == 0 || strcasecmp(name, "int16") == 0) {
    return PLY_TYPE_I16;
  }
  if (strcasecmp(name, "uint") == 0 || strcasecmp(name, "uint32") == 0) {
    return PLY_TYPE_U32;
  }
  if (strcasecmp(name, "int") == 0 || strcasecmp(name, "int32") == 0) {
    return PLY_TYPE_I32;
  }
  return PLY_TYPE_UNKNOWN;
}

static size_t ply_type_size(PlyType type) {
  switch (type) {
    case PLY_TYPE_FLOAT32:
    case PLY_TYPE_U32:
    case PLY_TYPE_I32:
    case PLY_TYPE_UNKNOWN:
      return 4;
    case PLY_TYPE_FLOAT64:
      return 8;
    case PLY_TYPE_U16:
    case PLY_TYPE_I16:
      return 2;
    case PLY_TYPE_U8:
    case PLY_TYPE_I8:
      return 1;
  }
  return 4;
}

static bool ply_type_is_normalized_float(PlyType type) {
  return type == PLY_TYPE_FLOAT32 || type == PLY_TYPE_FLOAT64;
}

static bool ply_type_has_unsigned_max(PlyType type, double *max_out) {
  switch (type) {
    case PLY_TYPE_U8:
      *max_out = 255.0;
      return true;
    case PLY_TYPE_U16:
      *max_out = 65535.0;
      return true;
    case PLY_TYPE_U32:
      *max_out = 4294967295.0;
      return true;
    default:
      return false;
  }
}

static int normalize_color_value(double value, PlyType type) {
  if (!isfinite(value)) {
    return -1;
  }

  if (type != PLY_TYPE_UNKNOWN) {
    double unsigned_max = 0.0;
    if (ply_type_is_normalized_float(type)) {
      if (value >= 0.0 && value <= 1.0) {
        return (int)llround(value * 255.0);
      }
      long long rounded = llround(value);
      if (rounded < 0) {
        return 0;
      }
      if (rounded > 255) {
        return 255;
      }
      return (int)rounded;
    }

    if (ply_type_has_unsigned_max(type, &unsigned_max)) {
      double clamped = value;
      if (clamped < 0.0) {
        clamped = 0.0;
      }
      if (clamped > unsigned_max) {
        clamped = unsigned_max;
      }
      if (unsigned_max <= 255.0) {
        return (int)llround(clamped);
      }
      return (int)llround((clamped * 255.0) / unsigned_max);
    }
  }

  if (value >= 0.0 && value <= 1.0) {
    return (int)llround(value * 255.0);
  }

  long long rounded = llround(value);
  if (rounded < 0) {
    return 0;
  }
  if (rounded > 255) {
    return 255;
  }
  return (int)rounded;
}

static void init_bounds(Bounds *bounds) {
  bounds->min_x = INFINITY;
  bounds->min_y = INFINITY;
  bounds->min_z = INFINITY;
  bounds->max_x = -INFINITY;
  bounds->max_y = -INFINITY;
  bounds->max_z = -INFINITY;
}

static int find_property_index(
    const PlyHeader *header,
    const char *primary_name,
    const char *fallback_name,
    PlyType *type_out) {
  for (size_t i = 0; i < header->property_count; ++i) {
    const char *name = header->properties[i].name;
    if (strcmp(name, primary_name) == 0 ||
        (fallback_name != NULL && strcmp(name, fallback_name) == 0)) {
      if (type_out != NULL) {
        *type_out = header->properties[i].type;
      }
      return (int)i;
    }
  }
  if (type_out != NULL) {
    *type_out = PLY_TYPE_UNKNOWN;
  }
  return -1;
}

static AsciiFieldIndices build_ascii_indices(const PlyHeader *header) {
  AsciiFieldIndices indices;
  indices.x = find_property_index(header, "x", NULL, NULL);
  indices.y = find_property_index(header, "y", NULL, NULL);
  indices.z = find_property_index(header, "z", NULL, NULL);
  indices.r = find_property_index(header, "red", "r", &indices.r_type);
  indices.g = find_property_index(header, "green", "g", &indices.g_type);
  indices.b = find_property_index(header, "blue", "b", &indices.b_type);
  indices.a = find_property_index(header, "alpha", "a", &indices.a_type);
  return indices;
}

static BinaryLayout build_binary_layout(const PlyHeader *header) {
  BinaryLayout layout;
  memset(&layout, 0, sizeof(layout));

  size_t offset = 0;
  for (size_t i = 0; i < header->property_count; ++i) {
    const PlyProperty *property = &header->properties[i];
    BinaryField field = {true, offset, property->type};

    if (strcmp(property->name, "x") == 0) {
      layout.x = field;
    } else if (strcmp(property->name, "y") == 0) {
      layout.y = field;
    } else if (strcmp(property->name, "z") == 0) {
      layout.z = field;
    } else if (strcmp(property->name, "red") == 0 || strcmp(property->name, "r") == 0) {
      layout.r = field;
    } else if (strcmp(property->name, "green") == 0 || strcmp(property->name, "g") == 0) {
      layout.g = field;
    } else if (strcmp(property->name, "blue") == 0 || strcmp(property->name, "b") == 0) {
      layout.b = field;
    } else if (strcmp(property->name, "alpha") == 0 || strcmp(property->name, "a") == 0) {
      layout.a = field;
    }

    offset += ply_type_size(property->type);
  }

  layout.stride = offset;
  return layout;
}

static uint16_t read_u16_le(const unsigned char *ptr) {
  return (uint16_t)ptr[0] | ((uint16_t)ptr[1] << 8);
}

static int16_t read_i16_le(const unsigned char *ptr) {
  return (int16_t)read_u16_le(ptr);
}

static uint32_t read_u32_le(const unsigned char *ptr) {
  return (uint32_t)ptr[0] |
      ((uint32_t)ptr[1] << 8) |
      ((uint32_t)ptr[2] << 16) |
      ((uint32_t)ptr[3] << 24);
}

static int32_t read_i32_le(const unsigned char *ptr) {
  return (int32_t)read_u32_le(ptr);
}

static uint64_t read_u64_le(const unsigned char *ptr) {
  return (uint64_t)ptr[0] |
      ((uint64_t)ptr[1] << 8) |
      ((uint64_t)ptr[2] << 16) |
      ((uint64_t)ptr[3] << 24) |
      ((uint64_t)ptr[4] << 32) |
      ((uint64_t)ptr[5] << 40) |
      ((uint64_t)ptr[6] << 48) |
      ((uint64_t)ptr[7] << 56);
}

static void write_u32_le(unsigned char *ptr, uint32_t value) {
  ptr[0] = (unsigned char)(value & 0xffu);
  ptr[1] = (unsigned char)((value >> 8) & 0xffu);
  ptr[2] = (unsigned char)((value >> 16) & 0xffu);
  ptr[3] = (unsigned char)((value >> 24) & 0xffu);
}

static double read_binary_value(const unsigned char *ptr, PlyType type) {
  switch (type) {
    case PLY_TYPE_FLOAT32: {
      union {
        uint32_t u;
        float f;
      } value;
      value.u = read_u32_le(ptr);
      return (double)value.f;
    }
    case PLY_TYPE_FLOAT64: {
      union {
        uint64_t u;
        double d;
      } value;
      value.u = read_u64_le(ptr);
      return value.d;
    }
    case PLY_TYPE_U8:
      return (double)ptr[0];
    case PLY_TYPE_I8:
      return (double)(int8_t)ptr[0];
    case PLY_TYPE_U16:
      return (double)read_u16_le(ptr);
    case PLY_TYPE_I16:
      return (double)read_i16_le(ptr);
    case PLY_TYPE_U32:
      return (double)read_u32_le(ptr);
    case PLY_TYPE_I32:
      return (double)read_i32_le(ptr);
    case PLY_TYPE_UNKNOWN:
    default: {
      union {
        uint32_t u;
        float f;
      } value;
      value.u = read_u32_le(ptr);
      return (double)value.f;
    }
  }
}

static bool parse_uint64_str(const char *text, uint64_t *value_out) {
  errno = 0;
  char *end = NULL;
  unsigned long long value = strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') {
    return false;
  }
  *value_out = (uint64_t)value;
  return true;
}

static int append_property(PlyHeader *header, const char *name, PlyType type) {
  PlyProperty *next = realloc(
      header->properties,
      (header->property_count + 1) * sizeof(*header->properties));
  if (next == NULL) {
    fprintf(stderr, "Failed to allocate PLY property list.\n");
    return -1;
  }

  header->properties = next;
  header->properties[header->property_count].name = strdup(name);
  header->properties[header->property_count].type = type;
  if (header->properties[header->property_count].name == NULL) {
    fprintf(stderr, "Failed to allocate PLY property name.\n");
    return -1;
  }

  header->property_count++;
  return 0;
}

static int read_ply_header(const char *file_path, PlyHeader *header_out) {
  memset(header_out, 0, sizeof(*header_out));

  FILE *fp = fopen(file_path, "rb");
  if (fp == NULL) {
    fprintf(stderr, "Failed to open '%s': %s\n", file_path, strerror(errno));
    return -1;
  }

  bool saw_ply = false;
  bool saw_format = false;
  bool in_vertex_element = false;
  char *line = NULL;
  size_t line_capacity = 0;
  int rc = -1;

  while (getline(&line, &line_capacity, fp) != -1) {
    header_out->header_end_offset = ftello(fp);
    char *trimmed = trim_in_place(line);

    if (!saw_ply) {
      if (strcasecmp(trimmed, "ply") != 0) {
        fprintf(stderr, "Not a PLY file.\n");
        goto cleanup;
      }
      saw_ply = true;
      continue;
    }

    if (strcmp(trimmed, "end_header") == 0) {
      rc = 0;
      goto cleanup;
    }

    if (starts_with(trimmed, "comment ") || starts_with(trimmed, "obj_info ")) {
      continue;
    }

    if (starts_with(trimmed, "format ")) {
      char format_name[64];
      char format_version[64];
      if (sscanf(trimmed, "format %63s %63s", format_name, format_version) != 2) {
        fprintf(stderr, "Invalid PLY format line.\n");
        goto cleanup;
      }

      saw_format = true;
      if (strcmp(format_name, "ascii") == 0) {
        header_out->format = PLY_FORMAT_ASCII;
      } else if (strcmp(format_name, "binary_little_endian") == 0) {
        header_out->format = PLY_FORMAT_BINARY_LE;
      } else {
        fprintf(stderr, "Unsupported PLY format: %s %s\n", format_name, format_version);
        goto cleanup;
      }
      continue;
    }

    if (starts_with(trimmed, "element ")) {
      char element_name[64];
      char count_text[64];
      if (sscanf(trimmed, "element %63s %63s", element_name, count_text) != 2) {
        fprintf(stderr, "Invalid PLY element line.\n");
        goto cleanup;
      }

      in_vertex_element = strcmp(element_name, "vertex") == 0;
      if (in_vertex_element) {
        if (!parse_uint64_str(count_text, &header_out->vertex_count)) {
          fprintf(stderr, "Invalid vertex count in PLY header.\n");
          goto cleanup;
        }
      }
      continue;
    }

    if (in_vertex_element && starts_with(trimmed, "property ")) {
      char kind[64];
      char type_name[64];
      char prop_name[64];

      if (sscanf(trimmed, "property %63s %63s %63s", kind, type_name, prop_name) == 3 &&
          strcmp(kind, "list") == 0) {
        fprintf(stderr, "Unsupported vertex list property in PLY header.\n");
        goto cleanup;
      }

      if (sscanf(trimmed, "property %63s %63s", type_name, prop_name) != 2) {
        fprintf(stderr, "Invalid PLY property line.\n");
        goto cleanup;
      }

      if (append_property(header_out, prop_name, parse_ply_type(type_name)) != 0) {
        goto cleanup;
      }
      continue;
    }
  }

  fprintf(stderr, "Invalid PLY: missing end_header.\n");

cleanup:
  if (!saw_format && rc == 0) {
    fprintf(stderr, "Missing PLY format line.\n");
    rc = -1;
  }

  free(line);
  fclose(fp);

  if (rc != 0) {
    free_ply_header(header_out);
  }
  return rc;
}

static int parse_ascii_vertex_line(
    char *line,
    const PlyHeader *header,
    const AsciiFieldIndices *indices,
    Vertex *vertex_out) {
  vertex_out->x = NAN;
  vertex_out->y = NAN;
  vertex_out->z = NAN;
  vertex_out->r = 255;
  vertex_out->g = 255;
  vertex_out->b = 255;
  vertex_out->a = 255;

  size_t field_index = 0;
  char *cursor = line;
  while (*cursor != '\0' && field_index < header->property_count) {
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
      cursor++;
    }
    if (*cursor == '\0') {
      break;
    }

    errno = 0;
    char *end = NULL;
    double value = strtod(cursor, &end);
    if (end == cursor || errno == ERANGE) {
      value = NAN;
      while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
        cursor++;
      }
    } else {
      cursor = end;
    }

    if ((int)field_index == indices->x) {
      vertex_out->x = value;
    } else if ((int)field_index == indices->y) {
      vertex_out->y = value;
    } else if ((int)field_index == indices->z) {
      vertex_out->z = value;
    } else if ((int)field_index == indices->r) {
      int normalized = normalize_color_value(value, indices->r_type);
      if (normalized >= 0) {
        vertex_out->r = (uint8_t)normalized;
      }
    } else if ((int)field_index == indices->g) {
      int normalized = normalize_color_value(value, indices->g_type);
      if (normalized >= 0) {
        vertex_out->g = (uint8_t)normalized;
      }
    } else if ((int)field_index == indices->b) {
      int normalized = normalize_color_value(value, indices->b_type);
      if (normalized >= 0) {
        vertex_out->b = (uint8_t)normalized;
      }
    } else if ((int)field_index == indices->a) {
      int normalized = normalize_color_value(value, indices->a_type);
      if (normalized >= 0) {
        vertex_out->a = (uint8_t)normalized;
      }
    }

    field_index++;
  }

  return 0;
}

static int stream_ascii_vertices(
    const char *file_path,
    const PlyHeader *header,
    VertexVisitor visitor,
    void *ctx,
    ProgressLogger *progress,
    uint64_t *processed_out) {
  AsciiFieldIndices indices = build_ascii_indices(header);
  if (indices.x < 0 || indices.y < 0 || indices.z < 0) {
    fprintf(stderr, "PLY vertex must include x, y, and z.\n");
    return -1;
  }

  FILE *fp = fopen(file_path, "rb");
  if (fp == NULL) {
    fprintf(stderr, "Failed to open '%s': %s\n", file_path, strerror(errno));
    return -1;
  }
  if (fseeko(fp, header->header_end_offset, SEEK_SET) != 0) {
    fprintf(stderr, "Failed to seek PLY payload: %s\n", strerror(errno));
    fclose(fp);
    return -1;
  }

  char *line = NULL;
  size_t line_capacity = 0;
  uint64_t processed = 0;
  int rc = 0;

  while (processed < header->vertex_count && getline(&line, &line_capacity, fp) != -1) {
    char *trimmed = trim_in_place(line);
    if (trimmed[0] == '\0' || starts_with(trimmed, "comment")) {
      continue;
    }

    Vertex vertex;
    if (parse_ascii_vertex_line(trimmed, header, &indices, &vertex) != 0) {
      rc = -1;
      break;
    }
    if (visitor(&vertex, ctx) != 0) {
      rc = -1;
      break;
    }
    processed++;
    progress_logger_maybe_log(progress, processed);
  }

  free(line);
  fclose(fp);
  *processed_out = processed;
  return rc;
}

static int stream_binary_vertices(
    const char *file_path,
    const PlyHeader *header,
    VertexVisitor visitor,
    void *ctx,
    ProgressLogger *progress,
    uint64_t *processed_out) {
  BinaryLayout layout = build_binary_layout(header);
  if (!layout.x.present || !layout.y.present || !layout.z.present) {
    fprintf(stderr, "PLY vertex must include x, y, and z.\n");
    return -1;
  }
  if (layout.stride == 0) {
    fprintf(stderr, "Invalid binary PLY vertex layout.\n");
    return -1;
  }

  FILE *fp = fopen(file_path, "rb");
  if (fp == NULL) {
    fprintf(stderr, "Failed to open '%s': %s\n", file_path, strerror(errno));
    return -1;
  }
  if (fseeko(fp, header->header_end_offset, SEEK_SET) != 0) {
    fprintf(stderr, "Failed to seek PLY payload: %s\n", strerror(errno));
    fclose(fp);
    return -1;
  }

  unsigned char *buffer = malloc(STREAM_CHUNK_BYTES + layout.stride);
  if (buffer == NULL) {
    fprintf(stderr, "Failed to allocate binary stream buffer.\n");
    fclose(fp);
    return -1;
  }

  uint64_t processed = 0;
  size_t leftover = 0;
  int rc = 0;

  while (processed < header->vertex_count) {
    size_t bytes_read = fread(buffer + leftover, 1, STREAM_CHUNK_BYTES, fp);
    if (bytes_read == 0) {
      if (ferror(fp)) {
        fprintf(stderr, "Failed to read binary PLY payload.\n");
        rc = -1;
      }
      break;
    }

    size_t available = leftover + bytes_read;
    size_t complete_bytes = available - (available % layout.stride);

    for (size_t offset = 0;
         offset < complete_bytes && processed < header->vertex_count;
         offset += layout.stride) {
      const unsigned char *record = buffer + offset;
      Vertex vertex;
      vertex.x = read_binary_value(record + layout.x.offset, layout.x.type);
      vertex.y = read_binary_value(record + layout.y.offset, layout.y.type);
      vertex.z = read_binary_value(record + layout.z.offset, layout.z.type);
      vertex.r = layout.r.present
          ? (uint8_t)normalize_color_value(
                read_binary_value(record + layout.r.offset, layout.r.type),
                layout.r.type)
          : 255;
      vertex.g = layout.g.present
          ? (uint8_t)normalize_color_value(
                read_binary_value(record + layout.g.offset, layout.g.type),
                layout.g.type)
          : 255;
      vertex.b = layout.b.present
          ? (uint8_t)normalize_color_value(
                read_binary_value(record + layout.b.offset, layout.b.type),
                layout.b.type)
          : 255;
      vertex.a = layout.a.present
          ? (uint8_t)normalize_color_value(
                read_binary_value(record + layout.a.offset, layout.a.type),
                layout.a.type)
          : 255;

      if (visitor(&vertex, ctx) != 0) {
        rc = -1;
        break;
      }
      processed++;
      progress_logger_maybe_log(progress, processed);
    }

    if (rc != 0) {
      break;
    }

    leftover = available - complete_bytes;
    if (leftover > 0) {
      memmove(buffer, buffer + complete_bytes, leftover);
    }
  }

  if (rc == 0 && processed != header->vertex_count) {
    fprintf(
        stderr,
        "PLY binary parse error: expected %" PRIu64 " vertices, got %" PRIu64 "\n",
        header->vertex_count,
        processed);
    rc = -1;
  }

  free(buffer);
  fclose(fp);
  *processed_out = processed;
  return rc;
}

static int for_each_vertex(
    const char *file_path,
    const PlyHeader *header,
    VertexVisitor visitor,
    void *ctx,
    ProgressLogger *progress,
    uint64_t *processed_out) {
  if (header->format == PLY_FORMAT_ASCII) {
    return stream_ascii_vertices(file_path, header, visitor, ctx, progress, processed_out);
  }
  return stream_binary_vertices(file_path, header, visitor, ctx, progress, processed_out);
}

static uint32_t pack_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}

static void unpack_color(uint32_t packed, uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a) {
  *r = (uint8_t)(packed & 0xffu);
  *g = (uint8_t)((packed >> 8) & 0xffu);
  *b = (uint8_t)((packed >> 16) & 0xffu);
  *a = (uint8_t)((packed >> 24) & 0xffu);
}

static Scaler build_scaler(const Bounds *bounds, Grid grid) {
  Scaler scaler;
  scaler.grid = grid;
  scaler.min_x = bounds->min_x;
  scaler.min_y = bounds->min_y;
  scaler.min_z = bounds->min_z;
  scaler.range_x = bounds->max_x - bounds->min_x;
  scaler.range_y = bounds->max_y - bounds->min_y;
  scaler.range_z = bounds->max_z - bounds->min_z;
  scaler.grid_x_u64 = (uint64_t)grid.x;
  scaler.grid_y_u64 = (uint64_t)grid.y;
  return scaler;
}

static uint32_t quantize_axis(double value, double min_value, double range, uint32_t steps) {
  double ratio = range > 0.0 ? (value - min_value) / range : 0.5;
  long long index = (long long)floor(ratio * (double)steps);
  if (index < 0) {
    return 0;
  }
  if ((uint64_t)index >= (uint64_t)steps) {
    return steps - 1;
  }
  return (uint32_t)index;
}

static uint64_t quantize_cell_id(const Scaler *scaler, const Vertex *vertex) {
  uint32_t ix = quantize_axis(vertex->x, scaler->min_x, scaler->range_x, scaler->grid.x);
  uint32_t iy = quantize_axis(vertex->y, scaler->min_y, scaler->range_y, scaler->grid.y);
  uint32_t iz = quantize_axis(vertex->z, scaler->min_z, scaler->range_z, scaler->grid.z);
  return (uint64_t)ix +
      scaler->grid_x_u64 *
          ((uint64_t)iy + scaler->grid_y_u64 * (uint64_t)iz);
}

static void decode_cell_center(const Scaler *scaler, uint64_t cell_id, float *x, float *y, float *z) {
  uint64_t x_index = cell_id % scaler->grid_x_u64;
  uint64_t yz = cell_id / scaler->grid_x_u64;
  uint64_t y_index = yz % scaler->grid_y_u64;
  uint64_t z_index = yz / scaler->grid_y_u64;

  *x = (float)(((double)x_index + 0.5) / (double)scaler->grid.x * (double)SIZE_X_IN);
  *y = (float)(((double)y_index + 0.5) / (double)scaler->grid.y * (double)SIZE_Y_IN);
  *z = (float)(((double)z_index + 0.5) / (double)scaler->grid.z * (double)SIZE_Z_IN);
}

static int buffered_writer_open(BufferedWriter *writer, const char *path, size_t record_bytes) {
  memset(writer, 0, sizeof(*writer));
  writer->fp = fopen(path, "wb");
  if (writer->fp == NULL) {
    fprintf(stderr, "Failed to open '%s' for writing: %s\n", path, strerror(errno));
    return -1;
  }

  size_t capacity = (SHARD_BUFFER_BYTES / record_bytes) * record_bytes;
  if (capacity < record_bytes) {
    capacity = record_bytes;
  }

  writer->buffer = malloc(capacity);
  if (writer->buffer == NULL) {
    fprintf(stderr, "Failed to allocate write buffer for '%s'.\n", path);
    fclose(writer->fp);
    writer->fp = NULL;
    return -1;
  }

  writer->capacity = capacity;
  writer->used = 0;
  return 0;
}

static int buffered_writer_flush(BufferedWriter *writer) {
  if (writer->used == 0) {
    return 0;
  }

  if (fwrite(writer->buffer, 1, writer->used, writer->fp) != writer->used) {
    fprintf(stderr, "Failed to flush buffered writer: %s\n", strerror(errno));
    return -1;
  }
  writer->used = 0;
  return 0;
}

static int buffered_writer_append_shard_record(
    BufferedWriter *writer,
    uint64_t cell_id,
    float x,
    float y,
    float z,
    uint8_t r,
    uint8_t g,
    uint8_t b,
    uint8_t a) {
  char line[128];
  char x_text[32];
  char y_text[32];
  char z_text[32];
  uint32_t packed_color = pack_color(r, g, b, a);

  format_ascii_float(x, x_text, sizeof(x_text));
  format_ascii_float(y, y_text, sizeof(y_text));
  format_ascii_float(z, z_text, sizeof(z_text));

  int line_len = snprintf(
      line,
      sizeof(line),
      "%" PRIu64 " %s %s %s %u %u %u %u %" PRIu32 "\n",
      cell_id,
      x_text,
      y_text,
      z_text,
      (unsigned)r,
      (unsigned)g,
      (unsigned)b,
      (unsigned)a,
      packed_color);
  if (line_len < 0 || (size_t)line_len >= sizeof(line)) {
    fprintf(stderr, "Failed to format shard record.\n");
    return -1;
  }

  size_t bytes_needed = (size_t)line_len;
  if (writer->used + bytes_needed > writer->capacity) {
    if (buffered_writer_flush(writer) != 0) {
      return -1;
    }
  }

  if (bytes_needed > writer->capacity) {
    if (fwrite(line, 1, bytes_needed, writer->fp) != bytes_needed) {
      fprintf(stderr, "Failed to write shard record: %s\n", strerror(errno));
      return -1;
    }
    return 0;
  }

  memcpy(writer->buffer + writer->used, line, bytes_needed);
  writer->used += bytes_needed;
  return 0;
}

static int buffered_writer_append_output(
    BufferedWriter *writer,
    float x,
    float y,
    float z,
    uint8_t r,
    uint8_t g,
    uint8_t b,
    uint8_t a) {
  if (writer->used + OUTPUT_RECORD_BYTES > writer->capacity) {
    if (buffered_writer_flush(writer) != 0) {
      return -1;
    }
  }

  unsigned char *slot = writer->buffer + writer->used;
  union {
    float f;
    uint32_t u;
  } fx, fy, fz;
  fx.f = x;
  fy.f = y;
  fz.f = z;
  write_u32_le(slot, fx.u);
  write_u32_le(slot + 4, fy.u);
  write_u32_le(slot + 8, fz.u);
  slot[12] = r;
  slot[13] = g;
  slot[14] = b;
  slot[15] = a;
  writer->used += OUTPUT_RECORD_BYTES;
  return 0;
}

static int buffered_writer_close(BufferedWriter *writer) {
  int rc = 0;
  if (writer->fp != NULL) {
    if (buffered_writer_flush(writer) != 0) {
      rc = -1;
    }
    if (fclose(writer->fp) != 0) {
      fprintf(stderr, "Failed to close buffered writer: %s\n", strerror(errno));
      rc = -1;
    }
  }

  free(writer->buffer);
  writer->buffer = NULL;
  writer->fp = NULL;
  writer->capacity = 0;
  writer->used = 0;
  return rc;
}

static int bounds_scan_visitor(const Vertex *vertex, void *ctx) {
  BoundsScanContext *bounds_ctx = ctx;
  Bounds *bounds = bounds_ctx->bounds;

  if (!isfinite(vertex->x) || !isfinite(vertex->y) || !isfinite(vertex->z)) {
    return 0;
  }

  if (vertex->x < bounds->min_x) {
    bounds->min_x = vertex->x;
  }
  if (vertex->y < bounds->min_y) {
    bounds->min_y = vertex->y;
  }
  if (vertex->z < bounds->min_z) {
    bounds->min_z = vertex->z;
  }
  if (vertex->x > bounds->max_x) {
    bounds->max_x = vertex->x;
  }
  if (vertex->y > bounds->max_y) {
    bounds->max_y = vertex->y;
  }
  if (vertex->z > bounds->max_z) {
    bounds->max_z = vertex->z;
  }

  return 0;
}

static int shard_pass_visitor(const Vertex *vertex, void *ctx) {
  ShardPassContext *shard_ctx = ctx;
  if (!isfinite(vertex->x) || !isfinite(vertex->y) || !isfinite(vertex->z)) {
    return 0;
  }

  uint64_t cell_id = quantize_cell_id(shard_ctx->scaler, vertex);
  int shard_index = (int)(cell_id % SHARD_COUNT);
  float x, y, z;
  decode_cell_center(shard_ctx->scaler, cell_id, &x, &y, &z);
  if (buffered_writer_append_shard_record(
          &shard_ctx->writers[shard_index],
          cell_id,
          x,
          y,
          z,
          vertex->r,
          vertex->g,
          vertex->b,
          vertex->a) != 0) {
    return -1;
  }

  shard_ctx->sharded_point_count++;
  return 0;
}

static int compare_shard_records(const void *lhs_ptr, const void *rhs_ptr) {
  const ShardRecord *lhs = lhs_ptr;
  const ShardRecord *rhs = rhs_ptr;

  if (lhs->cell_id < rhs->cell_id) {
    return -1;
  }
  if (lhs->cell_id > rhs->cell_id) {
    return 1;
  }
  if (lhs->color < rhs->color) {
    return -1;
  }
  if (lhs->color > rhs->color) {
    return 1;
  }
  return 0;
}

static int reduce_shard(
    const char *shard_path,
    const Scaler *scaler,
    BufferedWriter *output_writer,
    ProgressLogger *progress,
    uint64_t *output_point_count) {
  struct stat stat_buf;
  if (stat(shard_path, &stat_buf) != 0) {
    fprintf(stderr, "Failed to stat shard '%s': %s\n", shard_path, strerror(errno));
    return -1;
  }

  if (stat_buf.st_size == 0) {
    return 0;
  }

  FILE *fp = fopen(shard_path, "rb");
  if (fp == NULL) {
    fprintf(stderr, "Failed to open shard '%s': %s\n", shard_path, strerror(errno));
    return -1;
  }

  size_t capacity = (size_t)(stat_buf.st_size / 24);
  if (capacity < 1024) {
    capacity = 1024;
  }
  if (capacity > (SIZE_MAX / sizeof(ShardRecord))) {
    capacity = SIZE_MAX / sizeof(ShardRecord);
  }

  ShardRecord *records = malloc(capacity * sizeof(*records));
  if (records == NULL) {
    fprintf(stderr, "Failed to allocate reduce buffer for '%s'.\n", shard_path);
    fclose(fp);
    return -1;
  }

  char *line = NULL;
  size_t line_capacity = 0;
  uint64_t record_count = 0;

  while (getline(&line, &line_capacity, fp) != -1) {
    char *trimmed = trim_in_place(line);
    if (trimmed[0] == '\0') {
      continue;
    }

    uint64_t cell_id = 0;
    uint32_t color = 0;
    if (parse_shard_record_line(trimmed, &cell_id, &color) != 0) {
      fprintf(stderr, "Corrupt shard file: %s\n", shard_path);
      free(line);
      free(records);
      fclose(fp);
      return -1;
    }

    if ((size_t)record_count == capacity) {
      if (capacity > (SIZE_MAX / sizeof(ShardRecord)) / 2) {
        fprintf(stderr, "Shard '%s' is too large to reduce in memory.\n", shard_path);
        free(line);
        free(records);
        fclose(fp);
        return -1;
      }
      size_t next_capacity = capacity * 2;
      ShardRecord *next_records = realloc(records, next_capacity * sizeof(*records));
      if (next_records == NULL) {
        fprintf(stderr, "Failed to grow reduce buffer for '%s'.\n", shard_path);
        free(line);
        free(records);
        fclose(fp);
        return -1;
      }
      records = next_records;
      capacity = next_capacity;
    }

    records[record_count].cell_id = cell_id;
    records[record_count].color = color;
    record_count++;
    progress_logger_maybe_log(progress, record_count);
  }

  free(line);
  fclose(fp);
  qsort(records, (size_t)record_count, sizeof(*records), compare_shard_records);

  uint64_t index = 0;
  while (index < record_count) {
    uint64_t cell_id = records[index].cell_id;
    uint32_t best_color = records[index].color;
    uint64_t best_count = 0;

    while (index < record_count && records[index].cell_id == cell_id) {
      uint32_t color = records[index].color;
      uint64_t count = 0;
      while (index < record_count &&
             records[index].cell_id == cell_id &&
             records[index].color == color) {
        count++;
        index++;
      }

      if (count > best_count || (count == best_count && color < best_color)) {
        best_count = count;
        best_color = color;
      }
    }

    float x, y, z;
    decode_cell_center(scaler, cell_id, &x, &y, &z);
    uint8_t r, g, b, a;
    unpack_color(best_color, &r, &g, &b, &a);
    if (buffered_writer_append_output(output_writer, x, y, z, r, g, b, a) != 0) {
      free(records);
      return -1;
    }

    (*output_point_count)++;
  }

  free(records);
  return 0;
}

static void format_ascii_float(double value, char *buffer, size_t buffer_size) {
  if (fabs(value) < 0.0000005) {
    value = 0.0;
  }

  snprintf(buffer, buffer_size, "%.6f", value);
  size_t length = strlen(buffer);
  while (length > 0 && buffer[length - 1] == '0') {
    buffer[--length] = '\0';
  }
  if (length > 0 && buffer[length - 1] == '.') {
    buffer[--length] = '\0';
  }
  if (strcmp(buffer, "-0") == 0) {
    buffer[0] = '0';
    buffer[1] = '\0';
  }
}

static int write_empty_ply(const char *output_path) {
  static const char *header =
      "ply\n"
      "format ascii 1.0\n"
      "element vertex 0\n"
      "property float x\n"
      "property float y\n"
      "property float z\n"
      "property uchar red\n"
      "property uchar green\n"
      "property uchar blue\n"
      "property uchar alpha\n"
      "end_header\n";

  FILE *fp = fopen(output_path, "wb");
  if (fp == NULL) {
    fprintf(stderr, "Failed to write '%s': %s\n", output_path, strerror(errno));
    return -1;
  }

  size_t header_len = strlen(header);
  int rc = 0;
  if (fwrite(header, 1, header_len, fp) != header_len) {
    fprintf(stderr, "Failed to write empty PLY header.\n");
    rc = -1;
  }
  fclose(fp);
  return rc;
}

static int write_final_ply(
    const char *vertex_data_path,
    const char *output_path,
    uint64_t point_count,
    ProgressLogger *progress) {
  FILE *output = fopen(output_path, "wb");
  if (output == NULL) {
    fprintf(stderr, "Failed to open '%s' for writing: %s\n", output_path, strerror(errno));
    return -1;
  }

  setvbuf(output, NULL, _IOFBF, STREAM_CHUNK_BYTES);

  char header[256];
  int header_len = snprintf(
      header,
      sizeof(header),
      "ply\n"
      "format ascii 1.0\n"
      "element vertex %" PRIu64 "\n"
      "property float x\n"
      "property float y\n"
      "property float z\n"
      "property uchar red\n"
      "property uchar green\n"
      "property uchar blue\n"
      "property uchar alpha\n"
      "end_header\n",
      point_count);
  if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
    fprintf(stderr, "Failed to build output PLY header.\n");
    fclose(output);
    return -1;
  }

  if (fwrite(header, 1, (size_t)header_len, output) != (size_t)header_len) {
    fprintf(stderr, "Failed to write PLY header.\n");
    fclose(output);
    return -1;
  }

  if (point_count == 0) {
    fclose(output);
    return 0;
  }

  FILE *input = fopen(vertex_data_path, "rb");
  if (input == NULL) {
    fprintf(stderr, "Failed to open staged output '%s': %s\n", vertex_data_path, strerror(errno));
    fclose(output);
    return -1;
  }

  setvbuf(input, NULL, _IOFBF, STREAM_CHUNK_BYTES);

  unsigned char buffer[OUTPUT_RECORD_BYTES];
  char line[128];
  char x_text[32];
  char y_text[32];
  char z_text[32];
  int rc = 0;

  for (uint64_t i = 0; i < point_count; ++i) {
    if (fread(buffer, 1, OUTPUT_RECORD_BYTES, input) != OUTPUT_RECORD_BYTES) {
      fprintf(stderr, "Corrupt staged output: incomplete point record.\n");
      rc = -1;
      break;
    }

    union {
      uint32_t u;
      float f;
    } fx, fy, fz;
    fx.u = read_u32_le(buffer);
    fy.u = read_u32_le(buffer + 4);
    fz.u = read_u32_le(buffer + 8);

    format_ascii_float(fx.f, x_text, sizeof(x_text));
    format_ascii_float(fy.f, y_text, sizeof(y_text));
    format_ascii_float(fz.f, z_text, sizeof(z_text));

    int line_len = snprintf(
        line,
        sizeof(line),
        "%s %s %s %u %u %u %u\n",
        x_text,
        y_text,
        z_text,
        (unsigned)buffer[12],
        (unsigned)buffer[13],
        (unsigned)buffer[14],
        (unsigned)buffer[15]);
    if (line_len < 0 || (size_t)line_len >= sizeof(line)) {
      fprintf(stderr, "Failed to format output vertex line.\n");
      rc = -1;
      break;
    }

    if (fwrite(line, 1, (size_t)line_len, output) != (size_t)line_len) {
      fprintf(stderr, "Failed to write output vertex line.\n");
      rc = -1;
      break;
    }

    progress_logger_maybe_log(progress, i + 1);
  }

  fclose(input);
  fclose(output);
  return rc;
}

static const char *format_duration(double total_seconds, char *buffer, size_t buffer_size) {
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

static void print_timing_summary(double total_seconds, uint64_t actual_input_point_count, uint64_t declared_vertex_count) {
  char duration[64];
  printf("Elapsed wall time: %s\n", format_duration(total_seconds, duration, sizeof(duration)));
  printf("Actual parsed input points: %" PRIu64 "\n", actual_input_point_count);

  if (declared_vertex_count > 0 && declared_vertex_count != actual_input_point_count) {
    printf("Header-declared input points: %" PRIu64 "\n", declared_vertex_count);
  }

  if (actual_input_point_count > 0) {
    double seconds_per_point = total_seconds / (double)actual_input_point_count;
    double microseconds_per_point = seconds_per_point * 1e6;
    double estimated_seconds = seconds_per_point * (double)ESTIMATE_POINT_COUNT;
    char estimate_duration[64];

    printf("Average wall time per input point: %.3f us\n", microseconds_per_point);
    printf(
        "Estimated wall time for %" PRIu64 " points: %s\n",
        ESTIMATE_POINT_COUNT,
        format_duration(estimated_seconds, estimate_duration, sizeof(estimate_duration)));
  } else {
    printf("Average wall time per input point: n/a\n");
    printf("Estimated wall time for %" PRIu64 " points: n/a\n", ESTIMATE_POINT_COUNT);
  }
}

static void print_stage_duration(const char *label, double seconds) {
  char duration[64];
  printf("%s time: %s\n", label, format_duration(seconds, duration, sizeof(duration)));
}

static void free_ply_header(PlyHeader *header) {
  for (size_t i = 0; i < header->property_count; ++i) {
    free(header->properties[i].name);
  }
  free(header->properties);
  memset(header, 0, sizeof(*header));
}

static void cleanup_temp_paths(TempPaths *temp_paths) {
  if (temp_paths->shard_writers_open) {
    for (int i = 0; i < SHARD_COUNT; ++i) {
      buffered_writer_close(&temp_paths->shard_writers[i]);
    }
    temp_paths->shard_writers_open = false;
  }

  if (temp_paths->temp_dir != NULL) {
    printf("Retained temp files under: %s\n", temp_paths->temp_dir);
  }

  if (temp_paths->output_data_path != NULL) {
    free(temp_paths->output_data_path);
    temp_paths->output_data_path = NULL;
  }

  for (int i = 0; i < SHARD_COUNT; ++i) {
    if (temp_paths->shard_paths[i] != NULL) {
      free(temp_paths->shard_paths[i]);
      temp_paths->shard_paths[i] = NULL;
    }
  }

  if (temp_paths->temp_dir != NULL) {
    free(temp_paths->temp_dir);
    temp_paths->temp_dir = NULL;
  }
}

static int init_temp_paths(TempPaths *temp_paths) {
  memset(temp_paths, 0, sizeof(*temp_paths));

  char cwd[PATH_MAX];
  if (getcwd(cwd, sizeof(cwd)) == NULL) {
    fprintf(stderr, "Failed to determine current working directory: %s\n", strerror(errno));
    return -1;
  }

  char temp_template[PATH_MAX + 32];
  int template_len = snprintf(
      temp_template,
      sizeof(temp_template),
      "%s/pointcloud-quantize-XXXXXX",
      cwd);
  if (template_len < 0 || (size_t)template_len >= sizeof(temp_template)) {
    fprintf(stderr, "Working directory path is too long for temp directory creation.\n");
    return -1;
  }

  int temp_fd = mkstemp(temp_template);
  if (temp_fd < 0) {
    fprintf(stderr, "Failed to allocate temporary path: %s\n", strerror(errno));
    return -1;
  }
  close(temp_fd);

  if (unlink(temp_template) != 0) {
    fprintf(stderr, "Failed to prepare temporary directory path: %s\n", strerror(errno));
    return -1;
  }
  if (mkdir(temp_template, 0700) != 0) {
    fprintf(stderr, "Failed to create temporary directory: %s\n", strerror(errno));
    return -1;
  }

  temp_paths->temp_dir = strdup(temp_template);
  if (temp_paths->temp_dir == NULL) {
    fprintf(stderr, "Failed to allocate temp directory path.\n");
    cleanup_temp_paths(temp_paths);
    return -1;
  }

  for (int i = 0; i < SHARD_COUNT; ++i) {
    char shard_name[32];
    snprintf(shard_name, sizeof(shard_name), "shard-%03d.bin", i);
    temp_paths->shard_paths[i] = dup_path_join(temp_paths->temp_dir, shard_name);
    if (temp_paths->shard_paths[i] == NULL) {
      fprintf(stderr, "Failed to allocate shard path.\n");
      cleanup_temp_paths(temp_paths);
      return -1;
    }
    if (buffered_writer_open(
            &temp_paths->shard_writers[i],
            temp_paths->shard_paths[i],
            SHARD_TEXT_RECORD_ESTIMATE) != 0) {
      cleanup_temp_paths(temp_paths);
      return -1;
    }
  }

  temp_paths->output_data_path = dup_path_join(temp_paths->temp_dir, "quantized-vertices.bin");
  if (temp_paths->output_data_path == NULL) {
    fprintf(stderr, "Failed to allocate staged output path.\n");
    cleanup_temp_paths(temp_paths);
    return -1;
  }

  temp_paths->shard_writers_open = true;
  return 0;
}

static int close_shard_writers(TempPaths *temp_paths) {
  int rc = 0;
  if (!temp_paths->shard_writers_open) {
    return 0;
  }

  for (int i = 0; i < SHARD_COUNT; ++i) {
    if (buffered_writer_close(&temp_paths->shard_writers[i]) != 0) {
      rc = -1;
    }
  }
  temp_paths->shard_writers_open = false;
  return rc;
}

static int parse_shard_record_line(const char *line, uint64_t *cell_id_out, uint32_t *color_out) {
  errno = 0;
  char *end = NULL;
  unsigned long long cell_id = strtoull(line, &end, 10);
  if (errno != 0 || end == line) {
    return -1;
  }

  for (int field = 0; field < 7; ++field) {
    while (*end != '\0' && isspace((unsigned char)*end)) {
      end++;
    }
    if (*end == '\0') {
      return -1;
    }

    errno = 0;
    char *next = NULL;
    if (field < 3) {
      (void)strtod(end, &next);
    } else {
      (void)strtoull(end, &next, 10);
    }
    if (errno != 0 || next == end) {
      return -1;
    }
    end = next;
  }

  while (*end != '\0' && isspace((unsigned char)*end)) {
    end++;
  }
  if (*end == '\0') {
    return -1;
  }

  errno = 0;
  char *color_end = NULL;
  unsigned long long color = strtoull(end, &color_end, 10);
  if (errno != 0 || color_end == end || color > UINT32_MAX) {
    return -1;
  }

  while (*color_end != '\0' && isspace((unsigned char)*color_end)) {
    color_end++;
  }
  if (*color_end != '\0') {
    return -1;
  }

  *cell_id_out = (uint64_t)cell_id;
  *color_out = (uint32_t)color;
  return 0;
}

int main(int argc, char **argv) {
  const double total_started_at = now_seconds();

  uint64_t log_interval = DEFAULT_LOG_INTERVAL;
  const char *input_path = NULL;
  const char *output_path = NULL;

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (strcmp(arg, "--log-interval") == 0) {
      if (i + 1 >= argc || !parse_uint64_str(argv[i + 1], &log_interval)) {
        fprintf(stderr, "Invalid value for --log-interval.\n");
        return 1;
      }
      i++;
      continue;
    }

    if (starts_with(arg, "--log-interval=")) {
      if (!parse_uint64_str(arg + strlen("--log-interval="), &log_interval)) {
        fprintf(stderr, "Invalid value for --log-interval.\n");
        return 1;
      }
      continue;
    }

    if (arg[0] == '-') {
      fprintf(stderr, "Unknown option: %s\n", arg);
      fprintf(stderr, "Usage: %s [--log-interval N] <input.ply> <output.ply>\n", argv[0]);
      return 1;
    }

    if (input_path == NULL) {
      input_path = arg;
    } else if (output_path == NULL) {
      output_path = arg;
    } else {
      fprintf(stderr, "Usage: %s [--log-interval N] <input.ply> <output.ply>\n", argv[0]);
      return 1;
    }
  }

  if (input_path == NULL || output_path == NULL) {
    fprintf(stderr, "Usage: %s [--log-interval N] <input.ply> <output.ply>\n", argv[0]);
    return 1;
  }
  if (strcmp(input_path, output_path) == 0) {
    fprintf(stderr, "Input and output paths must be different.\n");
    return 1;
  }

  if (access(input_path, R_OK) != 0) {
    fprintf(stderr, "Cannot read '%s': %s\n", input_path, strerror(errno));
    return 1;
  }
  if (ensure_parent_dir(output_path) != 0) {
    return 1;
  }

  PlyHeader header;
  if (read_ply_header(input_path, &header) != 0) {
    return 1;
  }

  if (header.property_count == 0) {
    fprintf(stderr, "PLY has no vertex element.\n");
    free_ply_header(&header);
    return 1;
  }

  if (header.vertex_count == 0) {
    if (write_empty_ply(output_path) != 0) {
      free_ply_header(&header);
      return 1;
    }
    printf("Input has no vertices. Wrote an empty quantized PLY.\n");
    print_timing_summary(now_seconds() - total_started_at, 0, header.vertex_count);
    free_ply_header(&header);
    return 0;
  }

  Bounds bounds;
  init_bounds(&bounds);
  BoundsScanContext bounds_ctx = {&bounds};
  uint64_t bounds_point_count = 0;
  double bounds_started_at = now_seconds();
  ProgressLogger bounds_progress;
  progress_logger_init(
      &bounds_progress,
      "Bounds scan",
      header.vertex_count,
      log_interval,
      bounds_started_at);
  if (for_each_vertex(
          input_path,
          &header,
          bounds_scan_visitor,
          &bounds_ctx,
          &bounds_progress,
          &bounds_point_count) != 0) {
    free_ply_header(&header);
    return 1;
  }
  double bounds_seconds = now_seconds() - bounds_started_at;

  if (!isfinite(bounds.min_x)) {
    if (write_empty_ply(output_path) != 0) {
      free_ply_header(&header);
      return 1;
    }
    printf("Input had no valid numeric vertices. Wrote an empty quantized PLY.\n");
    print_stage_duration("Bounds scan", bounds_seconds);
    print_timing_summary(now_seconds() - total_started_at, bounds_point_count, header.vertex_count);
    free_ply_header(&header);
    return 0;
  }

  Grid grid = {
      .x = (uint32_t)((SIZE_X_IN * DPI_X) > 0 ? (SIZE_X_IN * DPI_X) : 1),
      .y = (uint32_t)((SIZE_Y_IN * DPI_Y) > 0 ? (SIZE_Y_IN * DPI_Y) : 1),
      .z = (uint32_t)((SIZE_Z_IN * DPI_Z) > 0 ? (SIZE_Z_IN * DPI_Z) : 1),
  };
  Scaler scaler = build_scaler(&bounds, grid);

  TempPaths temp_paths;
  if (init_temp_paths(&temp_paths) != 0) {
    free_ply_header(&header);
    return 1;
  }

  uint64_t shard_pass_point_count = 0;
  double shard_started_at = now_seconds();
  ProgressLogger shard_progress;
  progress_logger_init(
      &shard_progress,
      "Shard points",
      header.vertex_count,
      log_interval,
      shard_started_at);
  ShardPassContext shard_ctx = {
      .scaler = &scaler,
      .writers = temp_paths.shard_writers,
      .sharded_point_count = 0,
  };
  if (for_each_vertex(
          input_path,
          &header,
          shard_pass_visitor,
          &shard_ctx,
          &shard_progress,
          &shard_pass_point_count) != 0) {
    cleanup_temp_paths(&temp_paths);
    free_ply_header(&header);
    return 1;
  }
  if (close_shard_writers(&temp_paths) != 0) {
    cleanup_temp_paths(&temp_paths);
    free_ply_header(&header);
    return 1;
  }
  double shard_seconds = now_seconds() - shard_started_at;

  uint64_t actual_input_point_count =
      bounds_point_count < shard_pass_point_count ? bounds_point_count : shard_pass_point_count;
  if (shard_ctx.sharded_point_count == 0) {
    if (write_empty_ply(output_path) != 0) {
      cleanup_temp_paths(&temp_paths);
      free_ply_header(&header);
      return 1;
    }
    printf("Input had no valid numeric vertices to quantize. Wrote an empty PLY.\n");
    print_stage_duration("Bounds scan", bounds_seconds);
    print_stage_duration("Shard points", shard_seconds);
    print_timing_summary(now_seconds() - total_started_at, actual_input_point_count, header.vertex_count);
    cleanup_temp_paths(&temp_paths);
    free_ply_header(&header);
    return 0;
  }

  BufferedWriter output_writer;
  if (buffered_writer_open(&output_writer, temp_paths.output_data_path, OUTPUT_RECORD_BYTES) != 0) {
    cleanup_temp_paths(&temp_paths);
    free_ply_header(&header);
    return 1;
  }

  uint64_t output_point_count = 0;
  double reduce_started_at = now_seconds();
  ProgressLogger reduce_progress;
  progress_logger_init(
      &reduce_progress,
      "Reduce shards",
      shard_ctx.sharded_point_count,
      log_interval,
      reduce_started_at);
  for (int i = 0; i < SHARD_COUNT; ++i) {
    if (reduce_shard(
            temp_paths.shard_paths[i],
            &scaler,
            &output_writer,
            &reduce_progress,
            &output_point_count) != 0) {
      buffered_writer_close(&output_writer);
      cleanup_temp_paths(&temp_paths);
      free_ply_header(&header);
      return 1;
    }
  }
  if (buffered_writer_close(&output_writer) != 0) {
    cleanup_temp_paths(&temp_paths);
    free_ply_header(&header);
    return 1;
  }
  double reduce_seconds = now_seconds() - reduce_started_at;

  struct stat staged_stat;
  if (stat(temp_paths.output_data_path, &staged_stat) != 0) {
    fprintf(stderr, "Failed to stat staged output '%s': %s\n", temp_paths.output_data_path, strerror(errno));
    cleanup_temp_paths(&temp_paths);
    free_ply_header(&header);
    return 1;
  }

  off_t expected_size = (off_t)(output_point_count * OUTPUT_RECORD_BYTES);
  if (staged_stat.st_size != expected_size) {
    fprintf(
        stderr,
        "Staged output size mismatch: expected %lld bytes, got %lld\n",
        (long long)expected_size,
        (long long)staged_stat.st_size);
    cleanup_temp_paths(&temp_paths);
    free_ply_header(&header);
    return 1;
  }

  double write_started_at = now_seconds();
  ProgressLogger write_progress;
  progress_logger_init(
      &write_progress,
      "Write output",
      output_point_count,
      log_interval,
      write_started_at);
  if (write_final_ply(
          temp_paths.output_data_path,
          output_path,
          output_point_count,
          &write_progress) != 0) {
    cleanup_temp_paths(&temp_paths);
    free_ply_header(&header);
    return 1;
  }
  double write_seconds = now_seconds() - write_started_at;

  printf(
      "Quantized %" PRIu64 " input points into %" PRIu64 " output points.\n",
      actual_input_point_count,
      output_point_count);
  printf("Target size: %d\" x %d\" x %d\"\n", SIZE_X_IN, SIZE_Y_IN, SIZE_Z_IN);
  printf("Target DPI: %d x %d x %d\n", DPI_X, DPI_Y, DPI_Z);
  print_stage_duration("Bounds scan", bounds_seconds);
  print_stage_duration("Shard points", shard_seconds);
  print_stage_duration("Reduce shards", reduce_seconds);
  print_stage_duration("Write output", write_seconds);
  print_timing_summary(now_seconds() - total_started_at, actual_input_point_count, header.vertex_count);

  cleanup_temp_paths(&temp_paths);
  free_ply_header(&header);
  return 0;
}
