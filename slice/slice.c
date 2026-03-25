#include "slice_cli.h"
#include "slice_pipeline.h"

int main(int argc, char **argv) {
  SliceOptions options;
  if (parse_slice_options(argc, argv, &options) != 0) {
    return 1;
  }

  return run_slice(&options) == 0 ? 0 : 1;
}
