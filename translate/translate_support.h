#ifndef TRANSLATE_SUPPORT_H
#define TRANSLATE_SUPPORT_H

#include "translate_common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

double translate_now_seconds(void);
const char *translate_format_duration(double total_seconds, char *buffer, size_t buffer_size);
void translate_format_ascii_double(double value, char *buffer, size_t buffer_size);

void translate_progress_logger_init(
    TranslateProgressLogger *progress,
    const char *label,
    uint64_t total,
    uint64_t interval,
    double started_at);
void translate_progress_logger_maybe_log(
    TranslateProgressLogger *progress,
    uint64_t processed);

bool translate_parse_uint64_str(const char *text, uint64_t *value_out);
int translate_ensure_parent_dir(const char *path);

uint16_t translate_read_u16_le(const unsigned char *ptr);
uint32_t translate_read_u32_le(const unsigned char *ptr);
int32_t translate_read_i32_le(const unsigned char *ptr);
uint64_t translate_read_u64_le(const unsigned char *ptr);
double translate_read_f64_le(const unsigned char *ptr);

void translate_write_u16_le(unsigned char *ptr, uint16_t value);
void translate_write_u64_le(unsigned char *ptr, uint64_t value);
void translate_write_f64_le(unsigned char *ptr, double value);

void translate_print_stage_duration(const char *label, double seconds);
void translate_print_timing_summary(double total_seconds, uint64_t point_count);

#endif
