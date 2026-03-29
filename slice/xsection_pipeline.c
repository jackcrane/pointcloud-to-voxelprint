#include "xsection_pipeline.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  uint32_t width;
  uint32_t height;
  size_t stride;
  uint8_t *pixels;
} Image;

typedef struct {
  uint32_t layer_index;
  char *path;
} LayerFile;

static int init_image(Image *image, uint32_t width, uint32_t height);
static void free_image(Image *image);
static void set_pixel(
    Image *image,
    uint32_t x,
    uint32_t y,
    uint8_t r,
    uint8_t g,
    uint8_t b,
    uint8_t a);
static int write_png(const char *path, const Image *image);
static int read_png(const char *path, Image *image_out);
static int collect_layer_files(const char *directory, LayerFile **layers_out, size_t *count_out);
static void free_layer_files(LayerFile *layers, size_t count);
static int compare_layer_files(const void *lhs, const void *rhs);
static bool parse_layer_file_name(const char *name, uint32_t *index_out);
static uint32_t read_u32_be(const uint8_t *bytes);
static void write_u32_be(uint8_t *out, uint32_t value);
static uint32_t crc32_bytes(const uint8_t *bytes, size_t length);
static uint32_t adler32_bytes(const uint8_t *bytes, size_t length);
static int append_chunk(
    FILE *file,
    const char type[4],
    const uint8_t *data,
    uint32_t length);
static int inflate_stored_blocks(
    const uint8_t *compressed,
    size_t compressed_size,
    uint8_t *raw_out,
    size_t raw_size);

int run_xsection(const XSectionOptions *options) {
  LayerFile *layers = NULL;
  size_t layer_count = 0;
  if (collect_layer_files(options->slice_options.output_dir, &layers, &layer_count) != 0) {
    return -1;
  }
  if (layer_count == 0) {
    fprintf(stderr, "No slice PNGs were found in %s\n", options->slice_options.output_dir);
    free_layer_files(layers, layer_count);
    return -1;
  }

  Image first_layer;
  memset(&first_layer, 0, sizeof(first_layer));
  if (read_png(layers[0].path, &first_layer) != 0) {
    free_layer_files(layers, layer_count);
    return -1;
  }

  const bool is_xz = options->plane == XSECTION_PLANE_XZ;
  const uint32_t source_limit = is_xz ? first_layer.height : first_layer.width;
  if (options->dist >= source_limit) {
    fprintf(
        stderr,
        "--dist=%" PRIu32 " is outside the source %s range 0..%" PRIu32 "\n",
        options->dist,
        is_xz ? "row" : "column",
        source_limit == 0 ? 0 : source_limit - 1);
    free_image(&first_layer);
    free_layer_files(layers, layer_count);
    return -1;
  }

  const uint32_t output_width = is_xz ? first_layer.width : first_layer.height;
  const uint32_t output_height = (uint32_t) layer_count;
  Image output;
  memset(&output, 0, sizeof(output));
  if (init_image(&output, output_width, output_height) != 0) {
    free_image(&first_layer);
    free_layer_files(layers, layer_count);
    return -1;
  }

  const double horizontal_dpi = options->slice_options.dpi;
  const double vertical_dpi = SLICE_NM_PER_INCH / options->slice_options.layer_height_nm;
  printf(
      "Cross-section plane=%s dist=%" PRIu32 "\n",
      is_xz ? "xz" : "yz",
      options->dist);
  printf(
      "Output pixels: %u x %u  @ horizontal %.3f dpi, vertical %.3f dpi\n",
      output_width,
      output_height,
      horizontal_dpi,
      vertical_dpi);

  for (size_t layer_pos = 0; layer_pos < layer_count; ++layer_pos) {
    Image layer;
    memset(&layer, 0, sizeof(layer));
    if (layer_pos == 0) {
      layer = first_layer;
      memset(&first_layer, 0, sizeof(first_layer));
    } else if (read_png(layers[layer_pos].path, &layer) != 0) {
      free_image(&output);
      free_image(&first_layer);
      free_layer_files(layers, layer_count);
      return -1;
    }

    if (layer.width != first_layer.width && layer_pos != 0) {
      fprintf(stderr, "Slice widths do not match: %s\n", layers[layer_pos].path);
      free_image(&layer);
      free_image(&output);
      free_image(&first_layer);
      free_layer_files(layers, layer_count);
      return -1;
    }
    if (layer.height != (is_xz ? source_limit : output_width)) {
      fprintf(stderr, "Slice heights do not match: %s\n", layers[layer_pos].path);
      free_image(&layer);
      free_image(&output);
      free_image(&first_layer);
      free_layer_files(layers, layer_count);
      return -1;
    }
    if ((!is_xz && layer.width != source_limit) || (is_xz && layer.width != output_width)) {
      fprintf(stderr, "Slice dimensions do not match: %s\n", layers[layer_pos].path);
      free_image(&layer);
      free_image(&output);
      free_image(&first_layer);
      free_layer_files(layers, layer_count);
      return -1;
    }

    const uint32_t out_y = output_height - 1u - (uint32_t) layer_pos;
    if (is_xz) {
      const size_t src_offset = (size_t) options->dist * layer.stride;
      const size_t dst_offset = (size_t) out_y * output.stride;
      memcpy(output.pixels + dst_offset, layer.pixels + src_offset, output.stride);
    } else {
      for (uint32_t src_y = 0; src_y < layer.height; ++src_y) {
        const size_t src_offset = (size_t) src_y * layer.stride + (size_t) options->dist * 4u;
        const uint8_t *pixel = layer.pixels + src_offset;
        set_pixel(&output, src_y, out_y, pixel[0], pixel[1], pixel[2], pixel[3]);
      }
    }

    free_image(&layer);
  }

  if (write_png(options->output_path, &output) != 0) {
    free_image(&output);
    free_layer_files(layers, layer_count);
    return -1;
  }

  printf("Wrote %s\n", options->output_path);
  free_image(&output);
  free_layer_files(layers, layer_count);
  return 0;
}

