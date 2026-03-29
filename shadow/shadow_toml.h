#ifndef SHADOW_TOML_H
#define SHADOW_TOML_H

#include "shadow_common.h"

#ifdef __cplusplus
extern "C" {
#endif

int load_shadow_config_file(const char *path, ShadowOptions *options);

#ifdef __cplusplus
}
#endif

#endif
