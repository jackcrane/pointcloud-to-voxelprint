#include "slice_cli.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
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
  bool radius_x_positive;
  bool radius_x_negative;
  bool radius_y_positive;
  bool radius_y_negative;
  bool radius_z_positive;
  bool radius_z_negative;
} SliceConfigFlags;

static void print_usage(const char *program_name) {
  fprintf(
      stderr,
      "Usage: %s --config <slice.toml> [input.ply] [output_dir]\n"
      "\n"
      "Options:\n"
      "  --config PATH   Path to the slice TOML config file\n"
      "  --help          Show this help message\n"
      "\n"
      "Notes:\n"
      "  - The TOML file holds slicer settings, including per-direction voxel radii.\n"
      "  - Positional input/output paths are optional CLI overrides for the TOML file.\n"
      "  - See slice/slice.example.toml for a documented example.\n",
      program_name);
}

static char *slice_strdup(const char *text) {
  const size_t len = strlen(text);
  char *copy = malloc(len + 1);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, text, len + 1);
  return copy;
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
  errno = 0;
  char *end = NULL;
  const double parsed = strtod(value, &end);
  if (value == end || end == NULL || *end != '\0' || errno == ERANGE) {
    return false;
  }
  *out = parsed;
  return true;
}

static char *trim_whitespace(char *text) {
  while (*text != '\0' && isspace((unsigned char) *text)) {
    ++text;
  }

  size_t len = strlen(text);
  while (len > 0 && isspace((unsigned char) text[len - 1])) {
    text[len - 1] = '\0';
    --len;
  }
  return text;
}

static void strip_comment(char *line) {
  bool in_quotes = false;
  for (char *cursor = line; *cursor != '\0'; ++cursor) {
    if (*cursor == '"' && (cursor == line || cursor[-1] != '\\')) {
      in_quotes = !in_quotes;
      continue;
    }
    if (*cursor == '#' && !in_quotes) {
      *cursor = '\0';
      return;
    }
  }
}

static bool parse_toml_string_value(
    const char *raw_value,
    const char *field_name,
    char **value_out) {
  size_t len = strlen(raw_value);
  if (len < 2 || raw_value[0] != '"' || raw_value[len - 1] != '"') {
    fprintf(stderr, "TOML field %s must be a quoted string.\n", field_name);
    return false;
  }

  char *result = malloc(len - 1);
  if (result == NULL) {
    fprintf(stderr, "Out of memory while parsing %s.\n", field_name);
    return false;
  }

  size_t write_index = 0;
  for (size_t i = 1; i + 1 < len; ++i) {
    char ch = raw_value[i];
    if (ch == '\\' && i + 1 < len - 1) {
      const char next = raw_value[++i];
      if (next == '"' || next == '\\') {
        ch = next;
      } else if (next == 'n') {
        ch = '\n';
      } else if (next == 't') {
        ch = '\t';
      } else {
        fprintf(stderr, "Unsupported escape sequence in %s.\n", field_name);
        free(result);
        return false;
      }
    }
    result[write_index++] = ch;
  }
  result[write_index] = '\0';

  free(*value_out);
  *value_out = result;
  return true;
}

static bool parse_positive_double_option(
    const char *option_name,
    const char *value,
    double *field_out) {
  double parsed = 0.0;
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
  if (!parse_double_str(value, &parsed) || parsed < 0.0) {
    fprintf(stderr, "Invalid value for %s.\n", option_name);
    return false;
  }
  *field_out = parsed;
  return true;
}

static bool parse_dimension_value(
    const char *field_name,
    const char *value,
    double *field_out,
    bool *explicit_out,
    bool *auto_out) {
  if (strcmp(value, "auto") == 0 || strcmp(value, "\"auto\"") == 0) {
    *field_out = 0.0;
    *explicit_out = false;
    *auto_out = true;
    return true;
  }

  double parsed = 0.0;
  if (!parse_double_str(value, &parsed) || parsed <= 0.0) {
    fprintf(stderr, "Invalid value for %s.\n", field_name);
    return false;
  }

  *field_out = parsed;
  *explicit_out = true;
  *auto_out = false;
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
  options.voxel_radius_x_positive_inches =
      SLICE_DEFAULT_VOXEL_RADIUS_INCHES * SLICE_DEFAULT_MULTIPLIER;
  options.voxel_radius_x_negative_inches =
      SLICE_DEFAULT_VOXEL_RADIUS_INCHES * SLICE_DEFAULT_MULTIPLIER;
  options.voxel_radius_y_positive_inches =
      SLICE_DEFAULT_VOXEL_RADIUS_INCHES * SLICE_DEFAULT_MULTIPLIER;
  options.voxel_radius_y_negative_inches =
      SLICE_DEFAULT_VOXEL_RADIUS_INCHES * SLICE_DEFAULT_MULTIPLIER;
  options.voxel_radius_z_positive_inches =
      SLICE_DEFAULT_VOXEL_RADIUS_INCHES * SLICE_DEFAULT_MULTIPLIER;
  options.voxel_radius_z_negative_inches =
      SLICE_DEFAULT_VOXEL_RADIUS_INCHES * SLICE_DEFAULT_MULTIPLIER;
  options.padding_ratio = SLICE_DEFAULT_PADDING_RATIO;
  options.log_interval = SLICE_DEFAULT_LOG_INTERVAL;
  return options;
}

