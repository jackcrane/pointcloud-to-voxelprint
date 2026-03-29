#ifndef SLICE_TOML_H
#define SLICE_TOML_H

#include "slice_common.h"

#ifdef __cplusplus
extern "C" {
#endif

int load_slice_config_file(const char *path, SliceOptions *options);

#ifdef __cplusplus
}
#endif

#endif
