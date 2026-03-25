#include "translate_pipeline.h"

#include "translate_las.h"
#include "translate_ply.h"
#include "translate_support.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  PlyWriter *writer;
  bool failed;
} TranslateContext;

static int translate_point_to_ply(
    double x,
    double y,
    double z,
    uint16_t r,
    uint16_t g,
    uint16_t b,
    void *ctx) {
  TranslateContext *context = ctx;
  if (ply_writer_write_vertex(context->writer, x, y, z, r, g, b) != 0) {
    context->failed = true;
    return -1;
  }
  return 0;
}

int run_translate(const TranslateOptions *options) {
  if (options == NULL) {
    fprintf(stderr, "Missing translate options.\n");
    return -1;
  }

  double started_at = translate_now_seconds();

  LasHeader header;
  if (read_las_header(options->input_path, &header) != 0) {
    return -1;
  }

  if (translate_ensure_parent_dir(options->output_path) != 0) {
    return -1;
  }

  printf(
      "Input LAS version: %u.%u\n",
      (unsigned)header.version_major,
      (unsigned)header.version_minor);
  printf("Input point format: %u\n", (unsigned)header.point_format);
  printf("Input point record length: %u bytes\n", (unsigned)header.point_record_length);
  printf("Input point count: %" PRIu64 "\n", header.point_count);
  printf("Output PLY format: ascii with double xyz and ushort rgb\n");

  PlyWriter writer;
  if (ply_writer_open(&writer, options->output_path, header.point_count) != 0) {
    return -1;
  }

  TranslateContext context = {
      .writer = &writer,
      .failed = false,
  };

  TranslateProgressLogger progress;
  translate_progress_logger_init(
      &progress,
      "Translate",
      header.point_count,
      options->log_interval,
      started_at);

  uint64_t processed = 0;
  int rc = stream_las_points(
      options->input_path,
      &header,
      translate_point_to_ply,
      &context,
      &progress,
      &processed);

  if (ply_writer_close(&writer) != 0) {
    rc = -1;
  }

  if (rc != 0 || context.failed) {
    return -1;
  }

  if (processed != header.point_count) {
    fprintf(
        stderr,
        "Translated point count mismatch: expected %" PRIu64 ", wrote %" PRIu64 ".\n",
        header.point_count,
        processed);
    return -1;
  }

  double total_seconds = translate_now_seconds() - started_at;
  translate_print_stage_duration("Translate", total_seconds);
  printf(
      "Wrote %" PRIu64 " colorized points to %s\n",
      processed,
      options->output_path);
  translate_print_timing_summary(total_seconds, processed);

  return 0;
}
