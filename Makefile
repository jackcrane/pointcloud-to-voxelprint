CC ?= cc
CFLAGS ?= -O3 -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=
LDLIBS ?= -lm

QUANTIZE_SRCS = \
	quantize.c \
	quantize_cli.c \
	quantize_pipeline.c \
	quantize_ply.c \
	quantize_support.c

QUANTIZE_HDRS = \
	quantize_cli.h \
	quantize_common.h \
	quantize_pipeline.h \
	quantize_ply.h \
	quantize_support.h

quantize: $(QUANTIZE_SRCS) $(QUANTIZE_HDRS)
	$(CC) $(CFLAGS) -o $@ $(QUANTIZE_SRCS) $(LDFLAGS) $(LDLIBS)

.PHONY: clean
clean:
	rm -f quantize *.o
