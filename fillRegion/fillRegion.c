#include "fillRegion_cli.h"
#include "fillRegion_pipeline.h"

int main(int argc, char **argv) {
  FillRegionOptions options;
  if (parse_fill_region_options(argc, argv, &options) != 0) {
    return 1;
  }

  const int rc = run_fill_region(&options) == 0 ? 0 : 1;
  free_fill_region_options(&options);
  return rc;
}
