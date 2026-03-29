#ifndef FILL_REGION_TOML_H
#define FILL_REGION_TOML_H

#include "fillRegion_common.h"

#ifdef __cplusplus
extern "C" {
#endif

int load_fill_region_config_file(const char *path, FillRegionOptions *options);

#ifdef __cplusplus
}
#endif

#endif
