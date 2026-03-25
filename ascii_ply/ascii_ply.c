#include "ascii_ply.h"

#include "ascii_ply_support.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static int ascii_ply_append_header_line(AsciiPlyHeader *header, const char *line) {
  char **next = realloc(header->lines, (header->line_count + 1) * sizeof(*header->lines));
  if (next == NULL) {
    fprintf(stderr, "Failed to allocate PLY header lines.\n");
    return -1;
  }

  header->lines = next;
  header->lines[header->line_count] = strdup(line);
  if (header->lines[header->line_count] == NULL) {
    fprintf(stderr, "Failed to duplicate PLY header line.\n");
    return -1;
  }

  header->line_count++;
  return 0;
}

static bool ascii_ply_parse_element_line(
    const char *line,
    char *name_out,
    size_t name_out_size,
    uint64_t *count_out) {
  (void)name_out_size;
  char count_buffer[64];
  if (sscanf(line, "element %63s %63s", name_out, count_buffer) != 2) {
    return false;
  }

  return ascii_ply_parse_uint64_str(count_buffer, count_out);
}

static bool ascii_ply_parse_property_line(
    const char *line,
    char *type_out,
    size_t type_out_size,
    char *name_out,
    size_t name_out_size) {
  (void)type_out_size;
  (void)name_out_size;
  if (sscanf(line, "property %63s %63s", type_out, name_out) != 2) {
    return false;
  }

  if (strcmp(type_out, "list") == 0) {
    return false;
  }

  return true;
}

