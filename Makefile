CC ?= cc
CFLAGS ?= -O3 -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=
LDLIBS ?= -lm

QUANTIZE_OBJS = \
	quantize.o \
	quantize_cli.o \
	quantize_pipeline.o \
	quantize_ply.o \
	quantize_support.o

quantize: $(QUANTIZE_OBJS)
	$(CC) $(CFLAGS) -o $@ $(QUANTIZE_OBJS) $(LDFLAGS) $(LDLIBS)

.PHONY: clean
clean:
	rm -f quantize $(QUANTIZE_OBJS)
