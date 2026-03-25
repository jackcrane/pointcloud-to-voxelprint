#include "quantize_ply.h"

#include "quantize_support.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
  int x;
  int y;
  int z;
  int r;
  int g;
  int b;
  int a;
  PlyType r_type;
  PlyType g_type;
  PlyType b_type;
  PlyType a_type;
} AsciiFieldIndices;

typedef struct {
  bool present;
  size_t offset;
  PlyType type;
} BinaryField;

typedef struct {
  size_t stride;
  BinaryField x;
  BinaryField y;
  BinaryField z;
  BinaryField r;
  BinaryField g;
  BinaryField b;
  BinaryField a;
} BinaryLayout;

static PlyType parse_ply_type(const char *name) {
  if (strcasecmp(name, "float") == 0 || strcasecmp(name, "float32") == 0) {
    return PLY_TYPE_FLOAT32;
  }
  if (strcasecmp(name, "double") == 0 || strcasecmp(name, "float64") == 0) {
    return PLY_TYPE_FLOAT64;
  }
  if (strcasecmp(name, "uchar") == 0 || strcasecmp(name, "uint8") == 0) {
    return PLY_TYPE_U8;
  }
  if (strcasecmp(name, "char") == 0 || strcasecmp(name, "int8") == 0) {
    return PLY_TYPE_I8;
  }
  if (strcasecmp(name, "ushort") == 0 || strcasecmp(name, "uint16") == 0) {
    return PLY_TYPE_U16;
  }
  if (strcasecmp(name, "short") == 0 || strcasecmp(name, "int16") == 0) {
    return PLY_TYPE_I16;
  }
  if (strcasecmp(name, "uint") == 0 || strcasecmp(name, "uint32") == 0) {
    return PLY_TYPE_U32;
  }
  if (strcasecmp(name, "int") == 0 || strcasecmp(name, "int32") == 0) {
    return PLY_TYPE_I32;
  }
  return PLY_TYPE_UNKNOWN;
}

static size_t ply_type_size(PlyType type) {
  switch (type) {
    case PLY_TYPE_FLOAT32:
    case PLY_TYPE_U32:
    case PLY_TYPE_I32:
    case PLY_TYPE_UNKNOWN:
      return 4;
    case PLY_TYPE_FLOAT64:
      return 8;
    case PLY_TYPE_U16:
    case PLY_TYPE_I16:
      return 2;
    case PLY_TYPE_U8:
    case PLY_TYPE_I8:
      return 1;
  }

  return 4;
}

static bool ply_type_is_normalized_float(PlyType type) {
  return type == PLY_TYPE_FLOAT32 || type == PLY_TYPE_FLOAT64;
}

static bool ply_type_has_unsigned_max(PlyType type, double *max_out) {
  switch (type) {
    case PLY_TYPE_U8:
      *max_out = 255.0;
      return true;
    case PLY_TYPE_U16:
      *max_out = 65535.0;
      return true;
    case PLY_TYPE_U32:
      *max_out = 4294967295.0;
      return true;
    default:
      return false;
  }
}

static int normalize_color_value(double value, PlyType type) {
  if (!isfinite(value)) {
    return -1;
  }

  if (type != PLY_TYPE_UNKNOWN) {
    double unsigned_max = 0.0;
    if (ply_type_is_normalized_float(type)) {
      if (value >= 0.0 && value <= 1.0) {
        return (int)llround(value * 255.0);
      }
      long long rounded = llround(value);
      if (rounded < 0) {
        return 0;
      }
      if (rounded > 255) {
        return 255;
      }
      return (int)rounded;
    }

    if (ply_type_has_unsigned_max(type, &unsigned_max)) {
      double clamped = value;
      if (clamped < 0.0) {
        clamped = 0.0;
      }
      if (clamped > unsigned_max) {
        clamped = unsigned_max;
      }
      if (unsigned_max <= 255.0) {
        return (int)llround(clamped);
      }
      return (int)llround((clamped * 255.0) / unsigned_max);
    }
  }

  if (value >= 0.0 && value <= 1.0) {
    return (int)llround(value * 255.0);
  }

  long long rounded = llround(value);
  if (rounded < 0) {
    return 0;
  }
  if (rounded > 255) {
    return 255;
  }
  return (int)rounded;
}

