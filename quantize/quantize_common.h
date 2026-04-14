#ifndef QUANTIZE_COMMON_H
#define QUANTIZE_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

static const uint64_t ESTIMATE_POINT_COUNT = UINT64_C(1239896640);
static const uint64_t DEFAULT_LOG_INTERVAL = UINT64_C(10000000);

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
  long long header_end_offset;
} PlyHeader;

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
  const char *label;
  uint64_t total;
  uint64_t interval;
  uint64_t next_log;
  double started_at;
} ProgressLogger;

typedef enum {
  QUANTIZE_START_BOUNDS = 0,
  QUANTIZE_START_SHARD = 1,
  QUANTIZE_START_REDUCE = 2,
} QuantizeStartStage;

typedef struct {
  const char *input_path;
  const char *output_path;
  const char *temp_dir_path;
  uint64_t log_interval;
  QuantizeStartStage start_stage;
  bool steps_specified;
} QuantizeOptions;

typedef int (*VertexVisitor)(const Vertex *vertex, void *ctx);

#endif
