#ifndef QUANTIZE_PLY_H
#define QUANTIZE_PLY_H

#include "quantize_common.h"

void free_ply_header(PlyHeader *header);
int read_ply_header(const char *file_path, PlyHeader *header_out);
int for_each_vertex(
    const char *file_path,
    const PlyHeader *header,
    VertexVisitor visitor,
    void *ctx,
    ProgressLogger *progress,
    uint64_t *processed_out);

#endif