static int find_property_index(
    const PlyHeader *header,
    const char *primary_name,
    const char *fallback_name,
    PlyType *type_out) {
  for (size_t i = 0; i < header->property_count; ++i) {
    const char *name = header->properties[i].name;
    if (strcmp(name, primary_name) == 0 ||
        (fallback_name != NULL && strcmp(name, fallback_name) == 0)) {
      if (type_out != NULL) {
        *type_out = header->properties[i].type;
      }
      return (int)i;
    }
  }

  if (type_out != NULL) {
    *type_out = PLY_TYPE_UNKNOWN;
  }
  return -1;
}

static AsciiFieldIndices build_ascii_indices(const PlyHeader *header) {
  AsciiFieldIndices indices;
  indices.x = find_property_index(header, "x", NULL, NULL);
  indices.y = find_property_index(header, "y", NULL, NULL);
  indices.z = find_property_index(header, "z", NULL, NULL);
  indices.r = find_property_index(header, "red", "r", &indices.r_type);
  indices.g = find_property_index(header, "green", "g", &indices.g_type);
  indices.b = find_property_index(header, "blue", "b", &indices.b_type);
  indices.a = find_property_index(header, "alpha", "a", &indices.a_type);
  return indices;
}

static BinaryLayout build_binary_layout(const PlyHeader *header) {
  BinaryLayout layout;
  memset(&layout, 0, sizeof(layout));

  size_t offset = 0;
  for (size_t i = 0; i < header->property_count; ++i) {
    const PlyProperty *property = &header->properties[i];
    BinaryField field = {true, offset, property->type};

    if (strcmp(property->name, "x") == 0) {
      layout.x = field;
    } else if (strcmp(property->name, "y") == 0) {
      layout.y = field;
    } else if (strcmp(property->name, "z") == 0) {
      layout.z = field;
    } else if (strcmp(property->name, "red") == 0 || strcmp(property->name, "r") == 0) {
      layout.r = field;
    } else if (strcmp(property->name, "green") == 0 || strcmp(property->name, "g") == 0) {
      layout.g = field;
    } else if (strcmp(property->name, "blue") == 0 || strcmp(property->name, "b") == 0) {
      layout.b = field;
    } else if (strcmp(property->name, "alpha") == 0 || strcmp(property->name, "a") == 0) {
      layout.a = field;
    }

    offset += ply_type_size(property->type);
  }

  layout.stride = offset;
  return layout;
}

static double read_binary_value(const unsigned char *ptr, PlyType type) {
  switch (type) {
    case PLY_TYPE_FLOAT32: {
      union {
        uint32_t u;
        float f;
      } value;
      value.u = read_u32_le(ptr);
      return (double)value.f;
    }
    case PLY_TYPE_FLOAT64: {
      union {
        uint64_t u;
        double d;
      } value;
      value.u = read_u64_le(ptr);
      return value.d;
    }
    case PLY_TYPE_U8:
      return (double)ptr[0];
    case PLY_TYPE_I8:
      return (double)(int8_t)ptr[0];
    case PLY_TYPE_U16:
      return (double)read_u16_le(ptr);
    case PLY_TYPE_I16:
      return (double)read_i16_le(ptr);
    case PLY_TYPE_U32:
      return (double)read_u32_le(ptr);
    case PLY_TYPE_I32:
      return (double)read_i32_le(ptr);
    case PLY_TYPE_UNKNOWN:
    default: {
      union {
        uint32_t u;
        float f;
      } value;
      value.u = read_u32_le(ptr);
      return (double)value.f;
    }
  }
}

