#include "addImages_pipeline.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
  uint32_t width;
  uint32_t height;
  size_t stride;
  uint8_t *pixels;
} Image;

typedef struct {
  uint32_t layer_index;
  char *name;
  char *path;
} LayerFile;

static int mkdir_recursive(const char *path);
static int collect_layer_files(const char *directory, LayerFile **layers_out, size_t *count_out);
static void free_layer_files(LayerFile *layers, size_t count);
static int compare_layer_files(const void *lhs, const void *rhs);
static bool parse_layer_file_name(const char *name, uint32_t *index_out);
static int init_image(Image *image, uint32_t width, uint32_t height);
static void free_image(Image *image);
static int read_png(const char *path, Image *image_out);
static int write_png(const char *path, const Image *image);
static int resize_image(const Image *source, uint32_t width, uint32_t height, Image *image_out);
static int flip_horizontal(const Image *source, Image *image_out);
static int prepare_overlay(const AddImagesOverlay *overlay, Image *image_out);
static int identify_image_with_magick(const char *path, uint32_t *width_out, uint32_t *height_out);
static int load_overlay_with_magick(
    const char *path,
    uint32_t width,
    uint32_t height,
    bool invert,
    Image *image_out);
static int shell_quote(const char *text, char **quoted_out);
static void alpha_composite(Image *base, const Image *overlay, uint32_t x0, uint32_t y0);
static int copy_file(const char *src_path, const char *dst_path);
static int build_output_path(const char *directory, const char *name, char **path_out);
static bool layer_is_in_range(const AddImagesOptions *options, uint32_t layer_index);
static uint32_t read_u32_be(const uint8_t *bytes);
static void write_u32_be(uint8_t *out, uint32_t value);
static uint32_t crc32_bytes(const uint8_t *bytes, size_t length);
static uint32_t adler32_bytes(const uint8_t *bytes, size_t length);
static int append_chunk(FILE *file, const char type[4], const uint8_t *data, uint32_t length);
static int inflate_stored_blocks(
    const uint8_t *compressed,
    size_t compressed_size,
    uint8_t *raw_out,
    size_t raw_size);
static int unfilter_png_rows(
    const uint8_t *raw,
    size_t raw_size,
    uint32_t width,
    uint32_t height,
    uint8_t channels,
    uint8_t *decoded);

int run_add_images(const AddImagesOptions *options) {
  LayerFile *layers = NULL;
  size_t layer_count = 0u;
  Image *overlays = NULL;
  int rc = -1;
  size_t composited = 0u;
  size_t copied = 0u;

  if (options == NULL) {
    fprintf(stderr, "Missing addImages options.\n");
    return -1;
  }

  if (collect_layer_files(options->input_dir, &layers, &layer_count) != 0) {
    return -1;
  }
  if (layer_count == 0u) {
    fprintf(stderr, "No layer PNGs were found in %s\n", options->input_dir);
    free_layer_files(layers, layer_count);
    return -1;
  }
  if (mkdir_recursive(options->output_dir) != 0) {
    free_layer_files(layers, layer_count);
    return -1;
  }

  overlays = calloc(options->overlay_count, sizeof(*overlays));
  if (overlays == NULL) {
    fprintf(stderr, "Failed to allocate prepared overlays.\n");
    free_layer_files(layers, layer_count);
    return -1;
  }

  for (size_t i = 0; i < options->overlay_count; ++i) {
    if (prepare_overlay(&options->overlays[i], &overlays[i]) != 0) {
      goto cleanup;
    }
  }

  for (size_t i = 0; i < layer_count; ++i) {
    char *output_path = NULL;
    if (build_output_path(options->output_dir, layers[i].name, &output_path) != 0) {
      goto cleanup;
    }

    if (layer_is_in_range(options, layers[i].layer_index)) {
      Image base;
      memset(&base, 0, sizeof(base));
      if (read_png(layers[i].path, &base) != 0) {
        free(output_path);
        goto cleanup;
      }

      for (size_t overlay_index = 0; overlay_index < options->overlay_count; ++overlay_index) {
        alpha_composite(
            &base,
            &overlays[overlay_index],
            options->overlays[overlay_index].x,
            options->overlays[overlay_index].y);
      }

      if (write_png(output_path, &base) != 0) {
        free_image(&base);
        free(output_path);
        goto cleanup;
      }
      free_image(&base);
      ++composited;
    } else {
      if (copy_file(layers[i].path, output_path) != 0) {
        free(output_path);
        goto cleanup;
      }
      ++copied;
    }

    free(output_path);
  }

  printf(
      "Wrote %zu composited layer(s) and copied %zu unchanged layer(s) to %s\n",
      composited,
      copied,
      options->output_dir);
  rc = 0;

cleanup:
  if (overlays != NULL) {
    for (size_t i = 0; i < options->overlay_count; ++i) {
      free_image(&overlays[i]);
    }
  }
  free(overlays);
  free_layer_files(layers, layer_count);
  return rc;
}

