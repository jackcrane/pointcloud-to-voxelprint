#include "shadow_cli.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shadow_toml.h"

static void print_usage(const char *program_name);
static int parse_color(const char *text, ShadowColor *color_out);
static int parse_u8_component(const char *text, const char *label, uint8_t *value_out);
static char *dup_string(const char *text);

void free_shadow_options(ShadowOptions *options) {
  if (options == NULL) {
    return;
  }

  free((char *) options->config_path);
  free((char *) options->input_dir);
  free((char *) options->output_dir);
  memset(options, 0, sizeof(*options));
}

int parse_shadow_options(int argc, char **argv, ShadowOptions *options_out) {
  ShadowOptions options;
  memset(&options, 0, sizeof(options));
  options.set_color.a = 255u;
  options.replace_color.a = 255u;
  options.direction = SHADOW_FROM_BOTTOM;
  bool has_config = false;
  bool has_set_color = false;
  bool has_replace_color = false;
  bool has_from = false;

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
        free_shadow_options(&options);
        return -1;
      }
      free((char *) options.config_path);
      options.config_path = dup_string(argv[++i]);
      if (options.config_path == NULL) {
        fprintf(stderr, "Failed to allocate config path.\n");
        free_shadow_options(&options);
        return -1;
      }
      has_config = true;
      continue;
    }

    if (strncmp(arg, "--config=", 9) == 0) {
      free((char *) options.config_path);
      options.config_path = dup_string(arg + 9);
      if (options.config_path == NULL) {
        fprintf(stderr, "Failed to allocate config path.\n");
        free_shadow_options(&options);
        return -1;
      }
      has_config = true;
      continue;
    }

    if (strcmp(arg, "--setColor") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "Missing value for --setColor.\n");
        print_usage(argv[0]);
        free_shadow_options(&options);
        return -1;
      }
      if (parse_color(argv[++i], &options.set_color) != 0) {
        free_shadow_options(&options);
        return -1;
      }
      has_set_color = true;
      continue;
    }

    if (strncmp(arg, "--setColor=", 11) == 0) {
      if (parse_color(arg + 11, &options.set_color) != 0) {
        free_shadow_options(&options);
        return -1;
      }
      has_set_color = true;
      continue;
    }

    if (strcmp(arg, "--replaceColor") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "Missing value for --replaceColor.\n");
        print_usage(argv[0]);
        free_shadow_options(&options);
        return -1;
      }
      if (parse_color(argv[++i], &options.replace_color) != 0) {
        free_shadow_options(&options);
        return -1;
      }
      has_replace_color = true;
      continue;
    }

    if (strncmp(arg, "--replaceColor=", 15) == 0) {
      if (parse_color(arg + 15, &options.replace_color) != 0) {
        free_shadow_options(&options);
        return -1;
      }
      has_replace_color = true;
      continue;
    }

    if (strcmp(arg, "--from") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "Missing value for --from.\n");
        print_usage(argv[0]);
        free_shadow_options(&options);
        return -1;
      }
      arg = argv[++i];
      if (strcmp(arg, "bottom") == 0) {
        options.direction = SHADOW_FROM_BOTTOM;
      } else if (strcmp(arg, "top") == 0) {
        options.direction = SHADOW_FROM_TOP;
      } else {
        fprintf(stderr, "Invalid --from value: %s\n", arg);
        print_usage(argv[0]);
        free_shadow_options(&options);
        return -1;
      }
      has_from = true;
      continue;
    }

    if (strncmp(arg, "--from=", 7) == 0) {
      const char *value = arg + 7;
      if (strcmp(value, "bottom") == 0) {
        options.direction = SHADOW_FROM_BOTTOM;
      } else if (strcmp(value, "top") == 0) {
        options.direction = SHADOW_FROM_TOP;
      } else {
        fprintf(stderr, "Invalid --from value: %s\n", value);
        print_usage(argv[0]);
        free_shadow_options(&options);
        return -1;
      }
      has_from = true;
      continue;
    }

    if (arg[0] == '-') {
      fprintf(stderr, "Unknown argument: %s\n", arg);
      print_usage(argv[0]);
      free_shadow_options(&options);
      return -1;
    }

    if (options.input_dir == NULL) {
      options.input_dir = dup_string(arg);
      if (options.input_dir == NULL) {
        fprintf(stderr, "Failed to allocate input path.\n");
        free_shadow_options(&options);
        return -1;
      }
      continue;
    }
    if (options.output_dir == NULL) {
      options.output_dir = dup_string(arg);
      if (options.output_dir == NULL) {
        fprintf(stderr, "Failed to allocate output path.\n");
        free_shadow_options(&options);
        return -1;
      }
      continue;
    }

    fprintf(stderr, "Unexpected positional argument: %s\n", arg);
    print_usage(argv[0]);
    free_shadow_options(&options);
    return -1;
  }

  if (has_config) {
    ShadowOptions loaded;
    if (load_shadow_config_file(options.config_path, &loaded) != 0) {
      free_shadow_options(&options);
      return -1;
    }

    free((char *) loaded.config_path);
    loaded.config_path = dup_string(options.config_path);
    if (loaded.config_path == NULL) {
      fprintf(stderr, "Failed to allocate config path.\n");
      free_shadow_options(&loaded);
      free_shadow_options(&options);
      return -1;
    }

    if (options.input_dir != NULL) {
      free((char *) loaded.input_dir);
      loaded.input_dir = (char *) options.input_dir;
      options.input_dir = NULL;
    }
    if (options.output_dir != NULL) {
      free((char *) loaded.output_dir);
      loaded.output_dir = (char *) options.output_dir;
      options.output_dir = NULL;
    }
    if (has_set_color) {
      loaded.set_color = options.set_color;
    }
    if (has_replace_color) {
      loaded.replace_color = options.replace_color;
    }
    if (has_from) {
      loaded.direction = options.direction;
    }

    free_shadow_options(&options);
    options = loaded;
  }

  if (options.input_dir == NULL || options.output_dir == NULL) {
    fprintf(stderr, has_config
                        ? "The config and overrides must provide input and output directories.\n"
                        : "<input_dir> and <output_dir> are required.\n");
    print_usage(argv[0]);
    free_shadow_options(&options);
    return -1;
  }
  if (!has_config && (!has_set_color || !has_replace_color || !has_from)) {
    fprintf(stderr, "--setColor, --replaceColor, and --from are required unless --config is used.\n");
    print_usage(argv[0]);
    free_shadow_options(&options);
    return -1;
  }

  *options_out = options;
  return 0;
}