static uint8_t read_binary_color(
    const unsigned char *record,
    const BinaryField *field,
    uint8_t default_value) {
  if (!field->present) {
    return default_value;
  }

  int normalized = normalize_color_value(
      read_binary_value(record + field->offset, field->type),
      field->type);
  if (normalized < 0) {
    return default_value;
  }
  return (uint8_t)normalized;
}

static int append_property(PlyHeader *header, const char *name, PlyType type) {
  PlyProperty *next = realloc(
      header->properties,
      (header->property_count + 1) * sizeof(*header->properties));
  if (next == NULL) {
    fprintf(stderr, "Failed to allocate PLY property list.\n");
    return -1;
  }

  header->properties = next;
  header->properties[header->property_count].name = strdup(name);
  header->properties[header->property_count].type = type;
  if (header->properties[header->property_count].name == NULL) {
    fprintf(stderr, "Failed to allocate PLY property name.\n");
    return -1;
  }

  header->property_count++;
  return 0;
}

void free_ply_header(PlyHeader *header) {
  for (size_t i = 0; i < header->property_count; ++i) {
    free(header->properties[i].name);
  }
  free(header->properties);
  memset(header, 0, sizeof(*header));
}

int read_ply_header(const char *file_path, PlyHeader *header_out) {
  memset(header_out, 0, sizeof(*header_out));

  FILE *fp = fopen(file_path, "rb");
  if (fp == NULL) {
    fprintf(stderr, "Failed to open '%s': %s\n", file_path, strerror(errno));
    return -1;
  }

  bool saw_ply = false;
  bool saw_format = false;
  bool in_vertex_element = false;
  char *line = NULL;
  size_t line_capacity = 0;
  int rc = -1;

  while (getline(&line, &line_capacity, fp) != -1) {
    header_out->header_end_offset = ftello(fp);
    char *trimmed = trim_in_place(line);

    if (!saw_ply) {
      if (strcasecmp(trimmed, "ply") != 0) {
        fprintf(stderr, "Not a PLY file.\n");
        goto cleanup;
      }
      saw_ply = true;
      continue;
    }

    if (strcmp(trimmed, "end_header") == 0) {
      rc = 0;
      goto cleanup;
    }

    if (starts_with(trimmed, "comment ") || starts_with(trimmed, "obj_info ")) {
      continue;
    }

    if (starts_with(trimmed, "format ")) {
      char format_name[64];
      char format_version[64];
      if (sscanf(trimmed, "format %63s %63s", format_name, format_version) != 2) {
        fprintf(stderr, "Invalid PLY format line.\n");
        goto cleanup;
      }

      saw_format = true;
      if (strcmp(format_name, "ascii") == 0) {
        header_out->format = PLY_FORMAT_ASCII;
      } else if (strcmp(format_name, "binary_little_endian") == 0) {
        header_out->format = PLY_FORMAT_BINARY_LE;
      } else {
        fprintf(stderr, "Unsupported PLY format: %s %s\n", format_name, format_version);
        goto cleanup;
      }
      continue;
    }

    if (starts_with(trimmed, "element ")) {
      char element_name[64];
      char count_text[64];
      if (sscanf(trimmed, "element %63s %63s", element_name, count_text) != 2) {
        fprintf(stderr, "Invalid PLY element line.\n");
        goto cleanup;
      }

      in_vertex_element = strcmp(element_name, "vertex") == 0;
      if (in_vertex_element && !parse_uint64_str(count_text, &header_out->vertex_count)) {
        fprintf(stderr, "Invalid vertex count in PLY header.\n");
        goto cleanup;
      }
      continue;
    }

    if (in_vertex_element && starts_with(trimmed, "property ")) {
      char kind[64];
      char type_name[64];
      char prop_name[64];

      if (sscanf(trimmed, "property %63s %63s %63s", kind, type_name, prop_name) == 3 &&
          strcmp(kind, "list") == 0) {
        fprintf(stderr, "Unsupported vertex list property in PLY header.\n");
        goto cleanup;
      }

      if (sscanf(trimmed, "property %63s %63s", type_name, prop_name) != 2) {
        fprintf(stderr, "Invalid PLY property line.\n");
        goto cleanup;
      }

      if (append_property(header_out, prop_name, parse_ply_type(type_name)) != 0) {
        goto cleanup;
      }
    }
  }

  fprintf(stderr, "Invalid PLY: missing end_header.\n");

cleanup:
  if (!saw_format && rc == 0) {
    fprintf(stderr, "Missing PLY format line.\n");
    rc = -1;
  }

  free(line);
  fclose(fp);

  if (rc != 0) {
    free_ply_header(header_out);
  }
  return rc;
}