static int ascii_ply_writer_flush(AsciiPlyWriter *writer) {
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

static int ascii_ply_writer_append_bytes(
    AsciiPlyWriter *writer,
    const unsigned char *bytes,
    size_t length) {
  if (length == 0) {
    return 0;
  }

  if (length > writer->capacity) {
    if (ascii_ply_writer_flush(writer) != 0) {
      return -1;
    }
    if (fwrite(bytes, 1, length, writer->fp) != length) {
      fprintf(stderr, "Failed to write PLY data: %s\n", strerror(errno));
      return -1;
    }
    return 0;
  }

  if (writer->used + length > writer->capacity && ascii_ply_writer_flush(writer) != 0) {
    return -1;
  }

  memcpy(writer->buffer + writer->used, bytes, length);
  writer->used += length;
  return 0;
}

static bool ascii_ply_line_is_ignorable(const char *line, size_t line_length) {
  size_t index = 0;
  while (index < line_length && isspace((unsigned char)line[index])) {
    index++;
  }

  if (index == line_length) {
    return true;
  }

  return ascii_ply_starts_with(line + index, "comment ");
}

static int ascii_ply_parse_vertex_tokens(
    AsciiPlyReader *reader,
    size_t line_length,
    size_t *token_count_out) {
  size_t token_count = 0;
  size_t cursor = 0;
  while (cursor < line_length) {
    while (cursor < line_length && isspace((unsigned char)reader->line[cursor])) {
      cursor++;
    }
    if (cursor >= line_length) {
      break;
    }

    if (token_count >= reader->max_tokens) {
      fprintf(stderr, "Vertex line has more fields than expected.\n");
      return -1;
    }

    size_t start = cursor;
    while (cursor < line_length && !isspace((unsigned char)reader->line[cursor])) {
      cursor++;
    }

    reader->token_offsets[token_count] = start;
    reader->token_lengths[token_count] = cursor - start;
    token_count++;
  }

  *token_count_out = token_count;
  return 0;
}

void ascii_ply_free_header(AsciiPlyHeader *header) {
  if (header == NULL) {
    return;
  }

  for (size_t i = 0; i < header->line_count; ++i) {
    free(header->lines[i]);
  }
  free(header->lines);
  memset(header, 0, sizeof(*header));
}

int ascii_ply_read_header(const char *path, AsciiPlyHeader *header_out) {
  memset(header_out, 0, sizeof(*header_out));

  FILE *fp = fopen(path, "rb");
  if (fp == NULL) {
    fprintf(stderr, "Failed to open '%s': %s\n", path, strerror(errno));
    return -1;
  }

  bool saw_ply = false;
  bool saw_format = false;
  bool saw_end_header = false;
  bool saw_vertex_element = false;
  bool in_vertex_element = false;
  char *line = NULL;
  size_t line_capacity = 0;
  int rc = -1;

  while (getline(&line, &line_capacity, fp) != -1) {
    header_out->header_end_offset = ftello(fp);
    if (ascii_ply_append_header_line(header_out, line) != 0) {
      goto cleanup;
    }

    char *cursor = line;
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
      cursor++;
    }

    char *end = cursor + strlen(cursor);
    while (end > cursor && isspace((unsigned char)end[-1])) {
      end--;
    }
    *end = '\0';

    if (!saw_ply) {
      if (strcmp(cursor, "ply") != 0) {
        fprintf(stderr, "Not an ASCII PLY file.\n");
        goto cleanup;
      }
      saw_ply = true;
      continue;
    }

    if (strcmp(cursor, "end_header") == 0) {
      saw_end_header = true;
      rc = 0;
      goto cleanup;
    }

    if (ascii_ply_starts_with(cursor, "comment ") ||
        ascii_ply_starts_with(cursor, "obj_info ")) {
      continue;
    }

    if (ascii_ply_starts_with(cursor, "format ")) {
      char format_name[64];
      char format_version[64];
      if (sscanf(cursor, "format %63s %63s", format_name, format_version) != 2) {
        fprintf(stderr, "Invalid PLY format line.\n");
        goto cleanup;
      }

      saw_format = true;
      if (strcmp(format_name, "ascii") != 0) {
        fprintf(stderr, "Unsupported PLY format: %s %s\n", format_name, format_version);
        goto cleanup;
      }
      continue;
    }

    if (ascii_ply_starts_with(cursor, "element ")) {
      char element_name[64];
      uint64_t element_count = 0;
      if (!ascii_ply_parse_element_line(
              cursor,
              element_name,
              sizeof(element_name),
              &element_count)) {
        fprintf(stderr, "Invalid PLY element line.\n");
        goto cleanup;
      }

      in_vertex_element = strcmp(element_name, "vertex") == 0;
      if (in_vertex_element) {
        if (saw_vertex_element) {
          fprintf(stderr, "Multiple vertex elements are not supported.\n");
          goto cleanup;
        }
        saw_vertex_element = true;
        header_out->vertex_count = element_count;
        header_out->vertex_element_line_index = header_out->line_count - 1;
      } else if (element_count > 0) {
        header_out->has_additional_elements = true;
      }
      continue;
    }

    if (in_vertex_element && ascii_ply_starts_with(cursor, "property ")) {
      char type_name[64];
      char property_name[64];
      if (!ascii_ply_parse_property_line(
              cursor,
              type_name,
              sizeof(type_name),
              property_name,
              sizeof(property_name))) {
        fprintf(stderr, "Unsupported vertex property line: %s\n", cursor);
        goto cleanup;
      }

      size_t property_index = header_out->property_count;
      if (strcmp(property_name, "x") == 0) {
        header_out->has_x = true;
        header_out->x_index = property_index;
      } else if (strcmp(property_name, "y") == 0) {
        header_out->has_y = true;
        header_out->y_index = property_index;
      } else if (strcmp(property_name, "z") == 0) {
        header_out->has_z = true;
        header_out->z_index = property_index;
      }

      header_out->property_count++;
    }
  }

  if (!saw_end_header) {
    fprintf(stderr, "Invalid PLY: missing end_header.\n");
  }

cleanup:
  free(line);
  fclose(fp);

  if (rc == 0) {
    if (!saw_format) {
      fprintf(stderr, "Missing PLY format line.\n");
      rc = -1;
    } else if (!saw_vertex_element) {
      fprintf(stderr, "Missing vertex element in PLY header.\n");
      rc = -1;
    } else if (!header_out->has_x || !header_out->has_y || !header_out->has_z) {
      fprintf(stderr, "PLY vertex must include x, y, and z properties.\n");
      rc = -1;
    } else if (header_out->property_count == 0) {
      fprintf(stderr, "PLY vertex element has no properties.\n");
      rc = -1;
    }
  }

  if (rc != 0) {
    ascii_ply_free_header(header_out);
  }
  return rc;
}

