#include "translate_cli.h"

#include "translate_support.h"

#include <stdio.h>
#include <string.h>

static void print_usage(const char *program_name) {
  fprintf(
      stderr,
      "Usage: %s [--log-interval N] <input.las> <output.ply>\n",
      program_name);
}

int parse_translate_options(int argc, char **argv, TranslateOptions *options_out) {
  TranslateOptions options;
  memset(&options, 0, sizeof(options));
  options.log_interval = TRANSLATE_DEFAULT_LOG_INTERVAL;

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];

    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      print_usage(argv[0]);
      return -1;
    }

    if (strcmp(arg, "--log-interval") == 0) {
      if (i + 1 >= argc || !translate_parse_uint64_str(argv[i + 1], &options.log_interval)) {
        fprintf(stderr, "Invalid value for --log-interval.\n");
        print_usage(argv[0]);
        return -1;
      }
      i++;
      continue;
    }

    if (strncmp(arg, "--log-interval=", strlen("--log-interval=")) == 0) {
      if (!translate_parse_uint64_str(
              arg + strlen("--log-interval="),
              &options.log_interval)) {
        fprintf(stderr, "Invalid value for --log-interval.\n");
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

  if (options.input_path == NULL || options.output_path == NULL) {
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
