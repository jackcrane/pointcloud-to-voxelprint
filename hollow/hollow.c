#include "hollow_cli.h"
#include "hollow_pipeline.h"

int main(int argc, char **argv) {
  HollowOptions options;
  if (parse_hollow_options(argc, argv, &options) != 0) {
    return 1;
  }

  const int rc = run_hollow(&options) == 0 ? 0 : 1;
  free_hollow_options(&options);
  return rc;
}
