#ifndef ASCII_PLY_SUPPORT_H
#define ASCII_PLY_SUPPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  ASCII_PLY_STREAM_BUFFER_BYTES = 8 * 1024 * 1024,
  ASCII_PLY_COPY_BUFFER_BYTES = 1024 * 1024,
};

static const uint64_t ASCII_PLY_DEFAULT_LOG_INTERVAL = UINT64_C(10000000);

typedef struct {
  const char *label;
  uint64_t total;
  uint64_t interval;
  uint64_t next_log;
  double started_at;
} AsciiPlyProgressLogger;

double ascii_ply_now_seconds(void);
const char *ascii_ply_format_duration(
    double total_seconds,
    char *buffer,
    size_t buffer_size);
void ascii_ply_progress_logger_init(
    AsciiPlyProgressLogger *progress,
    const char *label,
    uint64_t total,
    uint64_t interval,
    double started_at);
void ascii_ply_progress_logger_maybe_log(
    AsciiPlyProgressLogger *progress,
    uint64_t processed);

bool ascii_ply_starts_with(const char *text, const char *prefix);
int ascii_ply_ensure_parent_dir(const char *path);
bool ascii_ply_parse_uint64_str(const char *text, uint64_t *value_out);
bool ascii_ply_parse_double_str(const char *text, double *value_out);
void ascii_ply_format_ascii_double(double value, char *buffer, size_t buffer_size);

#endif