static int parse_ascii_vertex_line(
    char *line,
    const PlyHeader *header,
    const AsciiFieldIndices *indices,
    Vertex *vertex_out) {
  vertex_out->x = NAN;
  vertex_out->y = NAN;
  vertex_out->z = NAN;
  vertex_out->r = 255;
  vertex_out->g = 255;
  vertex_out->b = 255;
  vertex_out->a = 255;

  size_t field_index = 0;
  char *cursor = line;
  while (*cursor != '\0' && field_index < header->property_count) {
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
      cursor++;
    }
    if (*cursor == '\0') {
      break;
    }

    errno = 0;
    char *end = NULL;
    double value = strtod(cursor, &end);
    if (end == cursor || errno == ERANGE) {
      value = NAN;
      while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
        cursor++;
      }
    } else {
      cursor = end;
    }

    if ((int)field_index == indices->x) {
      vertex_out->x = value;
    } else if ((int)field_index == indices->y) {
      vertex_out->y = value;
    } else if ((int)field_index == indices->z) {
      vertex_out->z = value;
    } else if ((int)field_index == indices->r) {
      int normalized = normalize_color_value(value, indices->r_type);
      if (normalized >= 0) {
        vertex_out->r = (uint8_t)normalized;
      }
    } else if ((int)field_index == indices->g) {
      int normalized = normalize_color_value(value, indices->g_type);
      if (normalized >= 0) {
        vertex_out->g = (uint8_t)normalized;
      }
    } else if ((int)field_index == indices->b) {
      int normalized = normalize_color_value(value, indices->b_type);
      if (normalized >= 0) {
        vertex_out->b = (uint8_t)normalized;
      }
    } else if ((int)field_index == indices->a) {
      int normalized = normalize_color_value(value, indices->a_type);
      if (normalized >= 0) {
        vertex_out->a = (uint8_t)normalized;
      }
    }

    field_index++;
  }

  return 0;
}

static int stream_ascii_vertices(
    const char *file_path,
    const PlyHeader *header,
    VertexVisitor visitor,
    void *ctx,
    ProgressLogger *progress,
    uint64_t *processed_out) {
  AsciiFieldIndices indices = build_ascii_indices(header);
  if (indices.x < 0 || indices.y < 0 || indices.z < 0) {
    fprintf(stderr, "PLY vertex must include x, y, and z.\n");
    return -1;
  }

  FILE *fp = fopen(file_path, "rb");
  if (fp == NULL) {
    fprintf(stderr, "Failed to open '%s': %s\n", file_path, strerror(errno));
    return -1;
  }
  if (fseeko(fp, (off_t)header->header_end_offset, SEEK_SET) != 0) {
    fprintf(stderr, "Failed to seek PLY payload: %s\n", strerror(errno));
    fclose(fp);
    return -1;
  }

  char *line = NULL;
  size_t line_capacity = 0;
  uint64_t processed = 0;
  int rc = 0;

  while (processed < header->vertex_count && getline(&line, &line_capacity, fp) != -1) {
    char *trimmed = trim_in_place(line);
    if (trimmed[0] == '\0' || starts_with(trimmed, "comment")) {
      continue;
    }

    Vertex vertex;
    if (parse_ascii_vertex_line(trimmed, header, &indices, &vertex) != 0) {
      rc = -1;
      break;
    }
    if (visitor(&vertex, ctx) != 0) {
      rc = -1;
      break;
    }

    processed++;
    progress_logger_maybe_log(progress, processed);
  }

  free(line);
  fclose(fp);
  *processed_out = processed;
  return rc;
}

