#include "quantize_cli.h"

#include "quantize_support.h"

#include <stdio.h>
#include <string.h>

static const char *const STAGE_NAMES[] = {
    "bounds",
    "shard",
    "reduce",
    "write",
};

static void print_usage(const char *program_name) {
  fprintf(
      stderr,
      "Usage: %s [--log-interval N] [--steps bounds[,shard[,reduce[,write]]]] "
      "<input.ply> <output.ply>\n",
      program_name);
}

static int parse_steps_value(const char *value, unsigned *stage_mask_out) {
  char buffer[128];
  size_t value_len = strlen(value);
  if (value_len == 0 || value_len >= sizeof(buffer)) {
    return -1;
  }

  memcpy(buffer, value, value_len + 1);

  unsigned stage_mask = 0;
  size_t expected_stage_index = 0;
  char *saveptr = NULL;
  for (char *token = strtok_r(buffer, ",", &saveptr);
       token != NULL;
       token = strtok_r(NULL, ",", &saveptr)) {
    if (expected_stage_index >= (sizeof(STAGE_NAMES) / sizeof(STAGE_NAMES[0]))) {
      return -1;
    }
    if (strcmp(token, STAGE_NAMES[expected_stage_index]) != 0) {
      return -1;
    }

    stage_mask |= (unsigned)(1u << expected_stage_index);
    expected_stage_index++;
  }

  if (stage_mask == 0) {
    return -1;
  }

  *stage_mask_out = stage_mask;
  return 0;
}

int parse_quantize_options(int argc, char **argv, QuantizeOptions *options_out) {
  QuantizeOptions options;
  memset(&options, 0, sizeof(options));
  options.log_interval = DEFAULT_LOG_INTERVAL;
  options.stage_mask = QUANTIZE_STAGE_ALL;

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

    if (strcmp(arg, "--steps") == 0) {
      if (i + 1 >= argc || parse_steps_value(argv[i + 1], &options.stage_mask) != 0) {
        fprintf(
            stderr,
            "Invalid value for --steps. Use a prefix like bounds, bounds,shard, "
            "bounds,shard,reduce, or bounds,shard,reduce,write.\n");
        print_usage(argv[0]);
        return -1;
      }
      i++;
      continue;
    }

    if (starts_with(arg, "--steps=")) {
      if (parse_steps_value(arg + strlen("--steps="), &options.stage_mask) != 0) {
        fprintf(
            stderr,
            "Invalid value for --steps. Use a prefix like bounds, bounds,shard, "
            "bounds,shard,reduce, or bounds,shard,reduce,write.\n");
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