static int init_image(Image *image, uint32_t width, uint32_t height) {
  if (image == NULL || width == 0 || height == 0) {
    fprintf(stderr, "Invalid image dimensions.\n");
    return -1;
  }

  image->width = width;
  image->height = height;
  image->stride = (size_t) width * 4u;
  image->pixels = calloc(image->stride, height);
  if (image->pixels == NULL) {
    fprintf(stderr, "Failed to allocate image memory.\n");
    memset(image, 0, sizeof(*image));
    return -1;
  }

  return 0;
}

static void free_image(Image *image) {
  if (image == NULL) {
    return;
  }

  free(image->pixels);
  memset(image, 0, sizeof(*image));
}

static void set_pixel(
    Image *image,
    uint32_t x,
    uint32_t y,
    uint8_t r,
    uint8_t g,
    uint8_t b,
    uint8_t a) {
  if (image == NULL || image->pixels == NULL || x >= image->width || y >= image->height) {
    return;
  }

  const size_t offset = (size_t) y * image->stride + (size_t) x * 4u;
  image->pixels[offset] = r;
  image->pixels[offset + 1] = g;
  image->pixels[offset + 2] = b;
  image->pixels[offset + 3] = a;
}

static int collect_layer_files(const char *directory, LayerFile **layers_out, size_t *count_out) {
  DIR *dir = opendir(directory);
  if (dir == NULL) {
    fprintf(stderr, "Failed to open %s: %s\n", directory, strerror(errno));
    return -1;
  }

  LayerFile *layers = NULL;
  size_t count = 0;
  size_t capacity = 0;
  struct dirent *entry = NULL;
  while ((entry = readdir(dir)) != NULL) {
    uint32_t layer_index = 0;
    if (!parse_layer_file_name(entry->d_name, &layer_index)) {
      continue;
    }

    if (count == capacity) {
      const size_t new_capacity = capacity == 0 ? 64u : capacity * 2u;
      LayerFile *resized = realloc(layers, new_capacity * sizeof(*layers));
      if (resized == NULL) {
        fprintf(stderr, "Failed to allocate layer file list.\n");
        free_layer_files(layers, count);
        closedir(dir);
        return -1;
      }
      layers = resized;
      capacity = new_capacity;
    }

    const size_t path_len = strlen(directory) + strlen(entry->d_name) + 2u;
    layers[count].path = malloc(path_len);
    if (layers[count].path == NULL) {
      fprintf(stderr, "Failed to allocate a layer path.\n");
      free_layer_files(layers, count);
      closedir(dir);
      return -1;
    }
    snprintf(layers[count].path, path_len, "%s/%s", directory, entry->d_name);
    layers[count].layer_index = layer_index;
    ++count;
  }

  closedir(dir);
  qsort(layers, count, sizeof(*layers), compare_layer_files);
  *layers_out = layers;
  *count_out = count;
  return 0;
}

static void free_layer_files(LayerFile *layers, size_t count) {
  if (layers == NULL) {
    return;
  }

  for (size_t i = 0; i < count; ++i) {
    free(layers[i].path);
  }
  free(layers);
}

