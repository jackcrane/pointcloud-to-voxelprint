#include "quantize_cli.h"
#include "quantize_pipeline.h"

int main(int argc, char **argv) {
  QuantizeOptions options;
  if (parse_quantize_options(argc, argv, &options) != 0) {
    return 1;
  }

  return run_quantize(&options) == 0 ? 0 : 1;
}
