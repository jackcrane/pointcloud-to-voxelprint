#include "quantize_pipeline.h"

#include "quantize_ply.h"
#include "quantize_support.h"

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
#include <sys/stat.h>
#include <unistd.h>

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
  char *temp_dir;
  char *output_data_path;
  char *shard_paths[SHARD_COUNT];
  BufferedWriter shard_writers[SHARD_COUNT];
  bool shard_writers_open;
} TempPaths;

typedef struct {
  QuantizeOptions options;
  double total_started_at;
  PlyHeader header;
  Bounds bounds;
  Scaler scaler;
  TempPaths temp_paths;
  uint64_t bounds_point_count;
  double bounds_seconds;
  uint64_t shard_pass_point_count;
  uint64_t sharded_point_count;
  double shard_seconds;
  uint64_t output_point_count;
  double reduce_seconds;
  double write_seconds;
} QuantizeRun;

static void init_bounds(Bounds *bounds) {
  bounds->min_x = INFINITY;
  bounds->min_y = INFINITY;
  bounds->min_z = INFINITY;
  bounds->max_x = -INFINITY;
  bounds->max_y = -INFINITY;
  bounds->max_z = -INFINITY;
}

static Grid build_default_grid(void) {
  Grid grid = {
      .x = (uint32_t)((SIZE_X_IN * DPI_X) > 0 ? (SIZE_X_IN * DPI_X) : 1),
      .y = (uint32_t)((SIZE_Y_IN * DPI_Y) > 0 ? (SIZE_Y_IN * DPI_Y) : 1),
      .z = (uint32_t)((SIZE_Z_IN * DPI_Z) > 0 ? (SIZE_Z_IN * DPI_Z) : 1),
  };
  return grid;
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
      scaler->grid_x_u64 * ((uint64_t)iy + scaler->grid_y_u64 * (uint64_t)iz);
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

static uint32_t pack_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}

