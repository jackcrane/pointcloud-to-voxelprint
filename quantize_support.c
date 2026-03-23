#include "quantize_support.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

const char *format_duration(double total_seconds, char *buffer, size_t buffer_size) {
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

void progress_logger_init(
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

void progress_logger_maybe_log(ProgressLogger *progress, uint64_t processed) {
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

char *trim_in_place(char *text) {
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

bool starts_with(const char *text, const char *prefix) {
  return strncmp(text, prefix, strlen(prefix)) == 0;
}

char *dup_path_join(const char *dir, const char *name) {
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

int ensure_parent_dir(const char *path) {
  char *copy = strdup(path);
  if (copy == NULL) {
    fprintf(stderr, "Failed to allocate memory for parent path.\n");
    return -1;
  }

  char *slash = strrchr(copy, '/');
  if (slash == NULL || slash == copy) {
    free(copy);
    return 0;
  }

  *slash = '\0';
  int rc = mkdir_p(copy);
  free(copy);
  return rc;
}

bool parse_uint64_str(const char *text, uint64_t *value_out) {
  errno = 0;
  char *end = NULL;
  unsigned long long value = strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') {
    return false;
  }

  *value_out = (uint64_t)value;
  return true;
}

void format_ascii_float(double value, char *buffer, size_t buffer_size) {
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

uint16_t read_u16_le(const unsigned char *ptr) {
  return (uint16_t)ptr[0] | ((uint16_t)ptr[1] << 8);
}

int16_t read_i16_le(const unsigned char *ptr) {
  return (int16_t)read_u16_le(ptr);
}

uint32_t read_u32_le(const unsigned char *ptr) {
  return (uint32_t)ptr[0] |
      ((uint32_t)ptr[1] << 8) |
      ((uint32_t)ptr[2] << 16) |
      ((uint32_t)ptr[3] << 24);
}

int32_t read_i32_le(const unsigned char *ptr) {
  return (int32_t)read_u32_le(ptr);
}

uint64_t read_u64_le(const unsigned char *ptr) {
  return (uint64_t)ptr[0] |
      ((uint64_t)ptr[1] << 8) |
      ((uint64_t)ptr[2] << 16) |
      ((uint64_t)ptr[3] << 24) |
      ((uint64_t)ptr[4] << 32) |
      ((uint64_t)ptr[5] << 40) |
      ((uint64_t)ptr[6] << 48) |
      ((uint64_t)ptr[7] << 56);
}

void write_u32_le(unsigned char *ptr, uint32_t value) {
  ptr[0] = (unsigned char)(value & 0xffu);
  ptr[1] = (unsigned char)((value >> 8) & 0xffu);
  ptr[2] = (unsigned char)((value >> 16) & 0xffu);
  ptr[3] = (unsigned char)((value >> 24) & 0xffu);
}

void print_stage_duration(const char *label, double seconds) {
  char duration[64];
  printf("%s time: %s\n", label, format_duration(seconds, duration, sizeof(duration)));
}

void print_timing_summary(
    double total_seconds,
    uint64_t actual_input_point_count,
    uint64_t declared_vertex_count) {
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
