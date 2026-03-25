#include "translate_las.h"

#include "translate_support.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  LAS_PUBLIC_HEADER_MIN_BYTES = 227,
  LAS_PUBLIC_HEADER_V14_BYTES = 375,
};

static int describe_point_format(
    uint8_t point_format,
    size_t *color_offset_out,
    uint16_t *minimum_record_length_out) {
  switch (point_format) {
    case 2:
      *color_offset_out = 20;
      *minimum_record_length_out = 26;
      return 0;
    case 3:
      *color_offset_out = 28;
      *minimum_record_length_out = 34;
      return 0;
    case 5:
      *color_offset_out = 28;
      *minimum_record_length_out = 63;
      return 0;
    case 7:
      *color_offset_out = 30;
      *minimum_record_length_out = 36;
      return 0;
    case 8:
      *color_offset_out = 30;
      *minimum_record_length_out = 38;
      return 0;
    case 10:
      *color_offset_out = 30;
      *minimum_record_length_out = 67;
      return 0;
    default:
      return -1;
  }
}

static int parse_las_header_bytes(
    const unsigned char *bytes,
    size_t bytes_read,
    LasHeader *header_out) {
  if (bytes_read < LAS_PUBLIC_HEADER_MIN_BYTES) {
    fprintf(stderr, "LAS file is too small to contain a valid public header.\n");
    return -1;
  }

  if (memcmp(bytes, "LASF", 4) != 0) {
    fprintf(stderr, "Input is not a LAS file: missing LASF signature.\n");
    return -1;
  }

  memset(header_out, 0, sizeof(*header_out));
  header_out->version_major = bytes[24];
  header_out->version_minor = bytes[25];
  header_out->header_size = translate_read_u16_le(bytes + 94);
  header_out->point_data_offset = (uint64_t)translate_read_u32_le(bytes + 96);
  header_out->point_record_length = translate_read_u16_le(bytes + 105);
  header_out->scale_x = translate_read_f64_le(bytes + 131);
  header_out->scale_y = translate_read_f64_le(bytes + 139);
  header_out->scale_z = translate_read_f64_le(bytes + 147);
  header_out->offset_x = translate_read_f64_le(bytes + 155);
  header_out->offset_y = translate_read_f64_le(bytes + 163);
  header_out->offset_z = translate_read_f64_le(bytes + 171);

  unsigned char raw_point_format = bytes[104];
  header_out->is_compressed = (raw_point_format & 0x80u) != 0;
  if ((raw_point_format & 0x40u) != 0) {
    fprintf(stderr, "Unsupported LAS point format flags: 0x%02x\n", raw_point_format);
    return -1;
  }
  header_out->point_format = (uint8_t)(raw_point_format & 0x3fu);

  if (header_out->version_major != 1) {
    fprintf(
        stderr,
        "Unsupported LAS version %u.%u.\n",
        (unsigned)header_out->version_major,
        (unsigned)header_out->version_minor);
    return -1;
  }

  if (header_out->is_compressed) {
    fprintf(stderr, "Compressed LAS/LAZ input is not supported.\n");
    return -1;
  }

  uint64_t legacy_point_count = (uint64_t)translate_read_u32_le(bytes + 107);
  uint64_t extended_point_count = 0;
  if (header_out->version_minor >= 4) {
    if (bytes_read < LAS_PUBLIC_HEADER_V14_BYTES) {
      fprintf(stderr, "LAS 1.4 file is missing the full public header.\n");
      return -1;
    }
    extended_point_count = translate_read_u64_le(bytes + 247);
  }
  header_out->point_count = extended_point_count > 0 ? extended_point_count : legacy_point_count;

  size_t color_offset = 0;
  uint16_t minimum_record_length = 0;
  if (describe_point_format(
          header_out->point_format,
          &color_offset,
          &minimum_record_length) != 0) {
    fprintf(
        stderr,
        "Unsupported point format %u. Supported colorized formats: 2, 3, 5, 7, 8, 10.\n",
        (unsigned)header_out->point_format);
    return -1;
  }

  if (header_out->point_record_length < minimum_record_length) {
    fprintf(
        stderr,
        "Invalid LAS record length %u for point format %u.\n",
        (unsigned)header_out->point_record_length,
        (unsigned)header_out->point_format);
    return -1;
  }

  if (header_out->header_size < LAS_PUBLIC_HEADER_MIN_BYTES) {
    fprintf(stderr, "Invalid LAS header size %u.\n", (unsigned)header_out->header_size);
    return -1;
  }

  if (header_out->point_data_offset < header_out->header_size) {
    fprintf(
        stderr,
        "Invalid LAS point data offset %" PRIu64 ".\n",
        header_out->point_data_offset);
    return -1;
  }

  header_out->color_offset = color_offset;
  return 0;
}