static int compare_layer_files(const void *lhs, const void *rhs) {
  const LayerFile *a = lhs;
  const LayerFile *b = rhs;
  if (a->layer_index < b->layer_index) {
    return -1;
  }
  if (a->layer_index > b->layer_index) {
    return 1;
  }
  return strcmp(a->path, b->path);
}

static bool parse_layer_file_name(const char *name, uint32_t *index_out) {
  if (strncmp(name, "out_", 4) != 0) {
    return false;
  }

  errno = 0;
  char *end = NULL;
  unsigned long parsed = strtoul(name + 4, &end, 10);
  if (errno != 0 || end == name + 4 || *end != '\0' && strcmp(end, ".png") != 0) {
    return false;
  }
  if (parsed > UINT32_MAX || strcmp(end, ".png") != 0) {
    return false;
  }

  *index_out = (uint32_t) parsed;
  return true;
}

static uint32_t read_u32_be(const uint8_t *bytes) {
  return ((uint32_t) bytes[0] << 24) |
         ((uint32_t) bytes[1] << 16) |
         ((uint32_t) bytes[2] << 8) |
         (uint32_t) bytes[3];
}

static void write_u32_be(uint8_t *out, uint32_t value) {
  out[0] = (uint8_t) ((value >> 24) & 0xffu);
  out[1] = (uint8_t) ((value >> 16) & 0xffu);
  out[2] = (uint8_t) ((value >> 8) & 0xffu);
  out[3] = (uint8_t) (value & 0xffu);
}

static uint32_t crc32_bytes(const uint8_t *bytes, size_t length) {
  static bool initialized = false;
  static uint32_t table[256];
  if (!initialized) {
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int bit = 0; bit < 8; ++bit) {
        c = (c & 1u) != 0u ? 0xedb88320u ^ (c >> 1) : (c >> 1);
      }
      table[n] = c;
    }
    initialized = true;
  }

  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < length; ++i) {
    crc = table[(crc ^ bytes[i]) & 0xffu] ^ (crc >> 8);
  }
  return crc ^ 0xffffffffu;
}

static uint32_t adler32_bytes(const uint8_t *bytes, size_t length) {
  uint32_t a = 1;
  uint32_t b = 0;
  const uint32_t mod = 65521;
  for (size_t i = 0; i < length; ++i) {
    a = (a + bytes[i]) % mod;
    b = (b + a) % mod;
  }
  return (b << 16) | a;
}

static int append_chunk(
    FILE *file,
    const char type[4],
    const uint8_t *data,
    uint32_t length) {
  uint8_t length_be[4];
  uint8_t crc_be[4];
  write_u32_be(length_be, length);

  const size_t crc_buffer_size = 4u + (size_t) length;
  uint8_t *crc_buffer = malloc(crc_buffer_size);
  if (crc_buffer == NULL) {
    fprintf(stderr, "Failed to allocate PNG chunk memory.\n");
    return -1;
  }

  memcpy(crc_buffer, type, 4);
  if (length > 0 && data != NULL) {
    memcpy(crc_buffer + 4, data, length);
  }
  write_u32_be(crc_be, crc32_bytes(crc_buffer, crc_buffer_size));

  const int failed =
      fwrite(length_be, 1, sizeof(length_be), file) != sizeof(length_be) ||
      fwrite(type, 1, 4, file) != 4 ||
      (length > 0 && fwrite(data, 1, length, file) != length) ||
      fwrite(crc_be, 1, sizeof(crc_be), file) != sizeof(crc_be);
  free(crc_buffer);
  if (failed) {
    fprintf(stderr, "Failed to write a PNG chunk.\n");
    return -1;
  }

  return 0;
}

