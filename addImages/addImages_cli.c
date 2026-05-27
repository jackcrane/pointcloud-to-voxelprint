#include "addImages_cli.h"

#include <stdio.h>
#include <string.h>

#include "addImages_toml.h"

static void print_usage(const char *program_name) {
  fprintf(
      stderr,
      "Usage: %s --config <addImage.toml>\n"
      "\n"
      "Options:\n"
      "  --config PATH   Path to the addImages TOML config file\n"
      "  --help          Show this help message\n"
      "\n"
      "Notes:\n"
      "  - The input directory must contain out_<layer>.png files.\n"
      "  - Each [[image]] entry needs src, x, y, and width or height.\n"
      "  - invert=true mirrors that image horizontally before placement.\n",
      program_name);
}

int parse_add_images_options(int argc, char **argv, AddImagesOptions *options_out) {
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

  AddImagesOptions options;
  if (load_add_images_config_file(config_path, &options) != 0) {
    return -1;
  }

  options.config_path = config_path;
  *options_out = options;
  return 0;
}
