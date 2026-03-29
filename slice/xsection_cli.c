#include "xsection_cli.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "slice_toml.h"

static void print_usage(const char *program_name) {
  fprintf(
      stderr,
      "Usage: %s --config <slice.toml> --plane <xz|yz> --dist <index> <output.png>\n"
      "\n"
      "Options:\n"
      "  --config PATH   Path to the slice TOML config file\n"
      "  --plane VALUE   Cross-section plane: xz or yz\n"
      "  --dist INDEX    Source row/column index counted from the top-left\n"
      "  --help          Show this help message\n",
      program_name);
}

static int parse_plane(const char *value, XSectionPlane *plane_out) {
  if (strcmp(value, "xz") == 0) {
    *plane_out = XSECTION_PLANE_XZ;
    return 0;
  }
  if (strcmp(value, "yz") == 0) {
    *plane_out = XSECTION_PLANE_YZ;
    return 0;
  }

  fprintf(stderr, "Invalid plane: %s\n", value);
  return -1;
}

static int parse_dist(const char *value, uint32_t *dist_out) {
  errno = 0;
  char *end = NULL;
  unsigned long parsed = strtoul(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed > UINT32_MAX) {
    fprintf(stderr, "Invalid value for --dist: %s\n", value);
    return -1;
  }

  *dist_out = (uint32_t) parsed;
  return 0;
}

int parse_xsection_options(int argc, char **argv, XSectionOptions *options_out) {
  const char *config_path = NULL;
  const char *plane_text = NULL;
  const char *dist_text = NULL;
  const char *output_path = NULL;

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

    if (strcmp(arg, "--plane") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "Missing value for --plane.\n");
        print_usage(argv[0]);
        return -1;
      }
      plane_text = argv[++i];
      continue;
    }
    if (strncmp(arg, "--plane=", 8) == 0) {
      plane_text = arg + 8;
      continue;
    }

    if (strcmp(arg, "--dist") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "Missing value for --dist.\n");
        print_usage(argv[0]);
        return -1;
      }
      dist_text = argv[++i];
      continue;
    }
    if (strncmp(arg, "--dist=", 7) == 0) {
      dist_text = arg + 7;
      continue;
    }

    if (arg[0] == '-') {
      fprintf(stderr, "Unknown option: %s\n", arg);
      print_usage(argv[0]);
      return -1;
    }

    if (output_path == NULL) {
      output_path = arg;
      continue;
    }

    fprintf(stderr, "Unexpected positional argument: %s\n", arg);
    print_usage(argv[0]);
    return -1;
  }

  if (config_path == NULL || plane_text == NULL || dist_text == NULL || output_path == NULL) {
    fprintf(stderr, "--config, --plane, --dist, and <output.png> are required.\n");
    print_usage(argv[0]);
    return -1;
  }

  XSectionOptions options;
  memset(&options, 0, sizeof(options));

  if (load_slice_config_file(config_path, &options.slice_options) != 0) {
    return -1;
  }
  options.slice_options.config_path = config_path;

  if (parse_plane(plane_text, &options.plane) != 0 ||
      parse_dist(dist_text, &options.dist) != 0) {
    return -1;
  }

  if (options.slice_options.output_dir == NULL || options.slice_options.output_dir[0] == '\0') {
    fprintf(stderr, "output.directory must be set in the slice config.\n");
    return -1;
  }

  options.output_path = output_path;
  *options_out = options;
  return 0;
}
