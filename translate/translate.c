#include "translate_cli.h"
#include "translate_pipeline.h"

int main(int argc, char **argv) {
  TranslateOptions options;
  if (parse_translate_options(argc, argv, &options) != 0) {
    return 1;
  }

  return run_translate(&options) == 0 ? 0 : 1;
}
