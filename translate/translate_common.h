#ifndef TRANSLATE_COMMON_H
#define TRANSLATE_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  TRANSLATE_STREAM_BUFFER_BYTES = 8 * 1024 * 1024,
  TRANSLATE_ASCII_LINE_BUFFER_BYTES = 192,
};

static const uint64_t TRANSLATE_DEFAULT_LOG_INTERVAL = UINT64_C(10000000);

typedef struct {
  const char *label;
  uint64_t total;
  uint64_t interval;
  uint64_t next_log;
  double started_at;
} TranslateProgressLogger;

typedef struct {
  const char *input_path;
  const char *output_path;
  uint64_t log_interval;
} TranslateOptions;

typedef struct {
  uint8_t version_major;
  uint8_t version_minor;
  uint8_t point_format;
  bool is_compressed;
  uint16_t header_size;
  uint16_t point_record_length;
  uint64_t point_count;
  uint64_t point_data_offset;
  size_t color_offset;
  double scale_x;
  double scale_y;
  double scale_z;
  double offset_x;
  double offset_y;
  double offset_z;
} LasHeader;

typedef int (*LasPointVisitor)(
    double x,
    double y,
    double z,
    uint16_t r,
    uint16_t g,
    uint16_t b,
    void *ctx);

#endif