static int stream_binary_vertices(
    const char *file_path,
    const PlyHeader *header,
    VertexVisitor visitor,
    void *ctx,
    ProgressLogger *progress,
    uint64_t *processed_out) {
  BinaryLayout layout = build_binary_layout(header);
  if (!layout.x.present || !layout.y.present || !layout.z.present) {
    fprintf(stderr, "PLY vertex must include x, y, and z.\n");
    return -1;
  }
  if (layout.stride == 0) {
    fprintf(stderr, "Invalid binary PLY vertex layout.\n");
    return -1;
  }

  FILE *fp = fopen(file_path, "rb");
  if (fp == NULL) {
    fprintf(stderr, "Failed to open '%s': %s\n", file_path, strerror(errno));
    return -1;
  }
  if (fseeko(fp, (off_t)header->header_end_offset, SEEK_SET) != 0) {
    fprintf(stderr, "Failed to seek PLY payload: %s\n", strerror(errno));
    fclose(fp);
    return -1;
  }

  unsigned char *buffer = malloc(STREAM_CHUNK_BYTES + layout.stride);
  if (buffer == NULL) {
    fprintf(stderr, "Failed to allocate binary stream buffer.\n");
    fclose(fp);
    return -1;
  }

  uint64_t processed = 0;
  size_t leftover = 0;
  int rc = 0;

  while (processed < header->vertex_count) {
    size_t bytes_read = fread(buffer + leftover, 1, STREAM_CHUNK_BYTES, fp);
    if (bytes_read == 0) {
      if (ferror(fp)) {
        fprintf(stderr, "Failed to read binary PLY payload.\n");
        rc = -1;
      }
      break;
    }

    size_t available = leftover + bytes_read;
    size_t complete_bytes = available - (available % layout.stride);

    for (size_t offset = 0;
         offset < complete_bytes && processed < header->vertex_count;
         offset += layout.stride) {
      const unsigned char *record = buffer + offset;
      Vertex vertex;
      vertex.x = read_binary_value(record + layout.x.offset, layout.x.type);
      vertex.y = read_binary_value(record + layout.y.offset, layout.y.type);
      vertex.z = read_binary_value(record + layout.z.offset, layout.z.type);
      vertex.r = read_binary_color(record, &layout.r, 255);
      vertex.g = read_binary_color(record, &layout.g, 255);
      vertex.b = read_binary_color(record, &layout.b, 255);
      vertex.a = read_binary_color(record, &layout.a, 255);

      if (visitor(&vertex, ctx) != 0) {
        rc = -1;
        break;
      }

      processed++;
      progress_logger_maybe_log(progress, processed);
    }

    if (rc != 0) {
      break;
    }

    leftover = available - complete_bytes;
    if (leftover > 0) {
      memmove(buffer, buffer + complete_bytes, leftover);
    }
  }

  if (rc == 0 && processed != header->vertex_count) {
    fprintf(
        stderr,
        "PLY binary parse error: expected %" PRIu64 " vertices, got %" PRIu64 "\n",
        header->vertex_count,
        processed);
    rc = -1;
  }

  free(buffer);
  fclose(fp);
  *processed_out = processed;
  return rc;
}

int for_each_vertex(
    const char *file_path,
    const PlyHeader *header,
    VertexVisitor visitor,
    void *ctx,
    ProgressLogger *progress,
    uint64_t *processed_out) {
  if (header->format == PLY_FORMAT_ASCII) {
    return stream_ascii_vertices(file_path, header, visitor, ctx, progress, processed_out);
  }
  return stream_binary_vertices(file_path, header, visitor, ctx, progress, processed_out);
}
