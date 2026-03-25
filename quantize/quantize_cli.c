#include "quantize_cli.h"

#include "quantize_support.h"

#include <stdio.h>
#include <string.h>

static const char *const STAGE_NAMES[] = {
    "bounds",
    "shard",
    "reduce",
};

static void print_usage(const char *program_name) {
  fprintf(
      stderr,
      "Usage: %s [--log-interval N] [--temp-dir DIR] "
      "[--steps bounds,shard,reduce|shard,reduce|reduce] "
      "<input.ply> <output.ply>\n",
      program_name);
}

static int parse_steps_value(const char *value, QuantizeStartStage *start_stage_out) {
  char buffer[128];
  size_t value_len = strlen(value);
  if (value_len == 0 || value_len >= sizeof(buffer)) {
    return -1;
  }

  memcpy(buffer, value, value_len + 1);

  size_t start_index = SIZE_MAX;
  size_t token_index = 0;
  char *saveptr = NULL;
  for (char *token = strtok_r(buffer, ",", &saveptr);
       token != NULL;
       token = strtok_r(NULL, ",", &saveptr)) {
    if (token_index == 0) {
      for (size_t i = 0; i < (sizeof(STAGE_NAMES) / sizeof(STAGE_NAMES[0])); ++i) {
        if (strcmp(token, STAGE_NAMES[i]) == 0) {
          start_index = i;
          break;
        }
      }
      if (start_index == SIZE_MAX) {
        return -1;
      }
    }

    size_t expected_index = start_index + token_index;
    if (expected_index >= (sizeof(STAGE_NAMES) / sizeof(STAGE_NAMES[0]))) {
      return -1;
    }
    if (strcmp(token, STAGE_NAMES[expected_index]) != 0) {
      return -1;
    }

    token_index++;
  }

  if (start_index == SIZE_MAX || token_index == 0) {
    return -1;
  }

  *start_stage_out = (QuantizeStartStage)start_index;
  return 0;
}

int parse_quantize_options(int argc, char **argv, QuantizeOptions *options_out) {
  QuantizeOptions options;
  memset(&options, 0, sizeof(options));
  options.log_interval = DEFAULT_LOG_INTERVAL;
  options.start_stage = QUANTIZE_START_BOUNDS;

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

    if (strcmp(arg, "--temp-dir") == 0) {
      if (i + 1 >= argc || argv[i + 1][0] == '\0') {
        fprintf(stderr, "Invalid value for --temp-dir.\n");
        print_usage(argv[0]);
        return -1;
      }
      options.temp_dir_path = argv[i + 1];
      i++;
      continue;
    }

    if (starts_with(arg, "--temp-dir=")) {
      const char *value = arg + strlen("--temp-dir=");
      if (value[0] == '\0') {
        fprintf(stderr, "Invalid value for --temp-dir.\n");
        print_usage(argv[0]);
        return -1;
      }
      options.temp_dir_path = value;
      continue;
    }

    if (strcmp(arg, "--steps") == 0) {
      if (i + 1 >= argc || parse_steps_value(argv[i + 1], &options.start_stage) != 0) {
        fprintf(
            stderr,
            "Invalid value for --steps. Use bounds,shard,reduce, shard,reduce, or reduce.\n");
        print_usage(argv[0]);
        return -1;
      }
      i++;
      continue;
    }

    if (starts_with(arg, "--steps=")) {
      if (parse_steps_value(arg + strlen("--steps="), &options.start_stage) != 0) {
        fprintf(
            stderr,
            "Invalid value for --steps. Use bounds,shard,reduce, shard,reduce, or reduce.\n");
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
