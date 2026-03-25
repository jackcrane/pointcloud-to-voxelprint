#ifndef ASCII_PLY_H
#define ASCII_PLY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
  char **lines;
  size_t line_count;
  size_t vertex_element_line_index;
  uint64_t vertex_count;
  size_t property_count;
  size_t x_index;
  size_t y_index;
  size_t z_index;
  bool has_x;
  bool has_y;
  bool has_z;
  bool has_additional_elements;
  long long header_end_offset;
} AsciiPlyHeader;

typedef struct {
  const char *line;
  size_t line_length;
  size_t token_count;
  const size_t *token_offsets;
  const size_t *token_lengths;
} AsciiPlyVertexLine;

typedef struct {
  size_t token_index;
  const char *replacement;
  size_t replacement_length;
} AsciiPlyTokenReplacement;

typedef struct {
  FILE *fp;
  char *line;
  size_t line_capacity;
  size_t *token_offsets;
  size_t *token_lengths;
  size_t max_tokens;
  const AsciiPlyHeader *header;
  uint64_t vertices_read;
} AsciiPlyReader;

typedef struct {
  FILE *fp;
  unsigned char *buffer;
  size_t capacity;
  size_t used;
  uint64_t written_vertices;
} AsciiPlyWriter;

void ascii_ply_free_header(AsciiPlyHeader *header);
int ascii_ply_read_header(const char *path, AsciiPlyHeader *header_out);

int ascii_ply_reader_open(
    AsciiPlyReader *reader,
    const char *path,
    const AsciiPlyHeader *header);
int ascii_ply_reader_next_vertex(
    AsciiPlyReader *reader,
    AsciiPlyVertexLine *vertex_out);
int ascii_ply_reader_close(AsciiPlyReader *reader);
bool ascii_ply_parse_token_double(
    const AsciiPlyVertexLine *vertex,
    size_t token_index,
    double *value_out);

int ascii_ply_writer_open(
    AsciiPlyWriter *writer,
    const char *path,
    const AsciiPlyHeader *header,
    uint64_t vertex_count);
int ascii_ply_writer_write_vertex_line(
    AsciiPlyWriter *writer,
    const AsciiPlyVertexLine *vertex);
int ascii_ply_writer_write_vertex_with_replacements(
    AsciiPlyWriter *writer,
    const AsciiPlyVertexLine *vertex,
    const AsciiPlyTokenReplacement *replacements,
    size_t replacement_count);
int ascii_ply_writer_copy_remaining_from_reader(
    AsciiPlyWriter *writer,
    AsciiPlyReader *reader);
int ascii_ply_writer_close(AsciiPlyWriter *writer);

#endif
