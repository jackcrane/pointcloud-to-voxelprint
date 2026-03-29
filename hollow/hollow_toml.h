#ifndef HOLLOW_TOML_H
#define HOLLOW_TOML_H

#include "hollow_common.h"

#ifdef __cplusplus
extern "C" {
#endif

int load_hollow_config_file(const char *path, HollowOptions *options);

#ifdef __cplusplus
}
#endif

#endif