static int mkdir_recursive(const char *path) {
  if (path == NULL || path[0] == '\0') {
    fprintf(stderr, "Missing output directory.\n");
    return -1;
  }

  char *copy = strdup(path);
  if (copy == NULL) {
    fprintf(stderr, "Failed to allocate output directory path.\n");
    return -1;
  }

  for (char *cursor = copy + 1; *cursor != '\0'; ++cursor) {
    if (*cursor == '/') {
      *cursor = '\0';
      if (mkdir(copy, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "Failed to create %s: %s\n", copy, strerror(errno));
        free(copy);
        return -1;
      }
      *cursor = '/';
    }
  }

  if (mkdir(copy, 0777) != 0 && errno != EEXIST) {
    fprintf(stderr, "Failed to create %s: %s\n", copy, strerror(errno));
    free(copy);
    return -1;
  }

  free(copy);
  return 0;
}

static int collect_layer_files(const char *directory, LayerFile **layers_out, size_t *count_out) {
  DIR *dir = opendir(directory);
  if (dir == NULL) {
    fprintf(stderr, "Failed to open input directory %s: %s\n", directory, strerror(errno));
    return -1;
  }

  LayerFile *layers = NULL;
  size_t count = 0u;
  size_t capacity = 0u;

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    uint32_t layer_index = 0u;
    if (!parse_layer_file_name(entry->d_name, &layer_index)) {
      continue;
    }

    if (count == capacity) {
      const size_t next_capacity = capacity == 0u ? 64u : capacity * 2u;
      LayerFile *resized = realloc(layers, next_capacity * sizeof(*layers));
      if (resized == NULL) {
        fprintf(stderr, "Failed to grow layer file list.\n");
        free_layer_files(layers, count);
        closedir(dir);
        return -1;
      }
      layers = resized;
      capacity = next_capacity;
    }

    layers[count].layer_index = layer_index;
    layers[count].name = strdup(entry->d_name);
    if (layers[count].name == NULL ||
        build_output_path(directory, entry->d_name, &layers[count].path) != 0) {
      fprintf(stderr, "Failed to allocate layer file path.\n");
      free(layers[count].name);
      free_layer_files(layers, count);
      closedir(dir);
      return -1;
    }
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
    free(layers[i].name);
    free(layers[i].path);
  }
  free(layers);
}

static int compare_layer_files(const void *lhs, const void *rhs) {
  const LayerFile *a = (const LayerFile *) lhs;
  const LayerFile *b = (const LayerFile *) rhs;
  if (a->layer_index < b->layer_index) {
    return -1;
  }
  if (a->layer_index > b->layer_index) {
    return 1;
  }
  return strcmp(a->name, b->name);
}

static bool parse_layer_file_name(const char *name, uint32_t *index_out) {
  if (strncmp(name, "out_", 4u) != 0) {
    return false;
  }

  char *end = NULL;
  errno = 0;
  const unsigned long parsed = strtoul(name + 4, &end, 10);
  if (errno != 0 || end == name + 4 || strcmp(end, ".png") != 0 || parsed > UINT32_MAX) {
    return false;
  }

  *index_out = (uint32_t) parsed;
  return true;
}

