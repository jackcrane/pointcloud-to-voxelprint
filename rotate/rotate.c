#include "rotate_cli.h"
#include "rotate_pipeline.h"

int main(int argc, char **argv) {
  RotateOptions options;
  if (parse_rotate_options(argc, argv, &options) != 0) {
    return 1;
  }

  return run_rotate(&options) == 0 ? 0 : 1;
}
