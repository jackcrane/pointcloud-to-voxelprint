#ifndef TRANSLATE_LAS_H
#define TRANSLATE_LAS_H

#include "translate_common.h"

#include <stdint.h>

int read_las_header(const char *path, LasHeader *header_out);
int stream_las_points(
    const char *path,
    const LasHeader *header,
    LasPointVisitor visitor,
    void *ctx,
    TranslateProgressLogger *progress,
    uint64_t *processed_out);

#endif
