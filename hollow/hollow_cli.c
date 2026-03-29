#include "hollow_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hollow_toml.h"

void free_hollow_options(HollowOptions *options) {
  if (options == NULL) {
    return;
  }

  free((char *) options->input_dir);
  free((char *) options->output_dir);
  free(options->colors_for_removal);
  memset(options, 0, sizeof(*options));
}

static void print_usage(const char *program_name) {
  fprintf(
      stderr,
      "Usage: %s --config <hollow.toml>\n"
      "\n"
      "Options:\n"
      "  --config PATH   Path to the hollow TOML config file\n"
      "  --help          Show this help message\n"
      "\n"
      "Notes:\n"
      "  - The config must declare input/output directories.\n"
      "  - The input directory must contain out_<layer>.png files.\n"
      "  - colors_for_removal and destination_color accept [r,g,b] or [r,g,b,a].\n",
      program_name);
}

int parse_hollow_options(int argc, char **argv, HollowOptions *options_out) {
  const char *config_path = NULL;

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
      config_path = argv[++i];
      continue;
    }

    if (strncmp(arg, "--config=", 9) == 0) {
      config_path = arg + 9;
      continue;
    }

    fprintf(stderr, "Unknown argument: %s\n", arg);
    print_usage(argv[0]);
    return -1;
  }

  if (config_path == NULL) {
    fprintf(stderr, "A TOML config file is required.\n");
    print_usage(argv[0]);
    return -1;
  }

  HollowOptions options;
  if (load_hollow_config_file(config_path, &options) != 0) {
    return -1;
  }

  options.config_path = config_path;
  *options_out = options;
  return 0;
}
