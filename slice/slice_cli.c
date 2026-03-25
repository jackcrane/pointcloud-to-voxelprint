#include "slice_cli.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  bool multiplier;
  bool x_in;
  bool y_in;
  bool z_in;
  bool x_auto;
  bool y_auto;
  bool z_auto;
  bool voxel_radius_inches;
} SliceOptionFlags;

static void print_usage(const char *program_name) {
  fprintf(
      stderr,
      "Usage: %s [options] <input.ply> <output_dir>\n"
      "\n"
      "Options:\n"
      "  --log-interval N              Loader progress interval in points (default %" PRIu64 ")\n"
      "  --dpi VALUE                   XY dots per inch (default %.0f)\n"
      "  --layer-height-nm VALUE       Z layer height in nanometers (default %.0f)\n"
      "  --multiplier VALUE            Scales the default build size and voxel radius (default %.2f)\n"
      "  --x-in VALUE|auto             Physical X size in inches (default %.3f)\n"
      "  --y-in VALUE|auto             Physical Y size in inches (default %.3f)\n"
      "  --z-in VALUE|auto             Physical Z size in inches (default %.3f)\n"
      "  --longest-side-in VALUE       Longest-side fallback when any dimension is auto (default %.3f)\n"
      "  --voxel-radius-inches VALUE   Match radius in inches (default %.4f)\n"
      "  --padding-ratio VALUE         Fractional XY padding applied to bounds (default %.3f)\n"
      "  --help                        Show this help message\n",
      program_name,
      SLICE_DEFAULT_LOG_INTERVAL,
      SLICE_DEFAULT_DPI,
      SLICE_DEFAULT_LAYER_HEIGHT_NM,
      SLICE_DEFAULT_MULTIPLIER,
      SLICE_BASE_X_IN,
      SLICE_BASE_Y_IN,
      SLICE_BASE_Z_IN,
      SLICE_DEFAULT_LONGEST_SIDE_IN,
      SLICE_DEFAULT_VOXEL_RADIUS_INCHES,
      SLICE_DEFAULT_PADDING_RATIO);
}

static bool parse_uint64_str(const char *text, uint64_t *value_out) {
  errno = 0;
  char *end = NULL;
  unsigned long long value = strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') {
    return false;
  }

  *value_out = (uint64_t) value;
  return true;
}

static bool parse_double_str(const char *value, double *out) {
  char *end = NULL;
  double parsed = strtod(value, &end);
  if (value == end || end == NULL || *end != '\0' || errno == ERANGE) {
    return false;
  }
  *out = parsed;
  return true;
}

static bool parse_dimension_value(
    const char *option_name,
    const char *value,
    double *field_out,
    bool *explicit_out,
    bool *auto_out) {
  if (strcmp(value, "auto") == 0) {
    *field_out = 0.0;
    *explicit_out = false;
    *auto_out = true;
    return true;
  }

  double parsed = 0.0;
  errno = 0;
  if (!parse_double_str(value, &parsed) || parsed <= 0.0) {
    fprintf(stderr, "Invalid value for %s.\n", option_name);
    return false;
  }

  *field_out = parsed;
  *explicit_out = true;
  *auto_out = false;
  return true;
}

static bool parse_positive_double_option(
    const char *option_name,
    const char *value,
    double *field_out) {
  double parsed = 0.0;
  errno = 0;
  if (!parse_double_str(value, &parsed) || parsed <= 0.0) {
    fprintf(stderr, "Invalid value for %s.\n", option_name);
    return false;
  }
  *field_out = parsed;
  return true;
}

static bool parse_non_negative_double_option(
    const char *option_name,
    const char *value,
    double *field_out) {
  double parsed = 0.0;
  errno = 0;
  if (!parse_double_str(value, &parsed) || parsed < 0.0) {
    fprintf(stderr, "Invalid value for %s.\n", option_name);
    return false;
  }
  *field_out = parsed;
  return true;
}

