#include "translate_ply.h"

#include "translate_support.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ply_writer_flush(PlyWriter *writer) {
  if (writer->used == 0) {
    return 0;
  }

  if (fwrite(writer->buffer, 1, writer->used, writer->fp) != writer->used) {
    fprintf(stderr, "Failed to write PLY data: %s\n", strerror(errno));
    return -1;
  }

  writer->used = 0;
  return 0;
}

static int ply_writer_append_bytes(
    PlyWriter *writer,
    const unsigned char *bytes,
    size_t length) {
  if (length > writer->capacity) {
    if (ply_writer_flush(writer) != 0) {
      return -1;
    }
    if (fwrite(bytes, 1, length, writer->fp) != length) {
      fprintf(stderr, "Failed to write PLY data: %s\n", strerror(errno));
      return -1;
    }
    return 0;
  }

  if (writer->used + length > writer->capacity && ply_writer_flush(writer) != 0) {
    return -1;
  }

  memcpy(writer->buffer + writer->used, bytes, length);
  writer->used += length;
  return 0;
}

int ply_writer_open(PlyWriter *writer, const char *path, uint64_t point_count) {
  memset(writer, 0, sizeof(*writer));

  writer->fp = fopen(path, "wb");
  if (writer->fp == NULL) {
    fprintf(stderr, "Failed to open '%s' for writing: %s\n", path, strerror(errno));
    return -1;
  }

  setvbuf(writer->fp, NULL, _IOFBF, TRANSLATE_STREAM_BUFFER_BYTES);

  size_t capacity = TRANSLATE_STREAM_BUFFER_BYTES;

  writer->buffer = malloc(capacity);
  if (writer->buffer == NULL) {
    fprintf(stderr, "Failed to allocate PLY output buffer.\n");
    fclose(writer->fp);
    writer->fp = NULL;
    return -1;
  }

  writer->capacity = capacity;

  char header[256];
  int header_len = snprintf(
      header,
      sizeof(header),
      "ply\n"
      "format ascii 1.0\n"
      "element vertex %" PRIu64 "\n"
      "property double x\n"
      "property double y\n"
      "property double z\n"
      "property ushort red\n"
      "property ushort green\n"
      "property ushort blue\n"
      "end_header\n",
      point_count);
  if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
    fprintf(stderr, "Failed to build PLY header.\n");
    free(writer->buffer);
    writer->buffer = NULL;
    fclose(writer->fp);
    writer->fp = NULL;
    return -1;
  }

  if (fwrite(header, 1, (size_t)header_len, writer->fp) != (size_t)header_len) {
    fprintf(stderr, "Failed to write PLY header: %s\n", strerror(errno));
    free(writer->buffer);
    writer->buffer = NULL;
    fclose(writer->fp);
    writer->fp = NULL;
    return -1;
  }

  return 0;
}

int ply_writer_write_vertex(
    PlyWriter *writer,
    double x,
    double y,
    double z,
    uint16_t r,
    uint16_t g,
    uint16_t b) {
  char x_text[32];
  char y_text[32];
  char z_text[32];
  char line[TRANSLATE_ASCII_LINE_BUFFER_BYTES];

  translate_format_ascii_double(x, x_text, sizeof(x_text));
  translate_format_ascii_double(y, y_text, sizeof(y_text));
  translate_format_ascii_double(z, z_text, sizeof(z_text));

  int line_len = snprintf(
      line,
      sizeof(line),
      "%s %s %s %u %u %u\n",
      x_text,
      y_text,
      z_text,
      (unsigned)r,
      (unsigned)g,
      (unsigned)b);
  if (line_len < 0 || (size_t)line_len >= sizeof(line)) {
    fprintf(stderr, "Failed to format ASCII PLY vertex line.\n");
    return -1;
  }

  if (ply_writer_append_bytes(writer, (const unsigned char *)line, (size_t)line_len) != 0) {
    return -1;
  }

  writer->written_points++;
  return 0;
}

int ply_writer_close(PlyWriter *writer) {
  int rc = 0;

  if (writer->fp != NULL) {
    if (ply_writer_flush(writer) != 0) {
      rc = -1;
    }
    if (fclose(writer->fp) != 0) {
      fprintf(stderr, "Failed to close PLY output: %s\n", strerror(errno));
      rc = -1;
    }
  }

  free(writer->buffer);
  writer->fp = NULL;
  writer->buffer = NULL;
  writer->capacity = 0;
  writer->used = 0;

  return rc;
}