static void unpack_color(uint32_t packed, uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a) {
  *r = (uint8_t)(packed & 0xffu);
  *g = (uint8_t)((packed >> 8) & 0xffu);
  *b = (uint8_t)((packed >> 16) & 0xffu);
  *a = (uint8_t)((packed >> 24) & 0xffu);
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
      pack_color(r, g, b, a));
  if (line_len < 0 || (size_t)line_len >= sizeof(line)) {
    fprintf(stderr, "Failed to format shard record.\n");
    return -1;
  }

  size_t bytes_needed = (size_t)line_len;
  if (writer->used + bytes_needed > writer->capacity && buffered_writer_flush(writer) != 0) {
    return -1;
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
  if (writer->used + OUTPUT_RECORD_BYTES > writer->capacity &&
      buffered_writer_flush(writer) != 0) {
    return -1;
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
  Bounds *bounds = ((BoundsScanContext *)ctx)->bounds;

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
  int rc = 0;

  while (getline(&line, &line_capacity, fp) != -1) {
    char *trimmed = trim_in_place(line);
    if (trimmed[0] == '\0') {
      continue;
    }

    uint64_t cell_id = 0;
    uint32_t color = 0;
    if (parse_shard_record_line(trimmed, &cell_id, &color) != 0) {
      fprintf(stderr, "Corrupt shard file: %s\n", shard_path);
      rc = -1;
      break;
    }

    if ((size_t)record_count == capacity) {
      if (capacity > (SIZE_MAX / sizeof(ShardRecord)) / 2) {
        fprintf(stderr, "Shard '%s' is too large to reduce in memory.\n", shard_path);
        rc = -1;
        break;
      }

      size_t next_capacity = capacity * 2;
      ShardRecord *next_records = realloc(records, next_capacity * sizeof(*records));
      if (next_records == NULL) {
        fprintf(stderr, "Failed to grow reduce buffer for '%s'.\n", shard_path);
        rc = -1;
        break;
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

  if (rc != 0) {
    free(records);
    return -1;
  }

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
    uint8_t r, g, b, a;
    decode_cell_center(scaler, cell_id, &x, &y, &z);
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

  free(temp_paths->output_data_path);
  temp_paths->output_data_path = NULL;

  for (int i = 0; i < SHARD_COUNT; ++i) {
    free(temp_paths->shard_paths[i]);
    temp_paths->shard_paths[i] = NULL;
  }

  free(temp_paths->temp_dir);
  temp_paths->temp_dir = NULL;
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

static void quantize_run_init(QuantizeRun *run, const QuantizeOptions *options) {
  memset(run, 0, sizeof(*run));
  run->options = *options;
  run->total_started_at = now_seconds();
  init_bounds(&run->bounds);
}

static uint64_t actual_input_point_count(const QuantizeRun *run) {
  return run->bounds_point_count < run->shard_pass_point_count
      ? run->bounds_point_count
      : run->shard_pass_point_count;
}

static bool bounds_are_valid(const Bounds *bounds) {
  return isfinite(bounds->min_x);
}

static int prepare_paths(const QuantizeRun *run) {
  if (access(run->options.input_path, R_OK) != 0) {
    fprintf(stderr, "Cannot read '%s': %s\n", run->options.input_path, strerror(errno));
    return -1;
  }
  return ensure_parent_dir(run->options.output_path);
}

static int scan_bounds(QuantizeRun *run) {
  BoundsScanContext bounds_ctx = {&run->bounds};
  ProgressLogger progress;
  double started_at = now_seconds();

  progress_logger_init(
      &progress,
      "Bounds scan",
      run->header.vertex_count,
      run->options.log_interval,
      started_at);
  if (for_each_vertex(
          run->options.input_path,
          &run->header,
          bounds_scan_visitor,
          &bounds_ctx,
          &progress,
          &run->bounds_point_count) != 0) {
    return -1;
  }

  run->bounds_seconds = now_seconds() - started_at;
  return 0;
}

static int shard_vertices(QuantizeRun *run) {
  ShardPassContext shard_ctx = {
      .scaler = &run->scaler,
      .writers = run->temp_paths.shard_writers,
      .sharded_point_count = 0,
  };
  ProgressLogger progress;
  double started_at = now_seconds();

  progress_logger_init(
      &progress,
      "Shard points",
      run->header.vertex_count,
      run->options.log_interval,
      started_at);
  if (for_each_vertex(
          run->options.input_path,
          &run->header,
          shard_pass_visitor,
          &shard_ctx,
          &progress,
          &run->shard_pass_point_count) != 0) {
    return -1;
  }
  if (close_shard_writers(&run->temp_paths) != 0) {
    return -1;
  }

  run->shard_seconds = now_seconds() - started_at;
  run->sharded_point_count = shard_ctx.sharded_point_count;
  return 0;
}

static int reduce_shards_to_staging(QuantizeRun *run) {
  BufferedWriter output_writer;
  ProgressLogger progress;
  double started_at = now_seconds();

  if (buffered_writer_open(&output_writer, run->temp_paths.output_data_path, OUTPUT_RECORD_BYTES) != 0) {
    return -1;
  }

  progress_logger_init(
      &progress,
      "Reduce shards",
      run->sharded_point_count,
      run->options.log_interval,
      started_at);
  for (int i = 0; i < SHARD_COUNT; ++i) {
    if (reduce_shard(
            run->temp_paths.shard_paths[i],
            &run->scaler,
            &output_writer,
            &progress,
            &run->output_point_count) != 0) {
      buffered_writer_close(&output_writer);
      return -1;
    }
  }
  if (buffered_writer_close(&output_writer) != 0) {
    return -1;
  }

  run->reduce_seconds = now_seconds() - started_at;
  return 0;
}

static int verify_staged_output(const QuantizeRun *run) {
  struct stat staged_stat;
  if (stat(run->temp_paths.output_data_path, &staged_stat) != 0) {
    fprintf(
        stderr,
        "Failed to stat staged output '%s': %s\n",
        run->temp_paths.output_data_path,
        strerror(errno));
    return -1;
  }

  off_t expected_size = (off_t)(run->output_point_count * OUTPUT_RECORD_BYTES);
  if (staged_stat.st_size != expected_size) {
    fprintf(
        stderr,
        "Staged output size mismatch: expected %lld bytes, got %lld\n",
        (long long)expected_size,
        (long long)staged_stat.st_size);
    return -1;
  }

  return 0;
}

static int write_output(QuantizeRun *run) {
  ProgressLogger progress;
  double started_at = now_seconds();

  progress_logger_init(
      &progress,
      "Write output",
      run->output_point_count,
      run->options.log_interval,
      started_at);
  if (write_final_ply(
          run->temp_paths.output_data_path,
          run->options.output_path,
          run->output_point_count,
          &progress) != 0) {
    return -1;
  }

  run->write_seconds = now_seconds() - started_at;
  return 0;
}

static int write_empty_result(
    const QuantizeRun *run,
    const char *message,
    bool print_bounds_time,
    bool print_shard_time) {
  if (write_empty_ply(run->options.output_path) != 0) {
    return -1;
  }

  printf("%s\n", message);
  if (print_bounds_time) {
    print_stage_duration("Bounds scan", run->bounds_seconds);
  }
  if (print_shard_time) {
    print_stage_duration("Shard points", run->shard_seconds);
  }
  print_timing_summary(
      now_seconds() - run->total_started_at,
      actual_input_point_count(run),
      run->header.vertex_count);
  return 0;
}

static void print_success_summary(const QuantizeRun *run) {
  printf(
      "Quantized %" PRIu64 " input points into %" PRIu64 " output points.\n",
      actual_input_point_count(run),
      run->output_point_count);
  printf("Target size: %d\" x %d\" x %d\"\n", SIZE_X_IN, SIZE_Y_IN, SIZE_Z_IN);
  printf("Target DPI: %d x %d x %d\n", DPI_X, DPI_Y, DPI_Z);
  print_stage_duration("Bounds scan", run->bounds_seconds);
  print_stage_duration("Shard points", run->shard_seconds);
  print_stage_duration("Reduce shards", run->reduce_seconds);
  print_stage_duration("Write output", run->write_seconds);
  print_timing_summary(
      now_seconds() - run->total_started_at,
      actual_input_point_count(run),
      run->header.vertex_count);
}

int run_quantize(const QuantizeOptions *options) {
  QuantizeRun run;
  int rc = -1;

  quantize_run_init(&run, options);

  if (prepare_paths(&run) != 0) {
    goto cleanup;
  }
  if (read_ply_header(run.options.input_path, &run.header) != 0) {
    goto cleanup;
  }
  if (run.header.property_count == 0) {
    fprintf(stderr, "PLY has no vertex element.\n");
    goto cleanup;
  }
  if (run.header.vertex_count == 0) {
    rc = write_empty_result(&run, "Input has no vertices. Wrote an empty quantized PLY.", false, false);
    goto cleanup;
  }

  if (scan_bounds(&run) != 0) {
    goto cleanup;
  }
  if (!bounds_are_valid(&run.bounds)) {
    rc = write_empty_result(
        &run,
        "Input had no valid numeric vertices. Wrote an empty quantized PLY.",
        true,
        false);
    goto cleanup;
  }

  run.scaler = build_scaler(&run.bounds, build_default_grid());
  if (init_temp_paths(&run.temp_paths) != 0) {
    goto cleanup;
  }
  if (shard_vertices(&run) != 0) {
    goto cleanup;
  }
  if (run.sharded_point_count == 0) {
    rc = write_empty_result(
        &run,
        "Input had no valid numeric vertices to quantize. Wrote an empty PLY.",
        true,
        true);
    goto cleanup;
  }

  if (reduce_shards_to_staging(&run) != 0) {
    goto cleanup;
  }
  if (verify_staged_output(&run) != 0) {
    goto cleanup;
  }
  if (write_output(&run) != 0) {
    goto cleanup;
  }

  print_success_summary(&run);
  rc = 0;

cleanup:
  cleanup_temp_paths(&run.temp_paths);
  free_ply_header(&run.header);
  return rc;
}
