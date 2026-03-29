CC ?= cc
CFLAGS ?= -O3 -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L
CXX ?= c++
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra -D_POSIX_C_SOURCE=200809L
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
XSECTION_BIN = bin/xsection
FILL_REGION_DIR = fillRegion
FILL_REGION_BIN = bin/fillRegion
SHADOW_DIR = shadow
SHADOW_BIN = bin/shadow

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

SLICE_C_SRCS = \
	$(SLICE_DIR)/slice.c \
	$(SLICE_DIR)/slice_cli_toml.c \
	$(SLICE_DIR)/slice_pipeline.c

SLICE_CPP_SRCS = \
	$(SLICE_DIR)/slice_toml.cpp

SLICE_OBJS = \
	$(SLICE_DIR)/slice.o \
	$(SLICE_DIR)/slice_cli_toml.o \
	$(SLICE_DIR)/slice_pipeline.o \
	$(SLICE_DIR)/slice_toml.o

XSECTION_OBJS = \
	$(SLICE_DIR)/xsection.o \
	$(SLICE_DIR)/xsection_cli.o \
	$(SLICE_DIR)/xsection_pipeline.o \
	$(SLICE_DIR)/slice_toml.o

FILL_REGION_OBJS = \
	$(FILL_REGION_DIR)/fillRegion.o \
	$(FILL_REGION_DIR)/fillRegion_cli.o \
	$(FILL_REGION_DIR)/fillRegion_pipeline.o \
	$(FILL_REGION_DIR)/fillRegion_toml.o

SLICE_HDRS = \
	$(SLICE_DIR)/slice_cli.h \
	$(SLICE_DIR)/slice_common.h \
	$(SLICE_DIR)/slice_pipeline.h \
	$(SLICE_DIR)/slice_toml.h

XSECTION_HDRS = \
	$(SLICE_DIR)/xsection_cli.h \
	$(SLICE_DIR)/xsection_common.h \
	$(SLICE_DIR)/xsection_pipeline.h \
	$(SLICE_DIR)/slice_common.h \
	$(SLICE_DIR)/slice_toml.h

FILL_REGION_HDRS = \
	$(FILL_REGION_DIR)/fillRegion_cli.h \
	$(FILL_REGION_DIR)/fillRegion_common.h \
	$(FILL_REGION_DIR)/fillRegion_pipeline.h \
	$(FILL_REGION_DIR)/fillRegion_toml.h

SHADOW_SRCS = \
	$(SHADOW_DIR)/shadow.c \
	$(SHADOW_DIR)/shadow_cli.c \
	$(SHADOW_DIR)/shadow_pipeline.c

SHADOW_HDRS = \
	$(SHADOW_DIR)/shadow_cli.h \
	$(SHADOW_DIR)/shadow_common.h \
	$(SHADOW_DIR)/shadow_pipeline.h \
	$(SHADOW_DIR)/shadow_toml.h

SHADOW_OBJS = \
	$(SHADOW_DIR)/shadow.o \
	$(SHADOW_DIR)/shadow_cli.o \
	$(SHADOW_DIR)/shadow_pipeline.o \
	$(SHADOW_DIR)/shadow_toml.o

.PHONY: quantize translate rotate crop slice xsection fillRegion shadow clean

quantize: $(QUANTIZE_BIN)
translate: $(TRANSLATE_BIN)
rotate: $(ROTATE_BIN)
crop: $(CROP_BIN)
slice: $(SLICE_BIN)
xsection: $(XSECTION_BIN)
fillRegion: $(FILL_REGION_BIN)
shadow: $(SHADOW_BIN)

$(QUANTIZE_BIN): $(QUANTIZE_SRCS) $(QUANTIZE_HDRS) | bin
	$(CC) $(CFLAGS) -o $@ $(QUANTIZE_SRCS) $(LDFLAGS) $(LDLIBS)

$(TRANSLATE_BIN): $(TRANSLATE_SRCS) $(TRANSLATE_HDRS) | bin
	$(CC) $(CFLAGS) -o $@ $(TRANSLATE_SRCS) $(LDFLAGS) $(LDLIBS)

$(ROTATE_BIN): $(ROTATE_SRCS) $(ROTATE_HDRS) | bin
	$(CC) $(CFLAGS) -o $@ $(ROTATE_SRCS) $(LDFLAGS) $(LDLIBS)

