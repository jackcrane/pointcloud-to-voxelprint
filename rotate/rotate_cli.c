#include "rotate_cli.h"

#include "../ascii_ply/ascii_ply_support.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *program_name) {
  fprintf(
      stderr,
      "Usage: %s [--log-interval N] <input.ply> <output.ply> "
      "--centroid X,Y,Z --angle DEGREES --about x|y|z\n",
      program_name);
}

static bool parse_axis_value(const char *value, RotateAxis *axis_out) {
  if (value == NULL || value[0] == '\0' || value[1] != '\0') {
    return false;
  }

  switch ((char)tolower((unsigned char)value[0])) {
    case 'x':
      *axis_out = ROTATE_AXIS_X;
      return true;
    case 'y':
      *axis_out = ROTATE_AXIS_Y;
      return true;
    case 'z':
      *axis_out = ROTATE_AXIS_Z;
      return true;
    default:
      return false;
  }
}

static bool parse_centroid_value(
    const char *value,
    double *x_out,
    double *y_out,
    double *z_out) {
  const char *cursor = value;
  char *end = NULL;

  double x = strtod(cursor, &end);
  if (end == cursor || *end != ',') {
    return false;
  }

  cursor = end + 1;
  double y = strtod(cursor, &end);
  if (end == cursor || *end != ',') {
    return false;
  }

  cursor = end + 1;
  double z = strtod(cursor, &end);
  if (end == cursor || *end != '\0') {
    return false;
  }

  *x_out = x;
  *y_out = y;
  *z_out = z;
  return true;
}

int parse_rotate_options(int argc, char **argv, RotateOptions *options_out) {
  RotateOptions options;
  memset(&options, 0, sizeof(options));
  options.log_interval = ASCII_PLY_DEFAULT_LOG_INTERVAL;

  bool saw_centroid = false;
  bool saw_angle = false;
  bool saw_axis = false;

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

    if (strcmp(arg, "--centroid") == 0) {
      if (i + 1 >= argc ||
          !parse_centroid_value(
              argv[i + 1],
              &options.centroid_x,
              &options.centroid_y,
              &options.centroid_z)) {
        fprintf(stderr, "Invalid value for --centroid.\n");
        print_usage(argv[0]);
        return -1;
      }
      saw_centroid = true;
      i++;
      continue;
    }

    if (ascii_ply_starts_with(arg, "--centroid=")) {
      if (!parse_centroid_value(
              arg + strlen("--centroid="),
              &options.centroid_x,
              &options.centroid_y,
              &options.centroid_z)) {
        fprintf(stderr, "Invalid value for --centroid.\n");
        print_usage(argv[0]);
        return -1;
      }
      saw_centroid = true;
      continue;
    }

    if (strcmp(arg, "--angle") == 0) {
      if (i + 1 >= argc ||
          !ascii_ply_parse_double_str(argv[i + 1], &options.angle_degrees)) {
        fprintf(stderr, "Invalid value for --angle.\n");
        print_usage(argv[0]);
        return -1;
      }
      saw_angle = true;
      i++;
      continue;
    }

    if (ascii_ply_starts_with(arg, "--angle=")) {
      if (!ascii_ply_parse_double_str(
              arg + strlen("--angle="),
              &options.angle_degrees)) {
        fprintf(stderr, "Invalid value for --angle.\n");
        print_usage(argv[0]);
        return -1;
      }
      saw_angle = true;
      continue;
    }

    if (strcmp(arg, "--about") == 0) {
      if (i + 1 >= argc || !parse_axis_value(argv[i + 1], &options.axis)) {
        fprintf(stderr, "Invalid value for --about.\n");
        print_usage(argv[0]);
        return -1;
      }
      saw_axis = true;
      i++;
      continue;
    }

    if (ascii_ply_starts_with(arg, "--about=")) {
      if (!parse_axis_value(arg + strlen("--about="), &options.axis)) {
        fprintf(stderr, "Invalid value for --about.\n");
        print_usage(argv[0]);
        return -1;
      }
      saw_axis = true;
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
      !saw_centroid ||
      !saw_angle ||
      !saw_axis) {
    print_usage(argv[0]);
    return -1;
  }

  if (strcmp(options.input_path, options.output_path) == 0) {
    fprintf(stderr, "Input and output paths must be different.\n");
    return -1;
  }

  *options_out = options;
  return 0;
}
