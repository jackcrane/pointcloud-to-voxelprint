#ifndef QUANTIZE_SUPPORT_H
#define QUANTIZE_SUPPORT_H

#include "quantize_common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

double now_seconds(void);
const char *format_duration(double total_seconds, char *buffer, size_t buffer_size);
void progress_logger_init(
    ProgressLogger *progress,
    const char *label,
    uint64_t total,
    uint64_t interval,
    double started_at);
void progress_logger_maybe_log(ProgressLogger *progress, uint64_t processed);

char *trim_in_place(char *text);
bool starts_with(const char *text, const char *prefix);
char *dup_path_join(const char *dir, const char *name);
int ensure_parent_dir(const char *path);
bool parse_uint64_str(const char *text, uint64_t *value_out);
void format_ascii_float(double value, char *buffer, size_t buffer_size);

uint16_t read_u16_le(const unsigned char *ptr);
int16_t read_i16_le(const unsigned char *ptr);
uint32_t read_u32_le(const unsigned char *ptr);
int32_t read_i32_le(const unsigned char *ptr);
uint64_t read_u64_le(const unsigned char *ptr);
void write_u32_le(unsigned char *ptr, uint32_t value);

void print_stage_duration(const char *label, double seconds);
void print_timing_summary(
    double total_seconds,
    uint64_t actual_input_point_count,
    uint64_t declared_vertex_count);

#endif
