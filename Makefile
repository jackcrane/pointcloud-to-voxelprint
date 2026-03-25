CC ?= cc
CFLAGS ?= -O3 -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=
LDLIBS ?= -lm

QUANTIZE_DIR = quantize
QUANTIZE_BIN = bin/quantize
TRANSLATE_DIR = translate
TRANSLATE_BIN = bin/translate
ROTATE_DIR = rotate
ROTATE_BIN = bin/rotate
CROP_DIR = crop
CROP_BIN = bin/crop
ASCII_PLY_DIR = ascii_ply
SLICE_DIR = slice
SLICE_BIN = bin/slice

QUANTIZE_SRCS = \
	$(QUANTIZE_DIR)/quantize.c \
	$(QUANTIZE_DIR)/quantize_cli.c \
	$(QUANTIZE_DIR)/quantize_pipeline.c \
	$(QUANTIZE_DIR)/quantize_ply.c \
	$(QUANTIZE_DIR)/quantize_support.c

QUANTIZE_HDRS = \
	$(QUANTIZE_DIR)/quantize_cli.h \
	$(QUANTIZE_DIR)/quantize_common.h \
	$(QUANTIZE_DIR)/quantize_pipeline.h \
	$(QUANTIZE_DIR)/quantize_ply.h \
	$(QUANTIZE_DIR)/quantize_support.h

TRANSLATE_SRCS = \
	$(TRANSLATE_DIR)/translate.c \
	$(TRANSLATE_DIR)/translate_cli.c \
	$(TRANSLATE_DIR)/translate_las.c \
	$(TRANSLATE_DIR)/translate_pipeline.c \
	$(TRANSLATE_DIR)/translate_ply.c \
	$(TRANSLATE_DIR)/translate_support.c

TRANSLATE_HDRS = \
	$(TRANSLATE_DIR)/translate_cli.h \
	$(TRANSLATE_DIR)/translate_common.h \
	$(TRANSLATE_DIR)/translate_las.h \
	$(TRANSLATE_DIR)/translate_pipeline.h \
	$(TRANSLATE_DIR)/translate_ply.h \
	$(TRANSLATE_DIR)/translate_support.h

ASCII_PLY_SRCS = \
	$(ASCII_PLY_DIR)/ascii_ply.c \
	$(ASCII_PLY_DIR)/ascii_ply_support.c

ASCII_PLY_HDRS = \
	$(ASCII_PLY_DIR)/ascii_ply.h \
	$(ASCII_PLY_DIR)/ascii_ply_support.h

ROTATE_SRCS = \
	$(ROTATE_DIR)/rotate.c \
	$(ROTATE_DIR)/rotate_cli.c \
	$(ROTATE_DIR)/rotate_pipeline.c \
	$(ASCII_PLY_SRCS)

ROTATE_HDRS = \
	$(ROTATE_DIR)/rotate_cli.h \
	$(ROTATE_DIR)/rotate_common.h \
	$(ROTATE_DIR)/rotate_pipeline.h \
	$(ASCII_PLY_HDRS)

CROP_SRCS = \
	$(CROP_DIR)/crop.c \
	$(CROP_DIR)/crop_cli.c \
	$(CROP_DIR)/crop_pipeline.c \
	$(ASCII_PLY_SRCS)

CROP_HDRS = \
	$(CROP_DIR)/crop_cli.h \
	$(CROP_DIR)/crop_common.h \
	$(CROP_DIR)/crop_pipeline.h \
	$(ASCII_PLY_HDRS)

SLICE_SRCS = \
	$(SLICE_DIR)/slice.c \
	$(SLICE_DIR)/slice_cli.c \
	$(SLICE_DIR)/slice_pipeline.c

SLICE_HDRS = \
	$(SLICE_DIR)/slice_cli.h \
	$(SLICE_DIR)/slice_common.h \
	$(SLICE_DIR)/slice_pipeline.h

.PHONY: quantize translate rotate crop slice clean

quantize: $(QUANTIZE_BIN)
translate: $(TRANSLATE_BIN)
rotate: $(ROTATE_BIN)
crop: $(CROP_BIN)
slice: $(SLICE_BIN)

$(QUANTIZE_BIN): $(QUANTIZE_SRCS) $(QUANTIZE_HDRS) | bin
	$(CC) $(CFLAGS) -o $@ $(QUANTIZE_SRCS) $(LDFLAGS) $(LDLIBS)

$(TRANSLATE_BIN): $(TRANSLATE_SRCS) $(TRANSLATE_HDRS) | bin
	$(CC) $(CFLAGS) -o $@ $(TRANSLATE_SRCS) $(LDFLAGS) $(LDLIBS)

$(ROTATE_BIN): $(ROTATE_SRCS) $(ROTATE_HDRS) | bin
	$(CC) $(CFLAGS) -o $@ $(ROTATE_SRCS) $(LDFLAGS) $(LDLIBS)

$(CROP_BIN): $(CROP_SRCS) $(CROP_HDRS) | bin
	$(CC) $(CFLAGS) -o $@ $(CROP_SRCS) $(LDFLAGS) $(LDLIBS)

$(SLICE_BIN): $(SLICE_SRCS) $(SLICE_HDRS) | bin
	$(CC) $(CFLAGS) -o $@ $(SLICE_SRCS) $(LDFLAGS) $(LDLIBS)

bin:
	mkdir -p $@

clean:
	rm -f $(QUANTIZE_BIN) $(TRANSLATE_BIN) $(ROTATE_BIN) $(CROP_BIN) $(SLICE_BIN) *.o \
		$(QUANTIZE_DIR)/*.o $(TRANSLATE_DIR)/*.o $(ROTATE_DIR)/*.o $(CROP_DIR)/*.o \
		$(ASCII_PLY_DIR)/*.o $(SLICE_DIR)/*.o
