#include "rotate_pipeline.h"

#include "../ascii_ply/ascii_ply.h"
#include "../ascii_ply/ascii_ply_support.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  double x;
  double y;
  double z;
} Point3D;

static Point3D rotate_point(const RotateOptions *options, double radians, Point3D point) {
  double sin_theta = sin(radians);
  double cos_theta = cos(radians);

  double dx = point.x - options->centroid_x;
  double dy = point.y - options->centroid_y;
  double dz = point.z - options->centroid_z;

  Point3D rotated = point;
  switch (options->axis) {
    case ROTATE_AXIS_X:
      rotated.y = options->centroid_y + (dy * cos_theta) - (dz * sin_theta);
      rotated.z = options->centroid_z + (dy * sin_theta) + (dz * cos_theta);
      break;
    case ROTATE_AXIS_Y:
      rotated.x = options->centroid_x + (dx * cos_theta) + (dz * sin_theta);
      rotated.z = options->centroid_z - (dx * sin_theta) + (dz * cos_theta);
      break;
    case ROTATE_AXIS_Z:
      rotated.x = options->centroid_x + (dx * cos_theta) - (dy * sin_theta);
      rotated.y = options->centroid_y + (dx * sin_theta) + (dy * cos_theta);
      break;
  }

  return rotated;
}

static void sort_replacements(
    AsciiPlyTokenReplacement *replacements,
    size_t replacement_count) {
  for (size_t i = 0; i < replacement_count; ++i) {
    for (size_t j = i + 1; j < replacement_count; ++j) {
      if (replacements[j].token_index < replacements[i].token_index) {
        AsciiPlyTokenReplacement temp = replacements[i];
        replacements[i] = replacements[j];
        replacements[j] = temp;
      }
    }
  }
}

int run_rotate(const RotateOptions *options) {
  if (options == NULL) {
    fprintf(stderr, "Missing rotate options.\n");
    return -1;
  }

  int rc = -1;
  AsciiPlyHeader header;
  AsciiPlyReader reader;
  AsciiPlyWriter writer;
  uint64_t processed = 0;
  memset(&header, 0, sizeof(header));
  memset(&reader, 0, sizeof(reader));
  memset(&writer, 0, sizeof(writer));

  double started_at = ascii_ply_now_seconds();
  double radians = options->angle_degrees * (acos(-1.0) / 180.0);

  if (ascii_ply_read_header(options->input_path, &header) != 0) {
    goto cleanup;
  }

  if (ascii_ply_reader_open(&reader, options->input_path, &header) != 0) {
    goto cleanup;
  }

  if (ascii_ply_writer_open(&writer, options->output_path, &header, header.vertex_count) != 0) {
    goto cleanup;
  }

  printf("Input vertex count: %" PRIu64 "\n", header.vertex_count);
  printf("Output vertex count: %" PRIu64 "\n", header.vertex_count);
  printf("Rotation angle (degrees): %.12g\n", options->angle_degrees);

  AsciiPlyProgressLogger progress;
  ascii_ply_progress_logger_init(
      &progress,
      "Rotate",
      header.vertex_count,
      options->log_interval,
      started_at);

  while (true) {
    AsciiPlyVertexLine vertex;
    int next_rc = ascii_ply_reader_next_vertex(&reader, &vertex);
    if (next_rc < 0) {
      goto cleanup;
    }
    if (next_rc == 0) {
      break;
    }

    Point3D point;
    if (!ascii_ply_parse_token_double(&vertex, header.x_index, &point.x) ||
        !ascii_ply_parse_token_double(&vertex, header.y_index, &point.y) ||
        !ascii_ply_parse_token_double(&vertex, header.z_index, &point.z)) {
      fprintf(stderr, "Failed to parse vertex coordinates.\n");
      goto cleanup;
    }

    Point3D rotated = rotate_point(options, radians, point);

    char x_text[64];
    char y_text[64];
    char z_text[64];
    ascii_ply_format_ascii_double(rotated.x, x_text, sizeof(x_text));
    ascii_ply_format_ascii_double(rotated.y, y_text, sizeof(y_text));
    ascii_ply_format_ascii_double(rotated.z, z_text, sizeof(z_text));

    AsciiPlyTokenReplacement replacements[3] = {
        {header.x_index, x_text, strlen(x_text)},
        {header.y_index, y_text, strlen(y_text)},
        {header.z_index, z_text, strlen(z_text)},
    };
    sort_replacements(replacements, 3);

    if (ascii_ply_writer_write_vertex_with_replacements(&writer, &vertex, replacements, 3) != 0) {
      goto cleanup;
    }

    processed++;
    ascii_ply_progress_logger_maybe_log(&progress, processed);
  }

  if (processed != header.vertex_count) {
    fprintf(
        stderr,
        "Rotated point count mismatch: expected %" PRIu64 ", wrote %" PRIu64 ".\n",
        header.vertex_count,
        processed);
    goto cleanup;
  }

  if (ascii_ply_writer_copy_remaining_from_reader(&writer, &reader) != 0) {
    goto cleanup;
  }

  rc = 0;

cleanup:
  if (ascii_ply_writer_close(&writer) != 0) {
    rc = -1;
  }
  if (ascii_ply_reader_close(&reader) != 0) {
    rc = -1;
  }
  ascii_ply_free_header(&header);

  if (rc == 0) {
    double total_seconds = ascii_ply_now_seconds() - started_at;
    char duration[64];
    printf(
        "Rotate time: %s\n",
        ascii_ply_format_duration(total_seconds, duration, sizeof(duration)));
    printf("Rotated %" PRIu64 " points to %s\n", processed, options->output_path);
  }

  return rc;
}
