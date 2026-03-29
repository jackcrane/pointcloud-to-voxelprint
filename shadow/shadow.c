#include "shadow_cli.h"
#include "shadow_pipeline.h"

int main(int argc, char **argv) {
  ShadowOptions options;
  if (parse_shadow_options(argc, argv, &options) != 0) {
    return 1;
  }

  const int rc = run_shadow(&options) == 0 ? 0 : 1;
  free_shadow_options(&options);
  return rc;
}