static int write_png(const char *path, const Image *image) {
  const size_t raw_size = (size_t) image->height * (1u + image->stride);
  uint8_t *raw = malloc(raw_size);
  if (raw == NULL) {
    fprintf(stderr, "Failed to allocate PNG scanlines.\n");
    return -1;
  }

  size_t src = 0;
  size_t dst = 0;
  for (uint32_t y = 0; y < image->height; ++y) {
    raw[dst++] = 0;
    memcpy(raw + dst, image->pixels + src, image->stride);
    dst += image->stride;
    src += image->stride;
  }

  const size_t block_count = (raw_size + 65534u) / 65535u;
  const size_t zlib_size = 2u + block_count * 5u + raw_size + 4u;
  uint8_t *zlib_stream = malloc(zlib_size);
  if (zlib_stream == NULL) {
    fprintf(stderr, "Failed to allocate PNG zlib stream.\n");
    free(raw);
    return -1;
  }

  size_t zoff = 0;
  zlib_stream[zoff++] = 0x78;
  zlib_stream[zoff++] = 0x01;

  size_t raw_offset = 0;
  while (raw_offset < raw_size) {
    const size_t remaining = raw_size - raw_offset;
    const uint16_t block_size = (uint16_t) (remaining > 65535u ? 65535u : remaining);
    const uint8_t final_block = (raw_offset + block_size >= raw_size) ? 1u : 0u;
    zlib_stream[zoff++] = final_block;
    zlib_stream[zoff++] = (uint8_t) (block_size & 0xffu);
    zlib_stream[zoff++] = (uint8_t) ((block_size >> 8) & 0xffu);
    {
      const uint16_t nlen = (uint16_t) ~block_size;
      zlib_stream[zoff++] = (uint8_t) (nlen & 0xffu);
      zlib_stream[zoff++] = (uint8_t) ((nlen >> 8) & 0xffu);
    }
    memcpy(zlib_stream + zoff, raw + raw_offset, block_size);
    zoff += block_size;
    raw_offset += block_size;
  }

  write_u32_be(zlib_stream + zoff, adler32_bytes(raw, raw_size));
  zoff += 4u;

  FILE *file = fopen(path, "wb");
  if (file == NULL) {
    fprintf(stderr, "Failed to open %s for writing: %s\n", path, strerror(errno));
    free(zlib_stream);
    free(raw);
    return -1;
  }

  static const uint8_t png_signature[8] = {
      0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
  if (fwrite(png_signature, 1, sizeof(png_signature), file) != sizeof(png_signature)) {
    fprintf(stderr, "Failed to write the PNG signature.\n");
    fclose(file);
    free(zlib_stream);
    free(raw);
    return -1;
  }

  {
    uint8_t ihdr[13];
    write_u32_be(ihdr, image->width);
    write_u32_be(ihdr + 4, image->height);
    ihdr[8] = 8;
    ihdr[9] = 6;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;

    if (append_chunk(file, "IHDR", ihdr, sizeof(ihdr)) != 0 ||
        append_chunk(file, "IDAT", zlib_stream, (uint32_t) zoff) != 0 ||
        append_chunk(file, "IEND", NULL, 0) != 0) {
      fclose(file);
      free(zlib_stream);
      free(raw);
      return -1;
    }
  }

  if (fclose(file) != 0) {
    fprintf(stderr, "Failed to close %s.\n", path);
    free(zlib_stream);
    free(raw);
    return -1;
  }

  free(zlib_stream);
  free(raw);
  return 0;
}

static int read_png(const char *path, Image *image_out) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
    return -1;
  }

  if (fseek(file, 0, SEEK_END) != 0) {
    fprintf(stderr, "Failed to seek %s.\n", path);
    fclose(file);
    return -1;
  }
  {
    const long file_size_long = ftell(file);
    size_t file_size;
    uint8_t *bytes;
    size_t offset;
    bool saw_ihdr = false;
    bool saw_iend = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t *idat = NULL;
    size_t idat_size = 0;

    if (file_size_long < 0) {
      fprintf(stderr, "Failed to measure %s.\n", path);
      fclose(file);
      return -1;
    }
    file_size = (size_t) file_size_long;
    if (fseek(file, 0, SEEK_SET) != 0) {
      fprintf(stderr, "Failed to rewind %s.\n", path);
      fclose(file);
      return -1;
    }

    bytes = malloc(file_size);
    if (bytes == NULL) {
      fprintf(stderr, "Failed to allocate PNG input buffer.\n");
      fclose(file);
      return -1;
    }
    if (fread(bytes, 1, file_size, file) != file_size) {
      fprintf(stderr, "Failed to read %s.\n", path);
      free(bytes);
      fclose(file);
      return -1;
    }
    fclose(file);

    {
      static const uint8_t signature[8] = {
          0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
      if (file_size < sizeof(signature) || memcmp(bytes, signature, sizeof(signature)) != 0) {
        fprintf(stderr, "Unsupported PNG signature: %s\n", path);
        free(bytes);
        return -1;
      }
    }

    offset = 8u;
    while (offset + 12u <= file_size) {
      const uint32_t chunk_length = read_u32_be(bytes + offset);
      const uint8_t *chunk_type;
      const uint8_t *chunk_data;

      offset += 4u;
      chunk_type = bytes + offset;
      offset += 4u;
      if (offset + (size_t) chunk_length + 4u > file_size) {
        fprintf(stderr, "Truncated PNG chunk in %s\n", path);
        free(idat);
        free(bytes);
        return -1;
      }

      chunk_data = bytes + offset;
      offset += chunk_length;
      offset += 4u;

      if (memcmp(chunk_type, "IHDR", 4) == 0) {
        if (chunk_length != 13u) {
          fprintf(stderr, "Invalid IHDR chunk in %s\n", path);
          free(idat);
          free(bytes);
          return -1;
        }
        width = read_u32_be(chunk_data);
        height = read_u32_be(chunk_data + 4);
        if (chunk_data[8] != 8 || chunk_data[9] != 6 ||
            chunk_data[10] != 0 || chunk_data[11] != 0 || chunk_data[12] != 0) {
          fprintf(stderr, "Unsupported PNG format in %s\n", path);
          free(idat);
          free(bytes);
          return -1;
        }
        saw_ihdr = true;
      } else if (memcmp(chunk_type, "IDAT", 4) == 0) {
        uint8_t *resized = realloc(idat, idat_size + (size_t) chunk_length);
        if (resized == NULL) {
          fprintf(stderr, "Failed to grow PNG IDAT buffer.\n");
          free(idat);
          free(bytes);
          return -1;
        }
        idat = resized;
        memcpy(idat + idat_size, chunk_data, chunk_length);
        idat_size += chunk_length;
      } else if (memcmp(chunk_type, "IEND", 4) == 0) {
        saw_iend = true;
        break;
      }
    }

    if (!saw_ihdr || !saw_iend || width == 0 || height == 0 || idat_size < 6u) {
      fprintf(stderr, "Incomplete PNG data in %s\n", path);
      free(idat);
      free(bytes);
      return -1;
    }

    {
      Image image;
      uint8_t *raw;
      const size_t raw_size_hint = (size_t) height * (1u + (size_t) width * 4u);
      memset(&image, 0, sizeof(image));
      if (init_image(&image, width, height) != 0) {
        free(idat);
        free(bytes);
        return -1;
      }

      raw = malloc(raw_size_hint);
      if (raw == NULL) {
        fprintf(stderr, "Failed to allocate PNG raw scanlines.\n");
        free_image(&image);
        free(idat);
        free(bytes);
        return -1;
      }

      if (inflate_stored_blocks(idat, idat_size, raw, raw_size_hint) != 0) {
        fprintf(stderr, "Unsupported PNG compression in %s\n", path);
        free(raw);
        free_image(&image);
        free(idat);
        free(bytes);
        return -1;
      }

      for (uint32_t y = 0; y < height; ++y) {
        const size_t row_offset = (size_t) y * (1u + image.stride);
        if (raw[row_offset] != 0u) {
          fprintf(stderr, "Unsupported PNG filter in %s\n", path);
          free(raw);
          free_image(&image);
          free(idat);
          free(bytes);
          return -1;
        }
        memcpy(image.pixels + (size_t) y * image.stride, raw + row_offset + 1u, image.stride);
      }

      free(raw);
      free(idat);
      free(bytes);
      *image_out = image;
      return 0;
    }
  }
}

