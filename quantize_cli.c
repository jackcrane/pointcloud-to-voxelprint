#include "quantize_cli.h"

#include "quantize_support.h"

#include <stdio.h>
#include <string.h>

static void print_usage(const char *program_name) {
  fprintf(stderr, "Usage: %s [--log-interval N] <input.ply> <output.ply>\n", program_name);
}

int parse_quantize_options(int argc, char **argv, QuantizeOptions *options_out) {
  QuantizeOptions options;
  memset(&options, 0, sizeof(options));
  options.log_interval = DEFAULT_LOG_INTERVAL;

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (strcmp(arg, "--log-interval") == 0) {
      if (i + 1 >= argc || !parse_uint64_str(argv[i + 1], &options.log_interval)) {
        fprintf(stderr, "Invalid value for --log-interval.\n");
        print_usage(argv[0]);
        return -1;
      }
      i++;
      continue;
    }

    if (starts_with(arg, "--log-interval=")) {
      if (!parse_uint64_str(arg + strlen("--log-interval="), &options.log_interval)) {
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
