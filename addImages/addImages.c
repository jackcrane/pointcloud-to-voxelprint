#include "addImages_cli.h"
#include "addImages_pipeline.h"

int main(int argc, char **argv) {
  AddImagesOptions options;
  if (parse_add_images_options(argc, argv, &options) != 0) {
    return 1;
  }

  const int rc = run_add_images(&options) == 0 ? 0 : 1;
  free_add_images_options(&options);
  return rc;
}