$(CROP_BIN): $(CROP_SRCS) $(CROP_HDRS) | bin
	$(CC) $(CFLAGS) -o $@ $(CROP_SRCS) $(LDFLAGS) $(LDLIBS)

$(SLICE_BIN): $(SLICE_OBJS) $(SLICE_HDRS) | bin
	$(CXX) $(CXXFLAGS) -o $@ $(SLICE_OBJS) $(LDFLAGS) $(LDLIBS)

$(XSECTION_BIN): $(XSECTION_OBJS) $(XSECTION_HDRS) | bin
	$(CXX) $(CXXFLAGS) -o $@ $(XSECTION_OBJS) $(LDFLAGS) $(LDLIBS)

$(FILL_REGION_BIN): $(FILL_REGION_OBJS) $(FILL_REGION_HDRS) | bin
	$(CXX) $(CXXFLAGS) -o $@ $(FILL_REGION_OBJS) $(LDFLAGS) $(LDLIBS)

$(SHADOW_BIN): $(SHADOW_OBJS) $(SHADOW_HDRS) | bin
	$(CXX) $(CXXFLAGS) -o $@ $(SHADOW_OBJS) $(LDFLAGS) $(LDLIBS)

$(SLICE_DIR)/slice.o: $(SLICE_DIR)/slice.c $(SLICE_HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

$(SLICE_DIR)/slice_cli_toml.o: $(SLICE_DIR)/slice_cli_toml.c $(SLICE_HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

$(SLICE_DIR)/slice_pipeline.o: $(SLICE_DIR)/slice_pipeline.c $(SLICE_HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

$(SLICE_DIR)/slice_toml.o: $(SLICE_DIR)/slice_toml.cpp $(SLICE_HDRS)
	$(CXX) $(CXXFLAGS) -include cstdlib -c -o $@ $<

$(SLICE_DIR)/xsection.o: $(SLICE_DIR)/xsection.c $(XSECTION_HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

$(SLICE_DIR)/xsection_cli.o: $(SLICE_DIR)/xsection_cli.c $(XSECTION_HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

$(SLICE_DIR)/xsection_pipeline.o: $(SLICE_DIR)/xsection_pipeline.c $(XSECTION_HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

$(FILL_REGION_DIR)/fillRegion.o: $(FILL_REGION_DIR)/fillRegion.c $(FILL_REGION_HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

$(FILL_REGION_DIR)/fillRegion_cli.o: $(FILL_REGION_DIR)/fillRegion_cli.c $(FILL_REGION_HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

$(FILL_REGION_DIR)/fillRegion_pipeline.o: $(FILL_REGION_DIR)/fillRegion_pipeline.c $(FILL_REGION_HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

$(FILL_REGION_DIR)/fillRegion_toml.o: $(FILL_REGION_DIR)/fillRegion_toml.cpp $(FILL_REGION_HDRS)
	$(CXX) $(CXXFLAGS) -include cstdlib -c -o $@ $<

$(SHADOW_DIR)/shadow.o: $(SHADOW_DIR)/shadow.c $(SHADOW_HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

$(SHADOW_DIR)/shadow_cli.o: $(SHADOW_DIR)/shadow_cli.c $(SHADOW_HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

$(SHADOW_DIR)/shadow_pipeline.o: $(SHADOW_DIR)/shadow_pipeline.c $(SHADOW_HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

$(SHADOW_DIR)/shadow_toml.o: $(SHADOW_DIR)/shadow_toml.cpp $(SHADOW_HDRS)
	$(CXX) $(CXXFLAGS) -include cstdlib -c -o $@ $<

bin:
	mkdir -p $@

clean:
	rm -f $(QUANTIZE_BIN) $(TRANSLATE_BIN) $(ROTATE_BIN) $(CROP_BIN) $(SLICE_BIN) $(XSECTION_BIN) $(FILL_REGION_BIN) $(SHADOW_BIN) *.o \
		$(QUANTIZE_DIR)/*.o $(TRANSLATE_DIR)/*.o $(ROTATE_DIR)/*.o $(CROP_DIR)/*.o \
		$(ASCII_PLY_DIR)/*.o $(SLICE_DIR)/*.o $(FILL_REGION_DIR)/*.o $(SHADOW_DIR)/*.o
