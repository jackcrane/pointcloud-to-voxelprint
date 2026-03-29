#include "xsection_cli.h"
#include "xsection_pipeline.h"

int main(int argc, char **argv) {
  XSectionOptions options;
  if (parse_xsection_options(argc, argv, &options) != 0) {
    return 1;
  }

  return run_xsection(&options) == 0 ? 0 : 1;
}
