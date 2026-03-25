#ifndef TRANSLATE_PLY_H
#define TRANSLATE_PLY_H

#include "translate_common.h"

#include <stdint.h>
#include <stdio.h>

typedef struct {
  FILE *fp;
  unsigned char *buffer;
  size_t capacity;
  size_t used;
  uint64_t written_points;
} PlyWriter;

int ply_writer_open(PlyWriter *writer, const char *path, uint64_t point_count);
int ply_writer_write_vertex(
    PlyWriter *writer,
    double x,
    double y,
    double z,
    uint16_t r,
    uint16_t g,
    uint16_t b);
int ply_writer_close(PlyWriter *writer);

#endif