int ascii_ply_reader_open(
    AsciiPlyReader *reader,
    const char *path,
    const AsciiPlyHeader *header) {
  memset(reader, 0, sizeof(*reader));
  reader->header = header;
  reader->max_tokens = header->property_count;

  reader->fp = fopen(path, "rb");
  if (reader->fp == NULL) {
    fprintf(stderr, "Failed to open '%s': %s\n", path, strerror(errno));
    return -1;
  }

  if (fseeko(reader->fp, (off_t)header->header_end_offset, SEEK_SET) != 0) {
    fprintf(stderr, "Failed to seek PLY payload: %s\n", strerror(errno));
    fclose(reader->fp);
    reader->fp = NULL;
    return -1;
  }

  reader->token_offsets = malloc(reader->max_tokens * sizeof(*reader->token_offsets));
  reader->token_lengths = malloc(reader->max_tokens * sizeof(*reader->token_lengths));
  if (reader->token_offsets == NULL || reader->token_lengths == NULL) {
    fprintf(stderr, "Failed to allocate PLY token buffers.\n");
    ascii_ply_reader_close(reader);
    return -1;
  }

  return 0;
}

int ascii_ply_reader_next_vertex(
    AsciiPlyReader *reader,
    AsciiPlyVertexLine *vertex_out) {
  while (reader->vertices_read < reader->header->vertex_count) {
    ssize_t line_length = getline(&reader->line, &reader->line_capacity, reader->fp);
    if (line_length < 0) {
      fprintf(stderr, "Unexpected end of ASCII PLY vertex data.\n");
      return -1;
    }

    if (ascii_ply_line_is_ignorable(reader->line, (size_t)line_length)) {
      continue;
    }

    size_t token_count = 0;
    if (ascii_ply_parse_vertex_tokens(reader, (size_t)line_length, &token_count) != 0) {
      return -1;
    }

    if (token_count != reader->header->property_count) {
      fprintf(
          stderr,
          "Vertex field count mismatch: expected %zu, found %zu.\n",
          reader->header->property_count,
          token_count);
      return -1;
    }

    reader->vertices_read++;
    vertex_out->line = reader->line;
    vertex_out->line_length = (size_t)line_length;
    vertex_out->token_count = token_count;
    vertex_out->token_offsets = reader->token_offsets;
    vertex_out->token_lengths = reader->token_lengths;
    return 1;
  }

  return 0;
}

int ascii_ply_reader_close(AsciiPlyReader *reader) {
  int rc = 0;

  if (reader->fp != NULL && fclose(reader->fp) != 0) {
    fprintf(stderr, "Failed to close PLY reader: %s\n", strerror(errno));
    rc = -1;
  }

  free(reader->line);
  free(reader->token_offsets);
  free(reader->token_lengths);
  memset(reader, 0, sizeof(*reader));
  return rc;
}

bool ascii_ply_parse_token_double(
    const AsciiPlyVertexLine *vertex,
    size_t token_index,
    double *value_out) {
  if (token_index >= vertex->token_count) {
    return false;
  }

  const char *start = vertex->line + vertex->token_offsets[token_index];
  char *end = NULL;
  errno = 0;
  double value = strtod(start, &end);
  if (errno != 0 || end != start + vertex->token_lengths[token_index]) {
    return false;
  }

  *value_out = value;
  return true;
}

int ascii_ply_writer_open(
    AsciiPlyWriter *writer,
    const char *path,
    const AsciiPlyHeader *header,
    uint64_t vertex_count) {
  memset(writer, 0, sizeof(*writer));

  if (ascii_ply_ensure_parent_dir(path) != 0) {
    return -1;
  }

  writer->fp = fopen(path, "wb");
  if (writer->fp == NULL) {
    fprintf(stderr, "Failed to open '%s' for writing: %s\n", path, strerror(errno));
    return -1;
  }

  setvbuf(writer->fp, NULL, _IOFBF, ASCII_PLY_STREAM_BUFFER_BYTES);

  writer->capacity = ASCII_PLY_STREAM_BUFFER_BYTES;
  writer->buffer = malloc(writer->capacity);
  if (writer->buffer == NULL) {
    fprintf(stderr, "Failed to allocate PLY output buffer.\n");
    ascii_ply_writer_close(writer);
    return -1;
  }

  for (size_t i = 0; i < header->line_count; ++i) {
    if (i == header->vertex_element_line_index) {
      char vertex_line[128];
      int vertex_line_length = snprintf(
          vertex_line,
          sizeof(vertex_line),
          "element vertex %" PRIu64 "\n",
          vertex_count);
      if (vertex_line_length < 0 || (size_t)vertex_line_length >= sizeof(vertex_line)) {
        fprintf(stderr, "Failed to format PLY vertex count line.\n");
        ascii_ply_writer_close(writer);
        return -1;
      }

      if (ascii_ply_writer_append_bytes(
              writer,
              (const unsigned char *)vertex_line,
              (size_t)vertex_line_length) != 0) {
        ascii_ply_writer_close(writer);
        return -1;
      }
      continue;
    }

    size_t line_length = strlen(header->lines[i]);
    if (ascii_ply_writer_append_bytes(
            writer,
            (const unsigned char *)header->lines[i],
            line_length) != 0) {
      ascii_ply_writer_close(writer);
      return -1;
    }
  }

  return 0;
}

