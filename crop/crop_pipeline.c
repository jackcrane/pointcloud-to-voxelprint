#include "crop_pipeline.h"

#include "../ascii_ply/ascii_ply.h"
#include "../ascii_ply/ascii_ply_support.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static bool point_is_inside_crop(const CropOptions *options, double x, double y, double z) {
  return x >= options->min_x &&
      x <= options->max_x &&
      y >= options->min_y &&
      y <= options->max_y &&
      z >= options->min_z &&
      z <= options->max_z;
}

static int count_cropped_vertices(
    const CropOptions *options,
    const AsciiPlyHeader *header,
    uint64_t *kept_count_out) {
  AsciiPlyReader reader;
  memset(&reader, 0, sizeof(reader));

  if (ascii_ply_reader_open(&reader, options->input_path, header) != 0) {
    return -1;
  }

  AsciiPlyProgressLogger progress;
  ascii_ply_progress_logger_init(
      &progress,
      "Crop count",
      header->vertex_count,
      options->log_interval,
      ascii_ply_now_seconds());

  uint64_t kept_count = 0;
  uint64_t processed = 0;
  int rc = -1;

  while (true) {
    AsciiPlyVertexLine vertex;
    int next_rc = ascii_ply_reader_next_vertex(&reader, &vertex);
    if (next_rc < 0) {
      goto cleanup;
    }
    if (next_rc == 0) {
      break;
    }

    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    if (!ascii_ply_parse_token_double(&vertex, header->x_index, &x) ||
        !ascii_ply_parse_token_double(&vertex, header->y_index, &y) ||
        !ascii_ply_parse_token_double(&vertex, header->z_index, &z)) {
      fprintf(stderr, "Failed to parse vertex coordinates.\n");
      goto cleanup;
    }

    if (point_is_inside_crop(options, x, y, z)) {
      kept_count++;
    }

    processed++;
    ascii_ply_progress_logger_maybe_log(&progress, processed);
  }

  if (processed != header->vertex_count) {
    fprintf(
        stderr,
        "Crop count mismatch: expected %" PRIu64 ", parsed %" PRIu64 ".\n",
        header->vertex_count,
        processed);
    goto cleanup;
  }

  *kept_count_out = kept_count;
  rc = 0;

cleanup:
  if (ascii_ply_reader_close(&reader) != 0) {
    rc = -1;
  }
  return rc;
}

static int write_cropped_vertices(
    const CropOptions *options,
    const AsciiPlyHeader *header,
    uint64_t kept_count,
    uint64_t *written_count_out) {
  AsciiPlyReader reader;
  AsciiPlyWriter writer;
  memset(&reader, 0, sizeof(reader));
  memset(&writer, 0, sizeof(writer));

  if (ascii_ply_reader_open(&reader, options->input_path, header) != 0) {
    return -1;
  }

  if (ascii_ply_writer_open(&writer, options->output_path, header, kept_count) != 0) {
    ascii_ply_reader_close(&reader);
    return -1;
  }

  AsciiPlyProgressLogger progress;
  ascii_ply_progress_logger_init(
      &progress,
      "Crop write",
      header->vertex_count,
      options->log_interval,
      ascii_ply_now_seconds());

  uint64_t processed = 0;
  int rc = -1;

  while (true) {
    AsciiPlyVertexLine vertex;
    int next_rc = ascii_ply_reader_next_vertex(&reader, &vertex);
    if (next_rc < 0) {
      goto cleanup;
    }
    if (next_rc == 0) {
      break;
    }

    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    if (!ascii_ply_parse_token_double(&vertex, header->x_index, &x) ||
        !ascii_ply_parse_token_double(&vertex, header->y_index, &y) ||
        !ascii_ply_parse_token_double(&vertex, header->z_index, &z)) {
      fprintf(stderr, "Failed to parse vertex coordinates.\n");
      goto cleanup;
    }

    if (point_is_inside_crop(options, x, y, z) &&
        ascii_ply_writer_write_vertex_line(&writer, &vertex) != 0) {
      goto cleanup;
    }

    processed++;
    ascii_ply_progress_logger_maybe_log(&progress, processed);
  }

  if (processed != header->vertex_count) {
    fprintf(
        stderr,
        "Crop write mismatch: expected %" PRIu64 ", parsed %" PRIu64 ".\n",
        header->vertex_count,
        processed);
    goto cleanup;
  }

  if (writer.written_vertices != kept_count) {
    fprintf(
        stderr,
        "Cropped point count mismatch: expected %" PRIu64 ", wrote %" PRIu64 ".\n",
        kept_count,
        writer.written_vertices);
    goto cleanup;
  }

  *written_count_out = writer.written_vertices;
  rc = 0;

cleanup:
  if (ascii_ply_writer_close(&writer) != 0) {
    rc = -1;
  }
  if (ascii_ply_reader_close(&reader) != 0) {
    rc = -1;
  }
  return rc;
}

int run_crop(const CropOptions *options) {
  if (options == NULL) {
    fprintf(stderr, "Missing crop options.\n");
    return -1;
  }

  int rc = -1;
  AsciiPlyHeader header;
  memset(&header, 0, sizeof(header));

  double started_at = ascii_ply_now_seconds();
  uint64_t kept_count = 0;
  uint64_t written_count = 0;

  if (ascii_ply_read_header(options->input_path, &header) != 0) {
    goto cleanup;
  }

  if (header.has_additional_elements) {
    fprintf(
        stderr,
        "Crop only supports vertex-only ASCII PLY files because filtering changes vertex indices.\n");
    goto cleanup;
  }

  printf("Input vertex count: %" PRIu64 "\n", header.vertex_count);

  if (count_cropped_vertices(options, &header, &kept_count) != 0) {
    goto cleanup;
  }

  printf("Output vertex count: %" PRIu64 "\n", kept_count);

  if (write_cropped_vertices(options, &header, kept_count, &written_count) != 0) {
    goto cleanup;
  }

  rc = 0;

cleanup:
  ascii_ply_free_header(&header);

  if (rc == 0) {
    double total_seconds = ascii_ply_now_seconds() - started_at;
    char duration[64];
    printf(
        "Crop time: %s\n",
        ascii_ply_format_duration(total_seconds, duration, sizeof(duration)));
    printf("Wrote %" PRIu64 " cropped points to %s\n", written_count, options->output_path);
  }

  return rc;
}
