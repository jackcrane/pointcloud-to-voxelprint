#include "crop_cli.h"

#include "../ascii_ply/ascii_ply_support.h"

#include <stdio.h>
#include <string.h>

typedef struct {
  bool min_x;
  bool max_x;
  bool min_y;
  bool max_y;
  bool min_z;
  bool max_z;
} CropFlags;

static void print_usage(const char *program_name) {
  fprintf(
      stderr,
      "Usage: %s [--log-interval N] <input.ply> <output.ply> "
      "--minX VALUE --maxX VALUE --minY VALUE --maxY VALUE --minZ VALUE --maxZ VALUE\n",
      program_name);
}

static bool parse_option_value(
    const char *option_name,
    const char *value,
    double *field_out,
    bool *seen_out) {
  if (!ascii_ply_parse_double_str(value, field_out)) {
    fprintf(stderr, "Invalid value for %s.\n", option_name);
    return false;
  }

  *seen_out = true;
  return true;
}

int parse_crop_options(int argc, char **argv, CropOptions *options_out) {
  CropOptions options;
  CropFlags flags;
  memset(&options, 0, sizeof(options));
  memset(&flags, 0, sizeof(flags));
  options.log_interval = ASCII_PLY_DEFAULT_LOG_INTERVAL;

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];

    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      print_usage(argv[0]);
      return -1;
    }

    if (strcmp(arg, "--log-interval") == 0) {
      if (i + 1 >= argc ||
          !ascii_ply_parse_uint64_str(argv[i + 1], &options.log_interval)) {
        fprintf(stderr, "Invalid value for --log-interval.\n");
        print_usage(argv[0]);
        return -1;
      }
      i++;
      continue;
    }

    if (ascii_ply_starts_with(arg, "--log-interval=")) {
      if (!ascii_ply_parse_uint64_str(
              arg + strlen("--log-interval="),
              &options.log_interval)) {
        fprintf(stderr, "Invalid value for --log-interval.\n");
        print_usage(argv[0]);
        return -1;
      }
      continue;
    }

    if (strcmp(arg, "--minX") == 0) {
      if (i + 1 >= argc ||
          !parse_option_value("--minX", argv[i + 1], &options.min_x, &flags.min_x)) {
        print_usage(argv[0]);
        return -1;
      }
      i++;
      continue;
    }

    if (ascii_ply_starts_with(arg, "--minX=")) {
      if (!parse_option_value(
              "--minX",
              arg + strlen("--minX="),
              &options.min_x,
              &flags.min_x)) {
        print_usage(argv[0]);
        return -1;
      }
      continue;
    }

    if (strcmp(arg, "--maxX") == 0) {
      if (i + 1 >= argc ||
          !parse_option_value("--maxX", argv[i + 1], &options.max_x, &flags.max_x)) {
        print_usage(argv[0]);
        return -1;
      }
      i++;
      continue;
    }

    if (ascii_ply_starts_with(arg, "--maxX=")) {
      if (!parse_option_value(
              "--maxX",
              arg + strlen("--maxX="),
              &options.max_x,
              &flags.max_x)) {
        print_usage(argv[0]);
        return -1;
      }
      continue;
    }

    if (strcmp(arg, "--minY") == 0) {
      if (i + 1 >= argc ||
          !parse_option_value("--minY", argv[i + 1], &options.min_y, &flags.min_y)) {
        print_usage(argv[0]);
        return -1;
      }
      i++;
      continue;
    }

    if (ascii_ply_starts_with(arg, "--minY=")) {
      if (!parse_option_value(
              "--minY",
              arg + strlen("--minY="),
              &options.min_y,
              &flags.min_y)) {
        print_usage(argv[0]);
        return -1;
      }
      continue;
    }

    if (strcmp(arg, "--maxY") == 0) {
      if (i + 1 >= argc ||
          !parse_option_value("--maxY", argv[i + 1], &options.max_y, &flags.max_y)) {
        print_usage(argv[0]);
        return -1;
      }
      i++;
      continue;
    }

    if (ascii_ply_starts_with(arg, "--maxY=")) {
      if (!parse_option_value(
              "--maxY",
              arg + strlen("--maxY="),
              &options.max_y,
              &flags.max_y)) {
        print_usage(argv[0]);
        return -1;
      }
      continue;
    }

    if (strcmp(arg, "--minZ") == 0) {
      if (i + 1 >= argc ||
          !parse_option_value("--minZ", argv[i + 1], &options.min_z, &flags.min_z)) {
        print_usage(argv[0]);
        return -1;
      }
      i++;
      continue;
    }

    if (ascii_ply_starts_with(arg, "--minZ=")) {
      if (!parse_option_value(
              "--minZ",
              arg + strlen("--minZ="),
              &options.min_z,
              &flags.min_z)) {
        print_usage(argv[0]);
        return -1;
      }
      continue;
    }

    if (strcmp(arg, "--maxZ") == 0) {
      if (i + 1 >= argc ||
          !parse_option_value("--maxZ", argv[i + 1], &options.max_z, &flags.max_z)) {
        print_usage(argv[0]);
        return -1;
      }
      i++;
      continue;
    }

    if (ascii_ply_starts_with(arg, "--maxZ=")) {
      if (!parse_option_value(
              "--maxZ",
              arg + strlen("--maxZ="),
              &options.max_z,
              &flags.max_z)) {
        print_usage(argv[0]);
        return -1;
      }
      continue;
    }

    if (arg[0] == '-') {
      fprintf(stderr, "Unknown option: %s\n", arg);
      print_usage(argv[0]);
      return -1;
    }

    if (options.input_path == NULL) {
      options.input_path = arg;
    } else if (options.output_path == NULL) {
      options.output_path = arg;
    } else {
      print_usage(argv[0]);
      return -1;
    }
  }

  if (options.input_path == NULL ||
      options.output_path == NULL ||
      !flags.min_x ||
      !flags.max_x ||
      !flags.min_y ||
      !flags.max_y ||
      !flags.min_z ||
      !flags.max_z) {
    print_usage(argv[0]);
    return -1;
  }

  if (strcmp(options.input_path, options.output_path) == 0) {
    fprintf(stderr, "Input and output paths must be different.\n");
    return -1;
  }

  if (options.min_x > options.max_x ||
      options.min_y > options.max_y ||
      options.min_z > options.max_z) {
    fprintf(stderr, "Crop minimums must be less than or equal to maximums.\n");
    return -1;
  }

  *options_out = options;
  return 0;
}