int ascii_ply_writer_write_vertex_line(
    AsciiPlyWriter *writer,
    const AsciiPlyVertexLine *vertex) {
  if (ascii_ply_writer_append_bytes(
          writer,
          (const unsigned char *)vertex->line,
          vertex->line_length) != 0) {
    return -1;
  }

  if (vertex->line_length == 0 || vertex->line[vertex->line_length - 1] != '\n') {
    static const unsigned char newline = '\n';
    if (ascii_ply_writer_append_bytes(writer, &newline, 1) != 0) {
      return -1;
    }
  }

  writer->written_vertices++;
  return 0;
}

int ascii_ply_writer_write_vertex_with_replacements(
    AsciiPlyWriter *writer,
    const AsciiPlyVertexLine *vertex,
    const AsciiPlyTokenReplacement *replacements,
    size_t replacement_count) {
  size_t cursor = 0;

  for (size_t i = 0; i < replacement_count; ++i) {
    if (replacements[i].token_index >= vertex->token_count) {
      fprintf(stderr, "Replacement token index out of range.\n");
      return -1;
    }
    if (i > 0 && replacements[i - 1].token_index >= replacements[i].token_index) {
      fprintf(stderr, "Replacement token indices must be strictly increasing.\n");
      return -1;
    }

    size_t token_start = vertex->token_offsets[replacements[i].token_index];
    size_t token_end = token_start + vertex->token_lengths[replacements[i].token_index];

    if (ascii_ply_writer_append_bytes(
            writer,
            (const unsigned char *)vertex->line + cursor,
            token_start - cursor) != 0) {
      return -1;
    }
    if (ascii_ply_writer_append_bytes(
            writer,
            (const unsigned char *)replacements[i].replacement,
            replacements[i].replacement_length) != 0) {
      return -1;
    }

    cursor = token_end;
  }

  if (ascii_ply_writer_append_bytes(
          writer,
          (const unsigned char *)vertex->line + cursor,
          vertex->line_length - cursor) != 0) {
    return -1;
  }

  if (vertex->line_length == 0 || vertex->line[vertex->line_length - 1] != '\n') {
    static const unsigned char newline = '\n';
    if (ascii_ply_writer_append_bytes(writer, &newline, 1) != 0) {
      return -1;
    }
  }

  writer->written_vertices++;
  return 0;
}

int ascii_ply_writer_copy_remaining_from_reader(
    AsciiPlyWriter *writer,
    AsciiPlyReader *reader) {
  unsigned char *buffer = malloc(ASCII_PLY_COPY_BUFFER_BYTES);
  if (buffer == NULL) {
    fprintf(stderr, "Failed to allocate payload copy buffer.\n");
    return -1;
  }

  int rc = 0;
  while (true) {
    size_t bytes_read = fread(buffer, 1, ASCII_PLY_COPY_BUFFER_BYTES, reader->fp);
    if (bytes_read > 0) {
      if (ascii_ply_writer_append_bytes(writer, buffer, bytes_read) != 0) {
        rc = -1;
        break;
      }
    }

    if (bytes_read < ASCII_PLY_COPY_BUFFER_BYTES) {
      if (ferror(reader->fp)) {
        fprintf(stderr, "Failed to read trailing PLY payload: %s\n", strerror(errno));
        rc = -1;
      }
      break;
    }
  }

  free(buffer);
  return rc;
}

int ascii_ply_writer_close(AsciiPlyWriter *writer) {
  int rc = 0;

  if (writer->fp != NULL) {
    if (ascii_ply_writer_flush(writer) != 0) {
      rc = -1;
    }
    if (fclose(writer->fp) != 0) {
      fprintf(stderr, "Failed to close PLY output: %s\n", strerror(errno));
      rc = -1;
    }
  }

  free(writer->buffer);
  memset(writer, 0, sizeof(*writer));
  return rc;
}