static bool apply_config_value(
    SliceOptions *options,
    SliceConfigFlags *flags,
    const char *section,
    const char *key,
    const char *value) {
  if (strcmp(section, "input") == 0 && strcmp(key, "path") == 0) {
    return parse_toml_string_value(value, "input.path", (char **) &options->input_path);
  }

  if (strcmp(section, "output") == 0 && strcmp(key, "directory") == 0) {
    return parse_toml_string_value(value, "output.directory", (char **) &options->output_dir);
  }

  if (strcmp(section, "raster") == 0 && strcmp(key, "dpi") == 0) {
    return parse_positive_double_option("raster.dpi", value, &options->dpi);
  }
  if (strcmp(section, "raster") == 0 && strcmp(key, "layer_height_nm") == 0) {
    return parse_positive_double_option(
        "raster.layer_height_nm",
        value,
        &options->layer_height_nm);
  }

  if (strcmp(section, "physical") == 0 && strcmp(key, "multiplier") == 0) {
    flags->multiplier = true;
    return parse_positive_double_option("physical.multiplier", value, &options->multiplier);
  }
  if (strcmp(section, "physical") == 0 && strcmp(key, "x_in") == 0) {
    flags->x_in = true;
    return parse_dimension_value(
        "physical.x_in",
        value,
        &options->x_in,
        &options->x_in_set,
        &flags->x_auto);
  }
  if (strcmp(section, "physical") == 0 && strcmp(key, "y_in") == 0) {
    flags->y_in = true;
    return parse_dimension_value(
        "physical.y_in",
        value,
        &options->y_in,
        &options->y_in_set,
        &flags->y_auto);
  }
  if (strcmp(section, "physical") == 0 && strcmp(key, "z_in") == 0) {
    flags->z_in = true;
    return parse_dimension_value(
        "physical.z_in",
        value,
        &options->z_in,
        &options->z_in_set,
        &flags->z_auto);
  }
  if (strcmp(section, "physical") == 0 && strcmp(key, "longest_side_in") == 0) {
    return parse_positive_double_option(
        "physical.longest_side_in",
        value,
        &options->longest_side_in);
  }
  if (strcmp(section, "physical") == 0 && strcmp(key, "padding_ratio") == 0) {
    return parse_non_negative_double_option(
        "physical.padding_ratio",
        value,
        &options->padding_ratio);
  }

  if (strcmp(section, "sampling.radius_inches") == 0 &&
      strcmp(key, "x_positive") == 0) {
    flags->radius_x_positive = true;
    return parse_positive_double_option(
        "sampling.radius_inches.x_positive",
        value,
        &options->voxel_radius_x_positive_inches);
  }
  if (strcmp(section, "sampling.radius_inches") == 0 &&
      strcmp(key, "x_negative") == 0) {
    flags->radius_x_negative = true;
    return parse_positive_double_option(
        "sampling.radius_inches.x_negative",
        value,
        &options->voxel_radius_x_negative_inches);
  }
  if (strcmp(section, "sampling.radius_inches") == 0 &&
      strcmp(key, "y_positive") == 0) {
    flags->radius_y_positive = true;
    return parse_positive_double_option(
        "sampling.radius_inches.y_positive",
        value,
        &options->voxel_radius_y_positive_inches);
  }
  if (strcmp(section, "sampling.radius_inches") == 0 &&
      strcmp(key, "y_negative") == 0) {
    flags->radius_y_negative = true;
    return parse_positive_double_option(
        "sampling.radius_inches.y_negative",
        value,
        &options->voxel_radius_y_negative_inches);
  }
  if (strcmp(section, "sampling.radius_inches") == 0 &&
      strcmp(key, "z_positive") == 0) {
    flags->radius_z_positive = true;
    return parse_positive_double_option(
        "sampling.radius_inches.z_positive",
        value,
        &options->voxel_radius_z_positive_inches);
  }
  if (strcmp(section, "sampling.radius_inches") == 0 &&
      strcmp(key, "z_negative") == 0) {
    flags->radius_z_negative = true;
    return parse_positive_double_option(
        "sampling.radius_inches.z_negative",
        value,
        &options->voxel_radius_z_negative_inches);
  }

  if (strcmp(section, "logging") == 0 && strcmp(key, "interval") == 0) {
    if (!parse_uint64_str(value, &options->log_interval)) {
      fprintf(stderr, "Invalid value for logging.interval.\n");
      return false;
    }
    return true;
  }

  fprintf(stderr, "Unknown TOML key [%s] %s.\n", section, key);
  return false;
}