static SliceOptions default_options(void) {
  SliceOptions options;
  memset(&options, 0, sizeof(options));
  options.dpi = SLICE_DEFAULT_DPI;
  options.layer_height_nm = SLICE_DEFAULT_LAYER_HEIGHT_NM;
  options.multiplier = SLICE_DEFAULT_MULTIPLIER;
  options.x_in = SLICE_BASE_X_IN * SLICE_DEFAULT_MULTIPLIER;
  options.y_in = SLICE_BASE_Y_IN * SLICE_DEFAULT_MULTIPLIER;
  options.z_in = SLICE_BASE_Z_IN * SLICE_DEFAULT_MULTIPLIER;
  options.x_in_set = true;
  options.y_in_set = true;
  options.z_in_set = true;
  options.longest_side_in = SLICE_DEFAULT_LONGEST_SIDE_IN;
  options.voxel_radius_inches =
      SLICE_DEFAULT_VOXEL_RADIUS_INCHES * SLICE_DEFAULT_MULTIPLIER;
  options.padding_ratio = SLICE_DEFAULT_PADDING_RATIO;
  options.log_interval = SLICE_DEFAULT_LOG_INTERVAL;
  return options;
}

int parse_slice_options(int argc, char **argv, SliceOptions *options_out) {
  SliceOptions options = default_options();
  SliceOptionFlags flags;
  memset(&flags, 0, sizeof(flags));

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];

    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      print_usage(argv[0]);
      return -1;
    }

    if (strcmp(arg, "--log-interval") == 0) {
      if (i + 1 >= argc || !parse_uint64_str(argv[i + 1], &options.log_interval)) {
        fprintf(stderr, "Invalid value for --log-interval.\n");
        print_usage(argv[0]);
        return -1;
      }
      ++i;
      continue;
    }
    if (strncmp(arg, "--log-interval=", 15) == 0) {
      if (!parse_uint64_str(arg + 15, &options.log_interval)) {
        fprintf(stderr, "Invalid value for --log-interval.\n");
        print_usage(argv[0]);
        return -1;
      }
      continue;
    }

    if (strcmp(arg, "--dpi") == 0) {
      if (i + 1 >= argc ||
          !parse_positive_double_option("--dpi", argv[i + 1], &options.dpi)) {
        print_usage(argv[0]);
        return -1;
      }
      ++i;
      continue;
    }
    if (strncmp(arg, "--dpi=", 6) == 0) {
      if (!parse_positive_double_option("--dpi", arg + 6, &options.dpi)) {
        print_usage(argv[0]);
        return -1;
      }
      continue;
    }

    if (strcmp(arg, "--layer-height-nm") == 0) {
      if (i + 1 >= argc ||
          !parse_positive_double_option(
              "--layer-height-nm",
              argv[i + 1],
              &options.layer_height_nm)) {
        print_usage(argv[0]);
        return -1;
      }
      ++i;
      continue;
    }
    if (strncmp(arg, "--layer-height-nm=", 18) == 0) {
      if (!parse_positive_double_option(
              "--layer-height-nm",
              arg + 18,
              &options.layer_height_nm)) {
        print_usage(argv[0]);
        return -1;
      }
      continue;
    }

    if (strcmp(arg, "--multiplier") == 0) {
      if (i + 1 >= argc ||
          !parse_positive_double_option(
              "--multiplier",
              argv[i + 1],
              &options.multiplier)) {
        print_usage(argv[0]);
        return -1;
      }
      flags.multiplier = true;
      ++i;
      continue;
    }
    if (strncmp(arg, "--multiplier=", 13) == 0) {
      if (!parse_positive_double_option(
              "--multiplier",
              arg + 13,
              &options.multiplier)) {
        print_usage(argv[0]);
        return -1;
      }
      flags.multiplier = true;
      continue;
    }

    if (strcmp(arg, "--x-in") == 0) {
      if (i + 1 >= argc ||
          !parse_dimension_value(
              "--x-in",
              argv[i + 1],
              &options.x_in,
              &options.x_in_set,
              &flags.x_auto)) {
        print_usage(argv[0]);
        return -1;
      }
      flags.x_in = true;
      ++i;
      continue;
    }
    if (strncmp(arg, "--x-in=", 7) == 0) {
      if (!parse_dimension_value(
              "--x-in",
              arg + 7,
              &options.x_in,
              &options.x_in_set,
              &flags.x_auto)) {
        print_usage(argv[0]);
        return -1;
      }
      flags.x_in = true;
      continue;
    }

    if (strcmp(arg, "--y-in") == 0) {
      if (i + 1 >= argc ||
          !parse_dimension_value(
              "--y-in",
              argv[i + 1],
              &options.y_in,
              &options.y_in_set,
              &flags.y_auto)) {
        print_usage(argv[0]);
        return -1;
      }
      flags.y_in = true;
      ++i;
      continue;
    }
    if (strncmp(arg, "--y-in=", 7) == 0) {
      if (!parse_dimension_value(
              "--y-in",
              arg + 7,
              &options.y_in,
              &options.y_in_set,
              &flags.y_auto)) {
        print_usage(argv[0]);
        return -1;
      }
      flags.y_in = true;
      continue;
    }

    if (strcmp(arg, "--z-in") == 0) {
      if (i + 1 >= argc ||
          !parse_dimension_value(
              "--z-in",
              argv[i + 1],
              &options.z_in,
              &options.z_in_set,
              &flags.z_auto)) {
        print_usage(argv[0]);
        return -1;
      }
      flags.z_in = true;
      ++i;
      continue;
    }
    if (strncmp(arg, "--z-in=", 7) == 0) {
      if (!parse_dimension_value(
              "--z-in",
              arg + 7,
              &options.z_in,
              &options.z_in_set,
              &flags.z_auto)) {
        print_usage(argv[0]);
        return -1;
      }
      flags.z_in = true;
      continue;
    }

    if (strcmp(arg, "--longest-side-in") == 0) {
      if (i + 1 >= argc ||
          !parse_positive_double_option(
              "--longest-side-in",
              argv[i + 1],
              &options.longest_side_in)) {
        print_usage(argv[0]);
        return -1;
      }
      ++i;
      continue;
    }
    if (strncmp(arg, "--longest-side-in=", 18) == 0) {
      if (!parse_positive_double_option(
              "--longest-side-in",
              arg + 18,
              &options.longest_side_in)) {
        print_usage(argv[0]);
        return -1;
      }
      continue;
    }

    if (strcmp(arg, "--voxel-radius-inches") == 0) {
      if (i + 1 >= argc ||
          !parse_positive_double_option(
              "--voxel-radius-inches",
              argv[i + 1],
              &options.voxel_radius_inches)) {
        print_usage(argv[0]);
        return -1;
      }
      flags.voxel_radius_inches = true;
      ++i;
      continue;
    }
    if (strncmp(arg, "--voxel-radius-inches=", 22) == 0) {
      if (!parse_positive_double_option(
              "--voxel-radius-inches",
              arg + 22,
              &options.voxel_radius_inches)) {
        print_usage(argv[0]);
        return -1;
      }
      flags.voxel_radius_inches = true;
      continue;
    }

    if (strcmp(arg, "--padding-ratio") == 0) {
      if (i + 1 >= argc ||
          !parse_non_negative_double_option(
              "--padding-ratio",
              argv[i + 1],
              &options.padding_ratio)) {
        print_usage(argv[0]);
        return -1;
      }
      ++i;
      continue;
    }
    if (strncmp(arg, "--padding-ratio=", 16) == 0) {
      if (!parse_non_negative_double_option(
              "--padding-ratio",
              arg + 16,
              &options.padding_ratio)) {
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
    } else if (options.output_dir == NULL) {
      options.output_dir = arg;
    } else {
      print_usage(argv[0]);
      return -1;
    }
  }

  if (flags.multiplier) {
    if (!flags.x_in && !flags.x_auto) {
      options.x_in = SLICE_BASE_X_IN * options.multiplier;
      options.x_in_set = true;
    }
    if (!flags.y_in && !flags.y_auto) {
      options.y_in = SLICE_BASE_Y_IN * options.multiplier;
      options.y_in_set = true;
    }
    if (!flags.z_in && !flags.z_auto) {
      options.z_in = SLICE_BASE_Z_IN * options.multiplier;
      options.z_in_set = true;
    }
    if (!flags.voxel_radius_inches) {
      options.voxel_radius_inches =
          SLICE_DEFAULT_VOXEL_RADIUS_INCHES * options.multiplier;
    }
  }

  if (options.input_path == NULL || options.output_dir == NULL) {
    print_usage(argv[0]);
    return -1;
  }

  *options_out = options;
  return 0;
}
