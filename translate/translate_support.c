#include "translate_support.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

double translate_now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

const char *translate_format_duration(double total_seconds, char *buffer, size_t buffer_size) {
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

void translate_format_ascii_double(double value, char *buffer, size_t buffer_size) {
  if (fabs(value) < 0.0000000000005) {
    value = 0.0;
  }

  snprintf(buffer, buffer_size, "%.15g", value);
  if (strcmp(buffer, "-0") == 0 || strcmp(buffer, "-0.0") == 0) {
    buffer[0] = '0';
    buffer[1] = '\0';
  }
}

void translate_progress_logger_init(
    TranslateProgressLogger *progress,
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

void translate_progress_logger_maybe_log(
    TranslateProgressLogger *progress,
    uint64_t processed) {
  if (progress == NULL || progress->interval == 0 || processed < progress->next_log) {
    return;
  }

  char elapsed[64];
  double elapsed_seconds = translate_now_seconds() - progress->started_at;

  if (progress->total > 0) {
    double percent = ((double)processed * 100.0) / (double)progress->total;
    char eta[64];
    const char *eta_text = "n/a";
    if (processed > 0 && processed < progress->total) {
      double seconds_per_point = elapsed_seconds / (double)processed;
      double eta_seconds = seconds_per_point * (double)(progress->total - processed);
      eta_text = translate_format_duration(eta_seconds, eta, sizeof(eta));
    } else if (processed >= progress->total) {
      eta_text = translate_format_duration(0.0, eta, sizeof(eta));
    }

    printf(
        "%s progress: %" PRIu64 "/%" PRIu64 " (%.2f%%, elapsed %s, estimated remaining %s)\n",
        progress->label,
        processed,
        progress->total,
        percent,
        translate_format_duration(elapsed_seconds, elapsed, sizeof(elapsed)),
        eta_text);
  } else {
    printf(
        "%s progress: %" PRIu64 " points (elapsed %s)\n",
        progress->label,
        processed,
        translate_format_duration(elapsed_seconds, elapsed, sizeof(elapsed)));
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

bool translate_parse_uint64_str(const char *text, uint64_t *value_out) {
  errno = 0;
  char *end = NULL;
  unsigned long long value = strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') {
    return false;
  }

  *value_out = (uint64_t)value;
  return true;
}

static int translate_mkdir_p(const char *dir_path) {
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

int translate_ensure_parent_dir(const char *path) {
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
  int rc = translate_mkdir_p(copy);
  free(copy);
  return rc;
}

uint16_t translate_read_u16_le(const unsigned char *ptr) {
  return (uint16_t)ptr[0] | ((uint16_t)ptr[1] << 8);
}

uint32_t translate_read_u32_le(const unsigned char *ptr) {
  return (uint32_t)ptr[0] |
      ((uint32_t)ptr[1] << 8) |
      ((uint32_t)ptr[2] << 16) |
      ((uint32_t)ptr[3] << 24);
}

int32_t translate_read_i32_le(const unsigned char *ptr) {
  return (int32_t)translate_read_u32_le(ptr);
}

uint64_t translate_read_u64_le(const unsigned char *ptr) {
  return (uint64_t)ptr[0] |
      ((uint64_t)ptr[1] << 8) |
      ((uint64_t)ptr[2] << 16) |
      ((uint64_t)ptr[3] << 24) |
      ((uint64_t)ptr[4] << 32) |
      ((uint64_t)ptr[5] << 40) |
      ((uint64_t)ptr[6] << 48) |
      ((uint64_t)ptr[7] << 56);
}

double translate_read_f64_le(const unsigned char *ptr) {
  union {
    uint64_t u64;
    double f64;
  } value;
  value.u64 = translate_read_u64_le(ptr);
  return value.f64;
}

void translate_write_u16_le(unsigned char *ptr, uint16_t value) {
  ptr[0] = (unsigned char)(value & 0xffu);
  ptr[1] = (unsigned char)((value >> 8) & 0xffu);
}

void translate_write_u64_le(unsigned char *ptr, uint64_t value) {
  ptr[0] = (unsigned char)(value & 0xffu);
  ptr[1] = (unsigned char)((value >> 8) & 0xffu);
  ptr[2] = (unsigned char)((value >> 16) & 0xffu);
  ptr[3] = (unsigned char)((value >> 24) & 0xffu);
  ptr[4] = (unsigned char)((value >> 32) & 0xffu);
  ptr[5] = (unsigned char)((value >> 40) & 0xffu);
  ptr[6] = (unsigned char)((value >> 48) & 0xffu);
  ptr[7] = (unsigned char)((value >> 56) & 0xffu);
}

void translate_write_f64_le(unsigned char *ptr, double value) {
  union {
    uint64_t u64;
    double f64;
  } bits;
  bits.f64 = value;
  translate_write_u64_le(ptr, bits.u64);
}

void translate_print_stage_duration(const char *label, double seconds) {
  char duration[64];
  printf(
      "%s time: %s\n",
      label,
      translate_format_duration(seconds, duration, sizeof(duration)));
}

void translate_print_timing_summary(double total_seconds, uint64_t point_count) {
  char duration[64];
  printf(
      "Elapsed wall time: %s\n",
      translate_format_duration(total_seconds, duration, sizeof(duration)));
  printf("Translated points: %" PRIu64 "\n", point_count);

  if (point_count > 0) {
    double seconds_per_point = total_seconds / (double)point_count;
    printf("Average wall time per point: %.3f us\n", seconds_per_point * 1e6);
  } else {
    printf("Average wall time per point: n/a\n");
  }
}
