CC ?= cc
CFLAGS ?= -O3 -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=
LDLIBS ?= -lm

QUANTIZE_DIR = quantize
QUANTIZE_BIN = bin/quantize

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

.PHONY: quantize clean

quantize: $(QUANTIZE_BIN)

$(QUANTIZE_BIN): $(QUANTIZE_SRCS) $(QUANTIZE_HDRS) | bin
	$(CC) $(CFLAGS) -o $@ $(QUANTIZE_SRCS) $(LDFLAGS) $(LDLIBS)

bin:
	mkdir -p $@

clean:
	rm -f $(QUANTIZE_BIN) *.o $(QUANTIZE_DIR)/*.o
