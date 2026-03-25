#include "crop_cli.h"
#include "crop_pipeline.h"

int main(int argc, char **argv) {
  CropOptions options;
  if (parse_crop_options(argc, argv, &options) != 0) {
    return 1;
  }

  return run_crop(&options) == 0 ? 0 : 1;
}