static int inflate_stored_blocks(
    const uint8_t *compressed,
    size_t compressed_size,
    uint8_t *raw_out,
    size_t raw_size) {
  size_t offset = 2u;
  size_t raw_offset = 0;
  bool final_block = false;
  if (compressed_size < 6u) {
    return -1;
  }

  while (!final_block) {
    uint8_t header;
    uint16_t len;
    uint16_t nlen;
    if (offset + 5u > compressed_size - 4u) {
      return -1;
    }

    header = compressed[offset++];
    final_block = (header & 0x01u) != 0u;
    if ((header & 0x06u) != 0u) {
      return -1;
    }

    len = (uint16_t) compressed[offset] | (uint16_t) ((uint16_t) compressed[offset + 1] << 8);
    offset += 2u;
    nlen = (uint16_t) compressed[offset] | (uint16_t) ((uint16_t) compressed[offset + 1] << 8);
    offset += 2u;
    if ((uint16_t) ~len != nlen) {
      return -1;
    }
    if (offset + len > compressed_size - 4u || raw_offset + len > raw_size) {
      return -1;
    }

    memcpy(raw_out + raw_offset, compressed + offset, len);
    raw_offset += len;
    offset += len;
  }

  if (raw_offset != raw_size) {
    return -1;
  }

  return adler32_bytes(raw_out, raw_size) == read_u32_be(compressed + compressed_size - 4u)
             ? 0
             : -1;
}