static int init_image(Image *image, uint32_t width, uint32_t height) {
  if (width == 0u || height == 0u ||
      (size_t) height > SIZE_MAX / ((size_t) width * 4u)) {
    fprintf(stderr, "Invalid image dimensions %ux%u.\n", width, height);
    return -1;
  }

  image->width = width;
  image->height = height;
  image->stride = (size_t) width * 4u;
  image->pixels = calloc((size_t) height, image->stride);
  if (image->pixels == NULL) {
    fprintf(stderr, "Failed to allocate image pixels.\n");
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

static uint8_t png_channel_count(uint8_t color_type) {
  switch (color_type) {
    case 0:
    case 3:
      return 1u;
    case 2:
      return 3u;
    case 4:
      return 2u;
    case 6:
      return 4u;
    default:
      return 0u;
  }
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
  const long file_size_long = ftell(file);
  if (file_size_long < 0) {
    fprintf(stderr, "Failed to measure %s.\n", path);
    fclose(file);
    return -1;
  }
  const size_t file_size = (size_t) file_size_long;
  rewind(file);

  uint8_t *bytes = malloc(file_size);
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

  static const uint8_t signature[8] = {
      0x89u, 0x50u, 0x4eu, 0x47u, 0x0du, 0x0au, 0x1au, 0x0au};
  if (file_size < sizeof(signature) || memcmp(bytes, signature, sizeof(signature)) != 0) {
    fprintf(stderr, "Unsupported PNG signature: %s\n", path);
    free(bytes);
    return -1;
  }

  uint32_t width = 0u;
  uint32_t height = 0u;
  uint8_t bit_depth = 0u;
  uint8_t color_type = 0u;
  uint8_t *idat = NULL;
  size_t idat_size = 0u;
  uint8_t *palette = NULL;
  size_t palette_size = 0u;
  uint8_t *trns = NULL;
  size_t trns_size = 0u;
  bool saw_ihdr = false;
  bool saw_iend = false;
  size_t offset = 8u;

  while (offset + 12u <= file_size) {
    const uint32_t chunk_length = read_u32_be(bytes + offset);
    offset += 4u;
    const uint8_t *chunk_type = bytes + offset;
    offset += 4u;
    if (offset + (size_t) chunk_length + 4u > file_size) {
      fprintf(stderr, "Truncated PNG chunk in %s\n", path);
      goto fail;
    }
    const uint8_t *chunk_data = bytes + offset;
    offset += chunk_length;
    offset += 4u;

    if (memcmp(chunk_type, "IHDR", 4u) == 0) {
      if (chunk_length != 13u) {
        fprintf(stderr, "Invalid IHDR chunk in %s\n", path);
        goto fail;
      }
      width = read_u32_be(chunk_data);
      height = read_u32_be(chunk_data + 4u);
      bit_depth = chunk_data[8];
      color_type = chunk_data[9];
      if (bit_depth != 8u || png_channel_count(color_type) == 0u ||
          chunk_data[10] != 0u || chunk_data[11] != 0u || chunk_data[12] != 0u) {
        fprintf(stderr, "Unsupported PNG format in %s\n", path);
        goto fail;
      }
      saw_ihdr = true;
    } else if (memcmp(chunk_type, "PLTE", 4u) == 0) {
      uint8_t *copy = malloc(chunk_length);
      if (copy == NULL) {
        fprintf(stderr, "Failed to allocate PNG palette.\n");
        goto fail;
      }
      memcpy(copy, chunk_data, chunk_length);
      free(palette);
      palette = copy;
      palette_size = chunk_length;
    } else if (memcmp(chunk_type, "tRNS", 4u) == 0) {
      uint8_t *copy = malloc(chunk_length);
      if (copy == NULL) {
        fprintf(stderr, "Failed to allocate PNG transparency data.\n");
        goto fail;
      }
      memcpy(copy, chunk_data, chunk_length);
      free(trns);
      trns = copy;
      trns_size = chunk_length;
    } else if (memcmp(chunk_type, "IDAT", 4u) == 0) {
      uint8_t *resized = realloc(idat, idat_size + (size_t) chunk_length);
      if (resized == NULL) {
        fprintf(stderr, "Failed to grow PNG IDAT buffer.\n");
        goto fail;
      }
      idat = resized;
      memcpy(idat + idat_size, chunk_data, chunk_length);
      idat_size += chunk_length;
    } else if (memcmp(chunk_type, "IEND", 4u) == 0) {
      saw_iend = true;
      break;
    }
  }

  if (!saw_ihdr || !saw_iend || width == 0u || height == 0u || idat_size == 0u) {
    fprintf(stderr, "Incomplete PNG data in %s\n", path);
    goto fail;
  }
  if (color_type == 3u && (palette == NULL || palette_size % 3u != 0u)) {
    fprintf(stderr, "Indexed PNG is missing a valid palette: %s\n", path);
    goto fail;
  }

  const uint8_t channels = png_channel_count(color_type);
  const size_t row_size = (size_t) width * channels;
  const size_t raw_size = (size_t) height * (row_size + 1u);
  uint8_t *raw = malloc(raw_size);
  uint8_t *decoded = malloc((size_t) height * row_size);
  if (raw == NULL || decoded == NULL) {
    fprintf(stderr, "Failed to allocate PNG decode buffers.\n");
    free(raw);
    free(decoded);
    goto fail;
  }

  if (inflate_stored_blocks(idat, idat_size, raw, raw_size) != 0) {
    fprintf(stderr, "Unsupported PNG compression in %s\n", path);
    free(raw);
    free(decoded);
    goto fail;
  }

  if (unfilter_png_rows(raw, raw_size, width, height, channels, decoded) != 0) {
    fprintf(stderr, "Unsupported PNG filter in %s\n", path);
    free(raw);
    free(decoded);
    goto fail;
  }

  Image image;
  memset(&image, 0, sizeof(image));
  if (init_image(&image, width, height) != 0) {
    free(raw);
    free(decoded);
    goto fail;
  }

  for (size_t i = 0; i < (size_t) width * height; ++i) {
    const size_t src = (size_t) i * channels;
    const size_t dst = (size_t) i * 4u;
    if (color_type == 0u) {
      const uint8_t g = decoded[src];
      image.pixels[dst] = g;
      image.pixels[dst + 1u] = g;
      image.pixels[dst + 2u] = g;
      image.pixels[dst + 3u] = 255u;
    } else if (color_type == 2u) {
      image.pixels[dst] = decoded[src];
      image.pixels[dst + 1u] = decoded[src + 1u];
      image.pixels[dst + 2u] = decoded[src + 2u];
      image.pixels[dst + 3u] = 255u;
    } else if (color_type == 3u) {
      const uint8_t index = decoded[src];
      if ((size_t) index * 3u + 2u >= palette_size) {
        fprintf(stderr, "PNG palette index out of range in %s\n", path);
        free_image(&image);
        free(raw);
        free(decoded);
        goto fail;
      }
      image.pixels[dst] = palette[(size_t) index * 3u];
      image.pixels[dst + 1u] = palette[(size_t) index * 3u + 1u];
      image.pixels[dst + 2u] = palette[(size_t) index * 3u + 2u];
      image.pixels[dst + 3u] = index < trns_size ? trns[index] : 255u;
    } else if (color_type == 4u) {
      const uint8_t g = decoded[src];
      image.pixels[dst] = g;
      image.pixels[dst + 1u] = g;
      image.pixels[dst + 2u] = g;
      image.pixels[dst + 3u] = decoded[src + 1u];
    } else {
      memcpy(image.pixels + dst, decoded + src, 4u);
    }
  }

  free(raw);
  free(decoded);
  free(idat);
  free(palette);
  free(trns);
  free(bytes);
  *image_out = image;
  return 0;

fail:
  free(idat);
  free(palette);
  free(trns);
  free(bytes);
  return -1;
}

static int write_png(const char *path, const Image *image) {
  const size_t raw_size = (size_t) image->height * (1u + image->stride);
  uint8_t *raw = malloc(raw_size);
  if (raw == NULL) {
    fprintf(stderr, "Failed to allocate PNG scanlines.\n");
    return -1;
  }

  size_t src = 0u;
  size_t dst = 0u;
  for (uint32_t y = 0; y < image->height; ++y) {
    raw[dst++] = 0u;
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

  size_t zoff = 0u;
  zlib_stream[zoff++] = 0x78u;
  zlib_stream[zoff++] = 0x01u;

  size_t raw_offset = 0u;
  while (raw_offset < raw_size) {
    const size_t remaining = raw_size - raw_offset;
    const uint16_t block_size = (uint16_t) (remaining > 65535u ? 65535u : remaining);
    const uint8_t final_block = raw_offset + block_size >= raw_size ? 1u : 0u;
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
      0x89u, 0x50u, 0x4eu, 0x47u, 0x0du, 0x0au, 0x1au, 0x0au};
  if (fwrite(png_signature, 1, sizeof(png_signature), file) != sizeof(png_signature)) {
    fprintf(stderr, "Failed to write the PNG signature.\n");
    fclose(file);
    free(zlib_stream);
    free(raw);
    return -1;
  }

  uint8_t ihdr[13];
  write_u32_be(ihdr, image->width);
  write_u32_be(ihdr + 4, image->height);
  ihdr[8] = 8u;
  ihdr[9] = 6u;
  ihdr[10] = 0u;
  ihdr[11] = 0u;
  ihdr[12] = 0u;
  if (append_chunk(file, "IHDR", ihdr, sizeof(ihdr)) != 0 ||
      append_chunk(file, "IDAT", zlib_stream, (uint32_t) zoff) != 0 ||
      append_chunk(file, "IEND", NULL, 0u) != 0) {
    fclose(file);
    free(zlib_stream);
    free(raw);
    return -1;
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

static int resize_image(const Image *source, uint32_t width, uint32_t height, Image *image_out) {
  Image resized;
  memset(&resized, 0, sizeof(resized));
  if (init_image(&resized, width, height) != 0) {
    return -1;
  }

  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t src_y = (uint32_t) (((uint64_t) y * source->height) / height);
    for (uint32_t x = 0; x < width; ++x) {
      const uint32_t src_x = (uint32_t) (((uint64_t) x * source->width) / width);
      const size_t src = (size_t) src_y * source->stride + (size_t) src_x * 4u;
      const size_t dst = (size_t) y * resized.stride + (size_t) x * 4u;
      memcpy(resized.pixels + dst, source->pixels + src, 4u);
    }
  }

  *image_out = resized;
  return 0;
}

static int flip_horizontal(const Image *source, Image *image_out) {
  Image flipped;
  memset(&flipped, 0, sizeof(flipped));
  if (init_image(&flipped, source->width, source->height) != 0) {
    return -1;
  }

  for (uint32_t y = 0; y < source->height; ++y) {
    for (uint32_t x = 0; x < source->width; ++x) {
      const size_t src = (size_t) y * source->stride + (size_t) (source->width - 1u - x) * 4u;
      const size_t dst = (size_t) y * flipped.stride + (size_t) x * 4u;
      memcpy(flipped.pixels + dst, source->pixels + src, 4u);
    }
  }

  *image_out = flipped;
  return 0;
}

static int prepare_overlay(const AddImagesOverlay *overlay, Image *image_out) {
  Image source;
  Image resized;
  memset(&source, 0, sizeof(source));
  memset(&resized, 0, sizeof(resized));

  uint32_t source_width = 0u;
  uint32_t source_height = 0u;
  uint32_t width = overlay->has_width ? overlay->width : 0u;
  uint32_t height = overlay->has_height ? overlay->height : 0u;

  if ((!overlay->has_width || !overlay->has_height) &&
      identify_image_with_magick(overlay->src_path, &source_width, &source_height) != 0) {
    if (read_png(overlay->src_path, &source) != 0) {
      return -1;
    }
    source_width = source.width;
    source_height = source.height;
  }

  if (!overlay->has_width) {
    width = (uint32_t) fmax(1.0, round((double) source_width * (double) height / source_height));
  }
  if (!overlay->has_height) {
    height = (uint32_t) fmax(1.0, round((double) source_height * (double) width / source_width));
  }

  if (load_overlay_with_magick(overlay->src_path, width, height, overlay->invert, image_out) == 0) {
    free_image(&source);
    return 0;
  }

  if (source.pixels == NULL && read_png(overlay->src_path, &source) != 0) {
    return -1;
  }

  if (resize_image(&source, width, height, &resized) != 0) {
    free_image(&source);
    return -1;
  }
  free_image(&source);

  if (overlay->invert) {
    Image flipped;
    memset(&flipped, 0, sizeof(flipped));
    if (flip_horizontal(&resized, &flipped) != 0) {
      free_image(&resized);
      return -1;
    }
    free_image(&resized);
    *image_out = flipped;
  } else {
    *image_out = resized;
  }

  return 0;
}

static int identify_image_with_magick(const char *path, uint32_t *width_out, uint32_t *height_out) {
  char *quoted_path = NULL;
  if (shell_quote(path, &quoted_path) != 0) {
    return -1;
  }

  const char *prefix = "identify -format '%w %h' ";
  char *command = malloc(strlen(prefix) + strlen(quoted_path) + 1u);
  if (command == NULL) {
    free(quoted_path);
    return -1;
  }
  sprintf(command, "%s%s", prefix, quoted_path);

  FILE *pipe = popen(command, "r");
  free(command);
  free(quoted_path);
  if (pipe == NULL) {
    return -1;
  }

  unsigned long width = 0u;
  unsigned long height = 0u;
  const int scanned = fscanf(pipe, "%lu %lu", &width, &height);
  const int status = pclose(pipe);
  if (scanned != 2 || status != 0 || width == 0u || height == 0u ||
      width > UINT32_MAX || height > UINT32_MAX) {
    return -1;
  }

  *width_out = (uint32_t) width;
  *height_out = (uint32_t) height;
  return 0;
}

static int load_overlay_with_magick(
    const char *path,
    uint32_t width,
    uint32_t height,
    bool invert,
    Image *image_out) {
  char *quoted_path = NULL;
  if (shell_quote(path, &quoted_path) != 0) {
    return -1;
  }

  char resize_arg[64];
  snprintf(resize_arg, sizeof(resize_arg), "%ux%u!", width, height);
  const char *prefix = "magick ";
  const char *resize_prefix = " -depth 8 -filter point -resize ";
  const char *flop = invert ? " -flop" : "";
  const char *suffix = " rgba:-";
  const size_t command_len = strlen(prefix) + strlen(quoted_path) +
                             strlen(resize_prefix) + strlen(resize_arg) +
                             strlen(flop) + strlen(suffix) + 1u;
  char *command = malloc(command_len);
  if (command == NULL) {
    free(quoted_path);
    return -1;
  }
  snprintf(
      command,
      command_len,
      "%s%s%s%s%s%s",
      prefix,
      quoted_path,
      resize_prefix,
      resize_arg,
      flop,
      suffix);

  Image image;
  memset(&image, 0, sizeof(image));
  if (init_image(&image, width, height) != 0) {
    free(command);
    free(quoted_path);
    return -1;
  }

  FILE *pipe = popen(command, "r");
  free(command);
  free(quoted_path);
  if (pipe == NULL) {
    free_image(&image);
    return -1;
  }

  const size_t expected = (size_t) height * image.stride;
  const size_t read_count = fread(image.pixels, 1, expected, pipe);
  const int status = pclose(pipe);
  if (read_count != expected || status != 0) {
    free_image(&image);
    return -1;
  }

  *image_out = image;
  return 0;
}

static int shell_quote(const char *text, char **quoted_out) {
  size_t len = 2u;
  for (const char *cursor = text; *cursor != '\0'; ++cursor) {
    len += *cursor == '\'' ? 4u : 1u;
  }

  char *quoted = malloc(len + 1u);
  if (quoted == NULL) {
    return -1;
  }

  char *out = quoted;
  *out++ = '\'';
  for (const char *cursor = text; *cursor != '\0'; ++cursor) {
    if (*cursor == '\'') {
      memcpy(out, "'\\''", 4u);
      out += 4u;
    } else {
      *out++ = *cursor;
    }
  }
  *out++ = '\'';
  *out = '\0';

  *quoted_out = quoted;
  return 0;
}

static void alpha_composite(Image *base, const Image *overlay, uint32_t x0, uint32_t y0) {
  if (x0 >= base->width || y0 >= base->height) {
    return;
  }

  const uint32_t x_end =
      overlay->width > base->width - x0 ? base->width : x0 + overlay->width;
  const uint32_t y_end =
      overlay->height > base->height - y0 ? base->height : y0 + overlay->height;

  for (uint32_t y = y0; y < y_end; ++y) {
    const uint32_t overlay_y = y - y0;
    for (uint32_t x = x0; x < x_end; ++x) {
      const uint32_t overlay_x = x - x0;
      const size_t src = (size_t) overlay_y * overlay->stride + (size_t) overlay_x * 4u;
      const size_t dst = (size_t) y * base->stride + (size_t) x * 4u;
      const uint32_t sa = overlay->pixels[src + 3u];
      if (sa == 0u) {
        continue;
      }
      if (sa == 255u) {
        memcpy(base->pixels + dst, overlay->pixels + src, 4u);
        continue;
      }

      const uint32_t da = base->pixels[dst + 3u];
      for (size_t channel = 0u; channel < 3u; ++channel) {
        base->pixels[dst + channel] = (uint8_t) (
            (overlay->pixels[src + channel] * sa +
             base->pixels[dst + channel] * (255u - sa) + 127u) /
            255u);
      }
      base->pixels[dst + 3u] = (uint8_t) (sa + (da * (255u - sa) + 127u) / 255u);
    }
  }
}

static int copy_file(const char *src_path, const char *dst_path) {
  if (strcmp(src_path, dst_path) == 0) {
    return 0;
  }

  FILE *src = fopen(src_path, "rb");
  if (src == NULL) {
    fprintf(stderr, "Failed to open %s: %s\n", src_path, strerror(errno));
    return -1;
  }
  FILE *dst = fopen(dst_path, "wb");
  if (dst == NULL) {
    fprintf(stderr, "Failed to open %s: %s\n", dst_path, strerror(errno));
    fclose(src);
    return -1;
  }

  uint8_t buffer[64 * 1024];
  size_t read_count;
  while ((read_count = fread(buffer, 1, sizeof(buffer), src)) > 0u) {
    if (fwrite(buffer, 1, read_count, dst) != read_count) {
      fprintf(stderr, "Failed to write %s.\n", dst_path);
      fclose(src);
      fclose(dst);
      return -1;
    }
  }

  if (ferror(src)) {
    fprintf(stderr, "Failed to read %s.\n", src_path);
    fclose(src);
    fclose(dst);
    return -1;
  }
  if (fclose(src) != 0 || fclose(dst) != 0) {
    fprintf(stderr, "Failed to close copied files.\n");
    return -1;
  }
  return 0;
}

static int build_output_path(const char *directory, const char *name, char **path_out) {
  const size_t directory_len = strlen(directory);
  const size_t name_len = strlen(name);
  const bool needs_slash = directory_len > 0u && directory[directory_len - 1u] != '/';
  char *path = malloc(directory_len + (needs_slash ? 1u : 0u) + name_len + 1u);
  if (path == NULL) {
    fprintf(stderr, "Failed to allocate output path.\n");
    return -1;
  }

  memcpy(path, directory, directory_len);
  size_t offset = directory_len;
  if (needs_slash) {
    path[offset++] = '/';
  }
  memcpy(path + offset, name, name_len + 1u);
  *path_out = path;
  return 0;
}

static bool layer_is_in_range(const AddImagesOptions *options, uint32_t layer_index) {
  if (options->has_layer_first && layer_index < options->layer_first) {
    return false;
  }
  if (options->has_layer_last && layer_index > options->layer_last) {
    return false;
  }
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
  uint32_t a = 1u;
  uint32_t b = 0u;
  const uint32_t mod = 65521u;
  for (size_t i = 0; i < length; ++i) {
    a = (a + bytes[i]) % mod;
    b = (b + a) % mod;
  }
  return (b << 16) | a;
}

static int append_chunk(FILE *file, const char type[4], const uint8_t *data, uint32_t length) {
  uint8_t length_be[4];
  uint8_t crc_be[4];
  write_u32_be(length_be, length);

  const size_t crc_buffer_size = 4u + (size_t) length;
  uint8_t *crc_buffer = malloc(crc_buffer_size);
  if (crc_buffer == NULL) {
    fprintf(stderr, "Failed to allocate PNG chunk memory.\n");
    return -1;
  }

  memcpy(crc_buffer, type, 4u);
  if (length > 0u && data != NULL) {
    memcpy(crc_buffer + 4u, data, length);
  }
  write_u32_be(crc_be, crc32_bytes(crc_buffer, crc_buffer_size));

  const int failed =
      fwrite(length_be, 1, sizeof(length_be), file) != sizeof(length_be) ||
      fwrite(type, 1, 4u, file) != 4u ||
      (length > 0u && fwrite(data, 1, length, file) != length) ||
      fwrite(crc_be, 1, sizeof(crc_be), file) != sizeof(crc_be);
  free(crc_buffer);
  if (failed) {
    fprintf(stderr, "Failed to write a PNG chunk.\n");
    return -1;
  }

  return 0;
}

static int inflate_stored_blocks(
    const uint8_t *compressed,
    size_t compressed_size,
    uint8_t *raw_out,
    size_t raw_size) {
  size_t offset = 2u;
  size_t raw_offset = 0u;
  bool final_block = false;
  if (compressed_size < 6u) {
    return -1;
  }

  while (!final_block) {
    if (offset + 5u > compressed_size - 4u) {
      return -1;
    }

    const uint8_t header = compressed[offset++];
    final_block = (header & 0x01u) != 0u;
    if ((header & 0x06u) != 0u) {
      return -1;
    }

    const uint16_t len =
        (uint16_t) compressed[offset] | (uint16_t) ((uint16_t) compressed[offset + 1u] << 8);
    offset += 2u;
    const uint16_t nlen =
        (uint16_t) compressed[offset] | (uint16_t) ((uint16_t) compressed[offset + 1u] << 8);
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

static int paeth_predictor(int a, int b, int c) {
  const int p = a + b - c;
  const int pa = abs(p - a);
  const int pb = abs(p - b);
  const int pc = abs(p - c);
  if (pa <= pb && pa <= pc) {
    return a;
  }
  if (pb <= pc) {
    return b;
  }
  return c;
}

static int unfilter_png_rows(
    const uint8_t *raw,
    size_t raw_size,
    uint32_t width,
    uint32_t height,
    uint8_t channels,
    uint8_t *decoded) {
  const size_t row_size = (size_t) width * channels;
  const size_t expected_size = (size_t) height * (row_size + 1u);
  if (raw_size != expected_size) {
    return -1;
  }

  const size_t bpp = channels;
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t filter = raw[(size_t) y * (row_size + 1u)];
    const uint8_t *src = raw + (size_t) y * (row_size + 1u) + 1u;
    uint8_t *row = decoded + (size_t) y * row_size;
    const uint8_t *previous = y == 0u ? NULL : decoded + (size_t) (y - 1u) * row_size;
    memcpy(row, src, row_size);

    for (size_t i = 0; i < row_size; ++i) {
      const uint8_t left = i >= bpp ? row[i - bpp] : 0u;
      const uint8_t up = previous != NULL ? previous[i] : 0u;
      const uint8_t up_left = previous != NULL && i >= bpp ? previous[i - bpp] : 0u;
      switch (filter) {
        case 0:
          break;
        case 1:
          row[i] = (uint8_t) (row[i] + left);
          break;
        case 2:
          row[i] = (uint8_t) (row[i] + up);
          break;
        case 3:
          row[i] = (uint8_t) (row[i] + ((left + up) >> 1));
          break;
        case 4:
          row[i] = (uint8_t) (row[i] + paeth_predictor(left, up, up_left));
          break;
        default:
          return -1;
      }
    }
  }

  return 0;
}
