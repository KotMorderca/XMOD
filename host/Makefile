# SPDX-License-Identifier: GPL-3.0-only
# PC build of the engine and its tools (WSL / Linux: make -C host)
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=c99
OUT = ../build/host

ENGINE = ../engine/player.c ../engine/song.c ../engine/ft2_tables.c
HDRS = ../engine/song.h ../engine/player.h ../engine/fmt.h ../engine/ft2_tables.h modconv.h
FONTS = ../third_party/fonts/spleen-6x12.bdf ../third_party/fonts/spleen-5x8.bdf

all: $(OUT)/render $(OUT)/uisim $(OUT)/songtest $(OUT)/convert

$(OUT)/render: render.c modconv.c $(ENGINE) $(HDRS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ render.c modconv.c $(ENGINE)

$(OUT)/gen/font.h: ../tools/gen_font.py $(FONTS)
	@mkdir -p $(OUT)/gen
	python3 ../tools/gen_font.py $@ font6x12=../third_party/fonts/spleen-6x12.bdf font5x8=../third_party/fonts/spleen-5x8.bdf

# the tracker's pages with scripted input and PNG screenshots
$(OUT)/uisim: uisim.c modconv.c ../firmware/pages.c ../firmware/edit.c ../firmware/pages_ins.c ../firmware/pages_glo.c ../firmware/store.c ../firmware/gfx.c $(ENGINE) $(HDRS) $(OUT)/gen/font.h
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -I../engine -I$(OUT)/gen -o $@ uisim.c modconv.c $(ENGINE)

$(OUT)/convert: convert.c modconv.c modwrite.c modwrite.h modconv.h ../engine/ft2_tables.c
	$(CC) $(CFLAGS) -o $@ convert.c modconv.c modwrite.c ../engine/ft2_tables.c

# engine/song.c with a small arena against a plain copy: make -C host test
$(OUT)/storetest: storetest.c modconv.c ../firmware/store.c ../engine/song.c ../engine/song.h
	$(CC) $(CFLAGS) -I../engine -o $@ storetest.c modconv.c ../engine/song.c ../engine/ft2_tables.c

$(OUT)/songtest: songtest.c modconv.c ../engine/song.c ../engine/song.h ../engine/fmt.h
	$(CC) $(CFLAGS) -DSONG_ARENA=49152u -I../engine -o $@ songtest.c modconv.c ../engine/song.c ../engine/ft2_tables.c

SONGS = $(wildcard ../songs/*.xm ../songs/*.mod)

test: $(OUT)/songtest $(OUT)/storetest
	$(OUT)/storetest $(SONGS)
	for f in $(SONGS); do $(OUT)/songtest "$$f" 20000 || exit 1; done

# the release build's tests (GitHub): the above, and every song saved and loaded again plays the same
test-ci: all test
	for f in $(SONGS); do $(OUT)/render "$$f" $(OUT)/a.wav --seconds 20 2>/dev/null && 	  $(OUT)/render "$$f" $(OUT)/b.wav --seconds 20 --resave 2>/dev/null && cmp $(OUT)/a.wav $(OUT)/b.wav || exit 1; done
	@echo test-ci: ok

# XMOD's update loader (tools/build_fw.py patches Felucca's into build/fw/loader/src: build the firmware
# first): make -C host ldrtest OLD=an.fwsc-with-data-in-the-tail (XMOD 1.0.0's)
$(OUT)/ldrtest: ldrtest.c ../build/fw/loader/src/ldr_core.c ../firmware/layout.h
	$(CC) -O1 -g -Wall -Wno-unused-function -o $@ ldrtest.c

ldrtest: $(OUT)/ldrtest
	$(OUT)/ldrtest $(OLD) ../build/fw/xmod.fwsc

clean:
	rm -rf $(OUT)

.PHONY: all clean test test-ci ldrtest
