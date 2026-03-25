CC ?= cc
CFLAGS ?= -O3 -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=
LDLIBS ?= -lm

QUANTIZE_DIR = quantize
QUANTIZE_BIN = bin/quantize
TRANSLATE_DIR = translate
TRANSLATE_BIN = bin/translate

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

.PHONY: quantize translate clean

quantize: $(QUANTIZE_BIN)
translate: $(TRANSLATE_BIN)

$(QUANTIZE_BIN): $(QUANTIZE_SRCS) $(QUANTIZE_HDRS) | bin
	$(CC) $(CFLAGS) -o $@ $(QUANTIZE_SRCS) $(LDFLAGS) $(LDLIBS)

$(TRANSLATE_BIN): $(TRANSLATE_SRCS) $(TRANSLATE_HDRS) | bin
	$(CC) $(CFLAGS) -o $@ $(TRANSLATE_SRCS) $(LDFLAGS) $(LDLIBS)

bin:
	mkdir -p $@

clean:
	rm -f $(QUANTIZE_BIN) $(TRANSLATE_BIN) *.o $(QUANTIZE_DIR)/*.o $(TRANSLATE_DIR)/*.o
