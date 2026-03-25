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
  uint32_t count;
} PairCountEntry;

typedef struct {
  PairCountEntry *entries;
  size_t capacity;
  size_t used;
} PairCountTable;

typedef struct {
  uint64_t cell_id;
  uint32_t best_color;
  uint32_t best_count;
} CellBestEntry;

typedef struct {
  CellBestEntry *entries;
  size_t capacity;
  size_t used;
} CellBestTable;

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
  char *shard_paths[SHARD_COUNT];
  char *reduced_paths[SHARD_COUNT];
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

static bool should_start_from(const QuantizeRun *run, QuantizeStartStage stage) {
  return run->options.start_stage == stage;
}

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

static Scaler build_grid_scaler(Grid grid) {
  Scaler scaler;
  memset(&scaler, 0, sizeof(scaler));
  scaler.grid = grid;
  scaler.grid_x_u64 = (uint64_t)grid.x;
  scaler.grid_y_u64 = (uint64_t)grid.y;
  return scaler;
}

static Scaler build_scaler(const Bounds *bounds, Grid grid) {
  Scaler scaler = build_grid_scaler(grid);
  scaler.min_x = bounds->min_x;
  scaler.min_y = bounds->min_y;
  scaler.min_z = bounds->min_z;
  scaler.range_x = bounds->max_x - bounds->min_x;
  scaler.range_y = bounds->max_y - bounds->min_y;
  scaler.range_z = bounds->max_z - bounds->min_z;
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

static uint64_t mix_u64(uint64_t value) {
  value += UINT64_C(0x9e3779b97f4a7c15);
  value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}

static size_t round_up_power_of_two(size_t minimum) {
  size_t capacity = 1024;
  while (capacity < minimum) {
    if (capacity > SIZE_MAX / 2) {
      return 0;
    }
    capacity *= 2;
  }
  return capacity;
}

static size_t capacity_for_expected_items(size_t expected_items) {
  size_t minimum = expected_items < 16 ? 1024 : expected_items + (expected_items / 2);
  return round_up_power_of_two(minimum);
}

static int pair_count_table_init(PairCountTable *table, size_t expected_items) {
  size_t capacity = capacity_for_expected_items(expected_items);
  if (capacity == 0 || capacity > SIZE_MAX / sizeof(*table->entries)) {
    fprintf(stderr, "Pair-count table is too large to allocate.\n");
    return -1;
  }

  table->entries = calloc(capacity, sizeof(*table->entries));
  if (table->entries == NULL) {
    fprintf(stderr, "Failed to allocate pair-count table.\n");
    return -1;
  }

  table->capacity = capacity;
  table->used = 0;
  return 0;
}

static void pair_count_table_free(PairCountTable *table) {
  free(table->entries);
  table->entries = NULL;
  table->capacity = 0;
  table->used = 0;
}

static int pair_count_table_grow(PairCountTable *table) {
  PairCountTable next = {0};
  if (pair_count_table_init(&next, table->capacity) != 0) {
    return -1;
  }

  size_t mask = next.capacity - 1;
  for (size_t i = 0; i < table->capacity; ++i) {
    PairCountEntry entry = table->entries[i];
    if (entry.count == 0) {
      continue;
    }

    size_t index = (size_t)mix_u64(entry.cell_id ^ ((uint64_t)entry.color << 32)) & mask;
    while (next.entries[index].count != 0) {
      index = (index + 1) & mask;
    }
    next.entries[index] = entry;
    next.used++;
  }

  pair_count_table_free(table);
  *table = next;
  return 0;
}

static int pair_count_table_increment(
    PairCountTable *table,
    uint64_t cell_id,
    uint32_t color,
    uint32_t *count_out) {
  if ((table->used + 1) * 10 >= table->capacity * 7 && pair_count_table_grow(table) != 0) {
    return -1;
  }

  size_t mask = table->capacity - 1;
  size_t index = (size_t)mix_u64(cell_id ^ ((uint64_t)color << 32)) & mask;
  while (true) {
    PairCountEntry *entry = &table->entries[index];
    if (entry->count == 0) {
      entry->cell_id = cell_id;
      entry->color = color;
      entry->count = 1;
      table->used++;
      *count_out = 1;
      return 0;
    }
    if (entry->cell_id == cell_id && entry->color == color) {
      if (entry->count == UINT32_MAX) {
        fprintf(stderr, "Shard color count overflowed for cell %" PRIu64 ".\n", cell_id);
        return -1;
      }
      entry->count++;
      *count_out = entry->count;
      return 0;
    }
    index = (index + 1) & mask;
  }
}

static int cell_best_table_init(CellBestTable *table, size_t expected_items) {
  size_t capacity = capacity_for_expected_items(expected_items);
  if (capacity == 0 || capacity > SIZE_MAX / sizeof(*table->entries)) {
    fprintf(stderr, "Cell-best table is too large to allocate.\n");
    return -1;
  }

  table->entries = calloc(capacity, sizeof(*table->entries));
  if (table->entries == NULL) {
    fprintf(stderr, "Failed to allocate cell-best table.\n");
    return -1;
  }

  table->capacity = capacity;
  table->used = 0;
  return 0;
}

static void cell_best_table_free(CellBestTable *table) {
  free(table->entries);
  table->entries = NULL;
  table->capacity = 0;
  table->used = 0;
}

static int cell_best_table_update(
    CellBestTable *table,
    uint64_t cell_id,
    uint32_t color,
    uint32_t count) {
  size_t mask = table->capacity - 1;
  size_t index = (size_t)mix_u64(cell_id) & mask;
  while (true) {
    CellBestEntry *entry = &table->entries[index];
    if (entry->best_count == 0) {
      entry->cell_id = cell_id;
      entry->best_color = color;
      entry->best_count = count;
      table->used++;
      return 0;
    }
    if (entry->cell_id == cell_id) {
      if (count > entry->best_count || (count == entry->best_count && color < entry->best_color)) {
        entry->best_color = color;
        entry->best_count = count;
      }
      return 0;
    }
    index = (index + 1) & mask;
  }
}

static int parse_shard_record_key(const char *line, uint64_t *cell_id_out, uint32_t *color_out) {
  errno = 0;
  char *cell_end = NULL;
  unsigned long long cell_id = strtoull(line, &cell_end, 10);
  if (errno != 0 || cell_end == line) {
    return -1;
  }

  const char *scan = cell_end;
  const char *last_token = NULL;
  while (*scan != '\0') {
    while (*scan != '\0' && isspace((unsigned char)*scan)) {
      scan++;
    }
    if (*scan == '\0') {
      break;
    }
    last_token = scan;
    while (*scan != '\0' && !isspace((unsigned char)*scan)) {
      scan++;
    }
  }
  if (last_token == NULL) {
    return -1;
  }

  errno = 0;
  char *color_end = NULL;
  unsigned long long color = strtoull(last_token, &color_end, 10);
  if (errno != 0 || color_end == last_token || color > UINT32_MAX) {
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
    const char *reduced_path,
    const Scaler *scaler,
    ProgressLogger *progress,
    uint64_t processed_base,
    uint64_t *output_point_count,
    uint64_t *input_record_count_out) {
  *input_record_count_out = 0;

  struct stat stat_buf;
  if (stat(shard_path, &stat_buf) != 0) {
    fprintf(stderr, "Failed to stat shard '%s': %s\n", shard_path, strerror(errno));
    return -1;
  }

  BufferedWriter output_writer;
  if (buffered_writer_open(&output_writer, reduced_path, OUTPUT_RECORD_BYTES) != 0) {
    return -1;
  }

  if (stat_buf.st_size == 0) {
    return buffered_writer_close(&output_writer);
  }

  FILE *fp = fopen(shard_path, "rb");
  if (fp == NULL) {
    fprintf(stderr, "Failed to open shard '%s': %s\n", shard_path, strerror(errno));
    buffered_writer_close(&output_writer);
    return -1;
  }
  setvbuf(fp, NULL, _IOFBF, STREAM_CHUNK_BYTES);

  size_t expected_pairs = (size_t)((uint64_t)stat_buf.st_size / 256);
  if (expected_pairs < 1024) {
    expected_pairs = 1024;
  }

  PairCountTable pair_counts = {0};
  if (pair_count_table_init(&pair_counts, expected_pairs) != 0) {
    fclose(fp);
    buffered_writer_close(&output_writer);
    return -1;
  }

  char *line = NULL;
  size_t line_capacity = 0;
  uint64_t records_seen = 0;
  int rc = 0;

  while (getline(&line, &line_capacity, fp) != -1) {
    char *trimmed = trim_in_place(line);
    if (trimmed[0] == '\0') {
      continue;
    }

    uint64_t cell_id = 0;
    uint32_t color = 0;
    if (parse_shard_record_key(trimmed, &cell_id, &color) != 0) {
      fprintf(stderr, "Corrupt shard file: %s\n", shard_path);
      rc = -1;
      break;
    }

      uint32_t count = 0;
    if (pair_count_table_increment(&pair_counts, cell_id, color, &count) != 0) {
      rc = -1;
      break;
    }
    records_seen++;
    progress_logger_maybe_log(progress, processed_base + records_seen);
  }

  free(line);
  if (ferror(fp)) {
    fprintf(stderr, "Failed reading shard '%s': %s\n", shard_path, strerror(errno));
    rc = -1;
  }
  fclose(fp);
  *input_record_count_out = records_seen;

  if (rc != 0) {
    pair_count_table_free(&pair_counts);
    buffered_writer_close(&output_writer);
    return -1;
  }

  CellBestTable cell_best = {0};
  if (cell_best_table_init(&cell_best, pair_counts.used) != 0) {
    pair_count_table_free(&pair_counts);
    buffered_writer_close(&output_writer);
    return -1;
  }

  for (size_t i = 0; i < pair_counts.capacity; ++i) {
    PairCountEntry entry = pair_counts.entries[i];
    if (entry.count == 0) {
      continue;
    }
    if (cell_best_table_update(&cell_best, entry.cell_id, entry.color, entry.count) != 0) {
      cell_best_table_free(&cell_best);
      pair_count_table_free(&pair_counts);
      buffered_writer_close(&output_writer);
      return -1;
    }
  }
  pair_count_table_free(&pair_counts);

  for (size_t i = 0; i < cell_best.capacity; ++i) {
    CellBestEntry entry = cell_best.entries[i];
    if (entry.best_count == 0) {
      continue;
    }

    float x, y, z;
    uint8_t r, g, b, a;
    decode_cell_center(scaler, entry.cell_id, &x, &y, &z);
    unpack_color(entry.best_color, &r, &g, &b, &a);
    if (buffered_writer_append_output(&output_writer, x, y, z, r, g, b, a) != 0) {
      cell_best_table_free(&cell_best);
      buffered_writer_close(&output_writer);
      return -1;
    }
    (*output_point_count)++;
  }

  cell_best_table_free(&cell_best);
  return buffered_writer_close(&output_writer);
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
    const TempPaths *temp_paths,
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

  unsigned char buffer[OUTPUT_RECORD_BYTES];
  char line[128];
  char x_text[32];
  char y_text[32];
  char z_text[32];
  int rc = 0;
  uint64_t written_count = 0;

  for (int shard_index = 0; shard_index < SHARD_COUNT; ++shard_index) {
    const char *vertex_data_path = temp_paths->reduced_paths[shard_index];
    FILE *input = fopen(vertex_data_path, "rb");
    if (input == NULL) {
      fprintf(stderr, "Failed to open staged output '%s': %s\n", vertex_data_path, strerror(errno));
      rc = -1;
      break;
    }

    setvbuf(input, NULL, _IOFBF, STREAM_CHUNK_BYTES);

    while (true) {
      size_t bytes_read = fread(buffer, 1, OUTPUT_RECORD_BYTES, input);
      if (bytes_read == 0) {
        if (ferror(input)) {
          fprintf(stderr, "Failed reading staged output '%s': %s\n", vertex_data_path, strerror(errno));
          rc = -1;
        }
        break;
      }
      if (bytes_read != OUTPUT_RECORD_BYTES) {
        fprintf(stderr, "Corrupt staged output '%s': incomplete point record.\n", vertex_data_path);
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

      written_count++;
      progress_logger_maybe_log(progress, written_count);
    }

    fclose(input);
    if (rc != 0) {
      break;
    }
  }

  if (rc == 0 && written_count != point_count) {
    fprintf(
        stderr,
        "Staged output record mismatch: expected %" PRIu64 ", wrote %" PRIu64 "\n",
        point_count,
        written_count);
    rc = -1;
  }

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

  for (int i = 0; i < SHARD_COUNT; ++i) {
    free(temp_paths->shard_paths[i]);
    temp_paths->shard_paths[i] = NULL;
    free(temp_paths->reduced_paths[i]);
    temp_paths->reduced_paths[i] = NULL;
  }

  free(temp_paths->temp_dir);
  temp_paths->temp_dir = NULL;
}

static int ensure_directory_exists(const char *dir_path) {
  struct stat stat_buf;
  if (stat(dir_path, &stat_buf) == 0) {
    if (!S_ISDIR(stat_buf.st_mode)) {
      fprintf(stderr, "Temp path exists but is not a directory: %s\n", dir_path);
      return -1;
    }
    return 0;
  }
  if (errno != ENOENT) {
    fprintf(stderr, "Failed to stat temp directory '%s': %s\n", dir_path, strerror(errno));
    return -1;
  }

  if (ensure_parent_dir(dir_path) != 0) {
    return -1;
  }
  if (mkdir(dir_path, 0700) != 0 && errno != EEXIST) {
    fprintf(stderr, "Failed to create temp directory '%s': %s\n", dir_path, strerror(errno));
    return -1;
  }
  return 0;
}

static int configure_temp_paths(
    TempPaths *temp_paths,
    const char *base_dir,
    bool open_shard_writers) {
  temp_paths->temp_dir = strdup(base_dir);
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

    if (open_shard_writers &&
        buffered_writer_open(
            &temp_paths->shard_writers[i],
            temp_paths->shard_paths[i],
            96) != 0) {
      cleanup_temp_paths(temp_paths);
      return -1;
    }

    char reduced_name[32];
    snprintf(reduced_name, sizeof(reduced_name), "reduced-%03d.bin", i);
    temp_paths->reduced_paths[i] = dup_path_join(temp_paths->temp_dir, reduced_name);
    if (temp_paths->reduced_paths[i] == NULL) {
      fprintf(stderr, "Failed to allocate reduced shard path.\n");
      cleanup_temp_paths(temp_paths);
      return -1;
    }
  }

  temp_paths->shard_writers_open = open_shard_writers;
  return 0;
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

  return configure_temp_paths(temp_paths, temp_template, true);
}

static int init_temp_paths_with_base(TempPaths *temp_paths, const char *base_dir) {
  memset(temp_paths, 0, sizeof(*temp_paths));

  if (ensure_directory_exists(base_dir) != 0) {
    return -1;
  }

  return configure_temp_paths(temp_paths, base_dir, true);
}

static int init_existing_temp_paths(TempPaths *temp_paths, const char *base_dir) {
  memset(temp_paths, 0, sizeof(*temp_paths));

  if (ensure_directory_exists(base_dir) != 0) {
    return -1;
  }

  return configure_temp_paths(temp_paths, base_dir, false);
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
  if (run->shard_pass_point_count == 0) {
    if (run->bounds_point_count > 0) {
      return run->bounds_point_count;
    }
    return run->sharded_point_count;
  }
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
  if (ensure_parent_dir(run->options.output_path) != 0) {
    return -1;
  }
  if (run->options.start_stage != QUANTIZE_START_BOUNDS && run->options.temp_dir_path == NULL) {
    fprintf(stderr, "--temp-dir is required when --steps starts at shard or reduce.\n");
    return -1;
  }
  return 0;
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

static int prepare_resume_from_shard(QuantizeRun *run) {
  if (scan_bounds(run) != 0) {
    return -1;
  }
  if (!bounds_are_valid(&run->bounds)) {
    fprintf(stderr, "Input had no valid numeric vertices to quantize.\n");
    return -1;
  }

  run->scaler = build_scaler(&run->bounds, build_default_grid());
  if (init_temp_paths_with_base(&run->temp_paths, run->options.temp_dir_path) != 0) {
    return -1;
  }
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
  ProgressLogger progress;
  double started_at = now_seconds();
  uint64_t reduced_input_record_count = 0;

  progress_logger_init(
      &progress,
      "Reduce shards",
      run->sharded_point_count,
      run->options.log_interval,
      started_at);
  for (int i = 0; i < SHARD_COUNT; ++i) {
    uint64_t shard_input_record_count = 0;
    if (reduce_shard(
            run->temp_paths.shard_paths[i],
            run->temp_paths.reduced_paths[i],
            &run->scaler,
            &progress,
            reduced_input_record_count,
            &run->output_point_count,
            &shard_input_record_count) != 0) {
      return -1;
    }
    reduced_input_record_count += shard_input_record_count;
  }

  if (run->options.start_stage == QUANTIZE_START_REDUCE || run->sharded_point_count == 0) {
    run->sharded_point_count = reduced_input_record_count;
  }
  run->reduce_seconds = now_seconds() - started_at;
  return 0;
}

static int verify_staged_output(const QuantizeRun *run) {
  off_t total_size = 0;
  for (int i = 0; i < SHARD_COUNT; ++i) {
    struct stat staged_stat;
    if (stat(run->temp_paths.reduced_paths[i], &staged_stat) != 0) {
      fprintf(
          stderr,
          "Failed to stat staged output '%s': %s\n",
          run->temp_paths.reduced_paths[i],
          strerror(errno));
      return -1;
    }
    if ((staged_stat.st_size % OUTPUT_RECORD_BYTES) != 0) {
      fprintf(
          stderr,
          "Corrupt staged output '%s': size %lld is not a multiple of %d bytes\n",
          run->temp_paths.reduced_paths[i],
          (long long)staged_stat.st_size,
          OUTPUT_RECORD_BYTES);
      return -1;
    }
    total_size += staged_stat.st_size;
  }

  off_t expected_size = (off_t)(run->output_point_count * OUTPUT_RECORD_BYTES);
  if (total_size != expected_size) {
    fprintf(
        stderr,
        "Staged output size mismatch: expected %lld bytes, got %lld\n",
        (long long)expected_size,
        (long long)total_size);
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
          &run->temp_paths,
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

  if (run->options.start_stage == QUANTIZE_START_BOUNDS) {
    print_stage_duration("Bounds scan", run->bounds_seconds);
  }
  if (run->options.start_stage <= QUANTIZE_START_SHARD) {
    print_stage_duration("Shard points", run->shard_seconds);
  }
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
    rc = write_empty_result(
        &run,
        "Input has no vertices. Wrote an empty quantized PLY.",
        false,
        false);
    goto cleanup;
  }

  if (should_start_from(&run, QUANTIZE_START_BOUNDS)) {
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
    if ((run.options.temp_dir_path != NULL
             ? init_temp_paths_with_base(&run.temp_paths, run.options.temp_dir_path)
             : init_temp_paths(&run.temp_paths)) != 0) {
      goto cleanup;
    }
  } else if (should_start_from(&run, QUANTIZE_START_SHARD)) {
    if (prepare_resume_from_shard(&run) != 0) {
      goto cleanup;
    }
  } else {
    run.scaler = build_grid_scaler(build_default_grid());
    if (init_existing_temp_paths(&run.temp_paths, run.options.temp_dir_path) != 0) {
      goto cleanup;
    }
    run.sharded_point_count = 0;
  }

  if (run.options.start_stage <= QUANTIZE_START_SHARD) {
    if (shard_vertices(&run) != 0) {
      goto cleanup;
    }
    if (run.sharded_point_count == 0) {
      rc = write_empty_result(
          &run,
          "Input had no valid numeric vertices to quantize. Wrote an empty PLY.",
          run.options.start_stage == QUANTIZE_START_BOUNDS,
          true);
      goto cleanup;
    }
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
