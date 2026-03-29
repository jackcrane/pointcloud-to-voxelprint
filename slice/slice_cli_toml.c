#include "slice_cli.h"

#include <stdio.h>
#include <string.h>

#include "slice_toml.h"

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

int parse_slice_options(int argc, char **argv, SliceOptions *options_out) {
  const char *config_path = NULL;
  const char *input_override = NULL;
  const char *output_override = NULL;

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

    if (arg[0] == '-') {
      fprintf(stderr, "Unknown option: %s\n", arg);
      print_usage(argv[0]);
      return -1;
    }

    if (input_override == NULL) {
      input_override = arg;
      continue;
    }
    if (output_override == NULL) {
      output_override = arg;
      continue;
    }

    fprintf(stderr, "Unexpected positional argument: %s\n", arg);
    print_usage(argv[0]);
    return -1;
  }

  if (config_path == NULL) {
    fprintf(stderr, "A TOML config file is required.\n");
    print_usage(argv[0]);
    return -1;
  }

  SliceOptions options;
  if (load_slice_config_file(config_path, &options) != 0) {
    return -1;
  }

  options.config_path = config_path;
  if (input_override != NULL) {
    options.input_path = input_override;
  }
  if (output_override != NULL) {
    options.output_dir = output_override;
  }

  if (options.input_path == NULL || options.output_dir == NULL) {
    fprintf(stderr, "Both input.path and output.directory must be set.\n");
    return -1;
  }

  *options_out = options;
  return 0;
}