static bool load_slice_config(
    const char *path,
    SliceOptions *options,
    SliceConfigFlags *flags) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    fprintf(stderr, "Failed to open config file: %s\n", path);
    return false;
  }

  char line[4096];
  char section[128] = "";
  size_t line_number = 0;

  while (fgets(line, sizeof(line), file) != NULL) {
    ++line_number;
    strip_comment(line);
    char *trimmed = trim_whitespace(line);
    if (*trimmed == '\0') {
      continue;
    }

    if (*trimmed == '[') {
      const size_t len = strlen(trimmed);
      if (len < 3 || trimmed[len - 1] != ']') {
        fprintf(stderr, "Invalid TOML section header at line %zu.\n", line_number);
        fclose(file);
        return false;
      }
      trimmed[len - 1] = '\0';
      const char *raw_section = trim_whitespace(trimmed + 1);
      if (*raw_section == '\0' || strlen(raw_section) >= sizeof(section)) {
        fprintf(stderr, "Invalid TOML section header at line %zu.\n", line_number);
        fclose(file);
        return false;
      }
      snprintf(section, sizeof(section), "%s", raw_section);
      continue;
    }

    char *equals = NULL;
    bool in_quotes = false;
    for (char *cursor = trimmed; *cursor != '\0'; ++cursor) {
      if (*cursor == '"' && (cursor == trimmed || cursor[-1] != '\\')) {
        in_quotes = !in_quotes;
        continue;
      }
      if (*cursor == '=' && !in_quotes) {
        equals = cursor;
        break;
      }
    }
    if (equals == NULL) {
      fprintf(stderr, "Invalid TOML assignment at line %zu.\n", line_number);
      fclose(file);
      return false;
    }

    *equals = '\0';
    char *key = trim_whitespace(trimmed);
    char *value = trim_whitespace(equals + 1);
    if (*section == '\0' || *key == '\0' || *value == '\0') {
      fprintf(stderr, "Invalid TOML assignment at line %zu.\n", line_number);
      fclose(file);
      return false;
    }

    if (!apply_config_value(options, flags, section, key, value)) {
      fprintf(stderr, "Config parse failed at line %zu.\n", line_number);
      fclose(file);
      return false;
    }
  }

  fclose(file);
  return true;
}

static void apply_multiplier_defaults(SliceOptions *options, const SliceConfigFlags *flags) {
  if (!flags->multiplier) {
    return;
  }

  if (!flags->x_in && !flags->x_auto) {
    options->x_in = SLICE_BASE_X_IN * options->multiplier;
    options->x_in_set = true;
  }
  if (!flags->y_in && !flags->y_auto) {
    options->y_in = SLICE_BASE_Y_IN * options->multiplier;
    options->y_in_set = true;
  }
  if (!flags->z_in && !flags->z_auto) {
    options->z_in = SLICE_BASE_Z_IN * options->multiplier;
    options->z_in_set = true;
  }

  const double default_radius = SLICE_DEFAULT_VOXEL_RADIUS_INCHES * options->multiplier;
  if (!flags->radius_x_positive) {
    options->voxel_radius_x_positive_inches = default_radius;
  }
  if (!flags->radius_x_negative) {
    options->voxel_radius_x_negative_inches = default_radius;
  }
  if (!flags->radius_y_positive) {
    options->voxel_radius_y_positive_inches = default_radius;
  }
  if (!flags->radius_y_negative) {
    options->voxel_radius_y_negative_inches = default_radius;
  }
  if (!flags->radius_z_positive) {
    options->voxel_radius_z_positive_inches = default_radius;
  }
  if (!flags->radius_z_negative) {
    options->voxel_radius_z_negative_inches = default_radius;
  }
}

int parse_slice_options(int argc, char **argv, SliceOptions *options_out) {
  SliceOptions options = default_options();
  SliceConfigFlags flags;
  memset(&flags, 0, sizeof(flags));

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      print_usage(argv[0]);
      return -1;
    }

    if (strcmp(arg, "--config") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "Missing value for --config.\n");
        print_usage(argv[0]);
        return -1;
      }
      options.config_path = argv[++i];
      continue;
    }

    if (strncmp(arg, "--config=", 9) == 0) {
      options.config_path = arg + 9;
      continue;
    }

    if (arg[0] == '-') {
      fprintf(stderr, "Unknown option: %s\n", arg);
      print_usage(argv[0]);
      return -1;
    }

    if (options.input_path == NULL) {
      options.input_path = arg;
      continue;
    }
    if (options.output_dir == NULL) {
      options.output_dir = arg;
      continue;
    }

    fprintf(stderr, "Unexpected positional argument: %s\n", arg);
    print_usage(argv[0]);
    return -1;
  }

  if (options.config_path == NULL) {
    fprintf(stderr, "A TOML config file is required.\n");
    print_usage(argv[0]);
    return -1;
  }

  if (!load_slice_config(options.config_path, &options, &flags)) {
    return -1;
  }

  apply_multiplier_defaults(&options, &flags);

  if (options.input_path == NULL || options.output_dir == NULL) {
    fprintf(stderr, "Both input.path and output.directory must be set.\n");
    return -1;
  }

  *options_out = options;
  return 0;
}