int read_las_header(const char *path, LasHeader *header_out) {
  FILE *fp = fopen(path, "rb");
  if (fp == NULL) {
    fprintf(stderr, "Failed to open '%s': %s\n", path, strerror(errno));
    return -1;
  }

  unsigned char bytes[LAS_PUBLIC_HEADER_V14_BYTES];
  size_t bytes_read = fread(bytes, 1, sizeof(bytes), fp);
  if (bytes_read < LAS_PUBLIC_HEADER_MIN_BYTES) {
    if (ferror(fp)) {
      fprintf(stderr, "Failed to read LAS header from '%s': %s\n", path, strerror(errno));
    } else {
      fprintf(stderr, "LAS file '%s' is too small to contain a valid header.\n", path);
    }
    fclose(fp);
    return -1;
  }

  fclose(fp);
  return parse_las_header_bytes(bytes, bytes_read, header_out);
}

int stream_las_points(
    const char *path,
    const LasHeader *header,
    LasPointVisitor visitor,
    void *ctx,
    TranslateProgressLogger *progress,
    uint64_t *processed_out) {
  FILE *fp = fopen(path, "rb");
  if (fp == NULL) {
    fprintf(stderr, "Failed to open '%s': %s\n", path, strerror(errno));
    return -1;
  }

  setvbuf(fp, NULL, _IOFBF, TRANSLATE_STREAM_BUFFER_BYTES);

  if (fseeko(fp, (off_t)header->point_data_offset, SEEK_SET) != 0) {
    fprintf(stderr, "Failed to seek to LAS point data: %s\n", strerror(errno));
    fclose(fp);
    return -1;
  }

  size_t record_length = (size_t)header->point_record_length;
  size_t records_per_batch = TRANSLATE_STREAM_BUFFER_BYTES / record_length;
  if (records_per_batch == 0) {
    records_per_batch = 1;
  }

  size_t batch_bytes = records_per_batch * record_length;
  unsigned char *buffer = malloc(batch_bytes);
  if (buffer == NULL) {
    fprintf(stderr, "Failed to allocate LAS read buffer.\n");
    fclose(fp);
    return -1;
  }

  uint64_t processed = 0;
  int rc = 0;
  while (processed < header->point_count) {
    uint64_t remaining = header->point_count - processed;
    size_t target_records = records_per_batch;
    if (remaining < (uint64_t)target_records) {
      target_records = (size_t)remaining;
    }

    size_t target_bytes = target_records * record_length;
    size_t bytes_read = fread(buffer, 1, target_bytes, fp);
    if (bytes_read != target_bytes) {
      if (ferror(fp)) {
        fprintf(stderr, "Failed to read LAS point data: %s\n", strerror(errno));
      } else {
        fprintf(
            stderr,
            "Unexpected end of LAS point data after %" PRIu64 " points.\n",
            processed);
      }
      rc = -1;
      break;
    }

    for (size_t record_index = 0; record_index < target_records; ++record_index) {
      const unsigned char *record = buffer + (record_index * record_length);
      int32_t raw_x = translate_read_i32_le(record);
      int32_t raw_y = translate_read_i32_le(record + 4);
      int32_t raw_z = translate_read_i32_le(record + 8);

      double x = (double)raw_x * header->scale_x + header->offset_x;
      double y = (double)raw_y * header->scale_y + header->offset_y;
      double z = (double)raw_z * header->scale_z + header->offset_z;

      const unsigned char *color = record + header->color_offset;
      uint16_t r = translate_read_u16_le(color);
      uint16_t g = translate_read_u16_le(color + 2);
      uint16_t b = translate_read_u16_le(color + 4);

      if (visitor(x, y, z, r, g, b, ctx) != 0) {
        rc = -1;
        break;
      }

      processed++;
      translate_progress_logger_maybe_log(progress, processed);
    }

    if (rc != 0) {
      break;
    }
  }

  free(buffer);
  fclose(fp);

  if (processed_out != NULL) {
    *processed_out = processed;
  }
  return rc;
}