static void print_usage(const char *program_name) {
  fprintf(
      stderr,
      "Usage: %s [--config <shadow.toml>] [--setColor <r,g,b[,a]>] [--replaceColor <r,g,b[,a]>] "
      "--from <top|bottom> <input_dir> <output_dir>\n"
      "\n"
      "Options:\n"
      "  --config PATH         Path to a shadow TOML config file\n"
      "  --setColor COLOR      Replacement color to paint into exposed pixels\n"
      "  --replaceColor COLOR  Source color to replace while a pixel remains exposed\n"
      "  --from DIR            Process layers from top or bottom\n"
      "  --help                Show this help message\n"
      "\n"
      "Notes:\n"
      "  - COLOR accepts r,g,b or r,g,b,a with 0-255 components.\n"
      "  - Command-line values override the config file.\n"
      "  - The input directory must contain out_<layer>.png files.\n"
      "  - If --from bottom is used, layer 0 is processed first.\n",
      program_name);
}

static int parse_color(const char *text, ShadowColor *color_out) {
  char *mutable_text = strdup(text);
  if (mutable_text == NULL) {
    fprintf(stderr, "Failed to allocate color buffer.\n");
    return -1;
  }

  ShadowColor color = {0u, 0u, 0u, 255u};
  char *save = NULL;
  char *parts[4] = {0};
  int count = 0;

  for (char *token = strtok_r(mutable_text, ",", &save);
       token != NULL && count < 4;
       token = strtok_r(NULL, ",", &save)) {
    parts[count++] = token;
  }

  if (strtok_r(NULL, ",", &save) != NULL || (count != 3 && count != 4)) {
    fprintf(stderr, "Invalid color value: %s\n", text);
    free(mutable_text);
    return -1;
  }

  if (parse_u8_component(parts[0], "red", &color.r) != 0 ||
      parse_u8_component(parts[1], "green", &color.g) != 0 ||
      parse_u8_component(parts[2], "blue", &color.b) != 0) {
    free(mutable_text);
    return -1;
  }
  if (count == 4 && parse_u8_component(parts[3], "alpha", &color.a) != 0) {
    free(mutable_text);
    return -1;
  }

  free(mutable_text);
  *color_out = color;
  return 0;
}

static int parse_u8_component(const char *text, const char *label, uint8_t *value_out) {
  errno = 0;
  char *end = NULL;
  const unsigned long value = strtoul(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value > 255u) {
    fprintf(stderr, "Invalid %s color component: %s\n", label, text);
    return -1;
  }

  *value_out = (uint8_t) value;
  return 0;
}

static char *dup_string(const char *text) {
  if (text == NULL) {
    return NULL;
  }

  const size_t length = strlen(text) + 1u;
  char *copy = malloc(length);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, text, length);
  return copy;
}
