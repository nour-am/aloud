VERSION ?= 0.1.0

HOST_CC ?= clang
ARM_CC  := ./tools/arm-cc

CFLAGS_OUR := -std=c11 -O2 -Wall -Wextra -Werror
CFLAGS_TP  := -std=gnu11 -O2 -w

INCLUDES_COMMON := -Isrc -Ithird_party/sonic -Ithird_party/stb -Ithird_party/FBInk
HOST_FFMPEG_INC := -Ibuild/host/ffmpeg/include
HOST_FFMPEG_LIB := -Lbuild/host/ffmpeg/lib -lavformat -lavcodec -lswresample -lavutil

KINDLE_FFMPEG_INC := -Ibuild/kindle/ffmpeg/include
KINDLE_FFMPEG_LIB := -Lbuild/kindle/ffmpeg/lib -lavformat -lavcodec -lswresample -lavutil

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  HOST_LDFLAGS := -lm -lpthread -framework CoreFoundation
else
  HOST_LDFLAGS := -lm -lpthread
endif

CORE_SRCS := \
	src/store.c \
	src/decoder.c \
	src/mp4lite.c \
	src/canvas.c \
	src/library.c \
	src/audio_out_sim.c \
	src/audio_out_gst.c \
	src/audio_out_alsa.c \
	src/power.c \
	src/player.c \
	src/text.c \
	src/icons.c \
	src/ui.c \
	src/bench.c \
	src/platform_sim.c

FBINK_SRCS := \
	third_party/FBInk/fbink.c \
	third_party/FBInk/cutef8/utf8.c \
	third_party/FBInk/cutef8/dfa.c \
	third_party/FBInk/fbink_input_scan.c \
	third_party/FBInk/qimagescale/qimagescale.c

FBINK_FLAGS := \
	-D_GNU_SOURCE \
	-DFBINK_FOR_KINDLE \
	-DFBINK_MINIMAL \
	-DFBINK_WITH_DRAW \
	-DFBINK_WITH_INPUT \
	-DFBINK_WITH_IMAGE \
	-DFBINK_VERSION='"886f25f-aloud"'

.PHONY: all test sim showcase demo-video kindle package fontpacks clean FORCE

all: test sim kindle package

build/host/sonic.o: third_party/sonic/sonic.c
	@mkdir -p build/host
	$(HOST_CC) $(CFLAGS_TP) -Ithird_party/sonic -c $< -o $@

build/host/test_aloud: $(CORE_SRCS) tests/test_main.c build/host/sonic.o
	@mkdir -p build/host
	$(HOST_CC) $(CFLAGS_OUR) $(INCLUDES_COMMON) $(HOST_FFMPEG_INC) \
		$(CORE_SRCS) tests/test_main.c build/host/sonic.o \
		$(HOST_FFMPEG_LIB) $(HOST_LDFLAGS) -o $@

build/host/aloud-sim: $(CORE_SRCS) src/main.c src/demo.c src/demo.h build/host/sonic.o
	@mkdir -p build/host
	$(HOST_CC) $(CFLAGS_OUR) $(INCLUDES_COMMON) $(HOST_FFMPEG_INC) \
		$(CORE_SRCS) src/main.c src/demo.c build/host/sonic.o \
		$(HOST_FFMPEG_LIB) $(HOST_LDFLAGS) -o $@

test: build/host/test_aloud
	./tests/gen_fixtures.sh build/test-fixtures
	./build/host/test_aloud build/test-fixtures third_party/fonts
	sh tests/test_launcher.sh

sim: build/host/aloud-sim
	./tests/gen_fixtures.sh build/test-fixtures
	@mkdir -p tests/golden docs/screens
	./build/host/aloud-sim \
		--library build/test-fixtures/library \
		--state build/test-fixtures/state \
		--fonts third_party/fonts \
		--export-screens tests/golden
	cp tests/golden/pw5_library.png docs/screens/library.png
	cp tests/golden/pw5_folders.png docs/screens/folders.png
	cp tests/golden/pw5_player.png docs/screens/player.png
	cp tests/golden/pw5_chapters.png docs/screens/chapters.png
	cp tests/golden/pw5_sheet_speed.png docs/screens/sheet_speed.png
	cp tests/golden/pw5_sheet_sleep.png docs/screens/sheet_sleep.png
	cp tests/golden/pw5_landscape_player.png docs/screens/player_landscape.png
	cp tests/golden/kt4_player.png docs/screens/player_kt4.png

# README images (docs/media/): multilingual demo library rendered by the simulator.
showcase: build/host/aloud-sim
	./tools/make-showcase.sh

# docs/media/demo.mp4: scripted tour with real audio (downloads a LibriVox recording).
demo-video: showcase
	./tools/make-demo-video.sh

build/kindle/tp.o: third_party/sonic/sonic.c $(FBINK_SRCS)
	@mkdir -p build/kindle/fbink_objs
	$(ARM_CC) $(CFLAGS_TP) -Ithird_party/sonic -c third_party/sonic/sonic.c -o build/kindle/fbink_objs/sonic.o
	$(ARM_CC) $(CFLAGS_TP) $(FBINK_FLAGS) -Ithird_party/FBInk -Ithird_party/stb -c third_party/FBInk/fbink.c -o build/kindle/fbink_objs/fbink.o
	$(ARM_CC) $(CFLAGS_TP) $(FBINK_FLAGS) -Ithird_party/FBInk -c third_party/FBInk/cutef8/utf8.c -o build/kindle/fbink_objs/utf8.o
	$(ARM_CC) $(CFLAGS_TP) $(FBINK_FLAGS) -Ithird_party/FBInk -c third_party/FBInk/cutef8/dfa.c -o build/kindle/fbink_objs/dfa.o
	$(ARM_CC) $(CFLAGS_TP) $(FBINK_FLAGS) -Ithird_party/FBInk -c third_party/FBInk/fbink_input_scan.c -o build/kindle/fbink_objs/fbink_input_scan.o
	$(ARM_CC) $(CFLAGS_TP) $(FBINK_FLAGS) -Ithird_party/FBInk -c third_party/FBInk/qimagescale/qimagescale.c -o build/kindle/fbink_objs/qimagescale.o
	./tools/arm-ar rcs build/kindle/libtp.a build/kindle/fbink_objs/*.o
	@touch $@

build/kindle/aloud: $(CORE_SRCS) src/platform_kindle.c src/main.c build/kindle/tp.o
	@mkdir -p build/kindle
	$(ARM_CC) $(CFLAGS_OUR) -DALOUD_KINDLE_BUILD \
		-ffunction-sections -fdata-sections -static -s \
		$(INCLUDES_COMMON) $(KINDLE_FFMPEG_INC) \
		$(CORE_SRCS) src/platform_kindle.c src/main.c \
		build/kindle/libtp.a $(KINDLE_FFMPEG_LIB) \
		-lm -lpthread -Wl,--gc-sections -o $@

build/kindle/aloud-sink-armel: helper/aloud_sink.c
	@mkdir -p build/kindle
	zig cc -target arm-linux-gnueabi.2.12 -mcpu=cortex_a8 \
		-std=gnu11 -O2 -Wall -Wextra -Werror -s \
		helper/aloud_sink.c -ldl -o $@

build/kindle/aloud-sink-armhf: helper/aloud_sink.c
	@mkdir -p build/kindle
	zig cc -target arm-linux-gnueabihf.2.20 -mcpu=cortex_a8 \
		-std=gnu11 -O2 -Wall -Wextra -Werror -s \
		helper/aloud_sink.c -ldl -o $@

kindle: build/kindle/aloud build/kindle/aloud-sink-armel build/kindle/aloud-sink-armhf
	@file build/kindle/aloud build/kindle/aloud-sink-armel build/kindle/aloud-sink-armhf
	@ls -lh build/kindle/aloud build/kindle/aloud-sink-armel build/kindle/aloud-sink-armhf

package: kindle
	@rm -rf build/pkg dist/aloud-$(VERSION).zip
	@mkdir -p build/pkg/extensions/aloud/bin \
		build/pkg/extensions/aloud/fonts \
		build/pkg/audiobooks \
		build/pkg/documents \
		dist
	cp package/scriptlet.sh build/pkg/documents/Aloud.sh
	chmod +x build/pkg/documents/Aloud.sh
	cp package/config.xml build/pkg/extensions/aloud/config.xml
	cp package/menu.json build/pkg/extensions/aloud/menu.json
	cp package/launcher.sh build/pkg/extensions/aloud/aloud.sh
	chmod +x build/pkg/extensions/aloud/aloud.sh
	cp build/kindle/aloud build/pkg/extensions/aloud/bin/aloud
	cp build/kindle/aloud-sink-armel build/pkg/extensions/aloud/bin/aloud-sink-armel
	cp build/kindle/aloud-sink-armhf build/pkg/extensions/aloud/bin/aloud-sink-armhf
	cp tools/kindle-diagnose.sh build/pkg/extensions/aloud/bin/kindle-diagnose.sh
	cp tools/kindle-audio-test.sh build/pkg/extensions/aloud/bin/kindle-audio-test.sh
	cp tools/kindle-perf.sh build/pkg/extensions/aloud/bin/kindle-perf.sh
	mkdir -p build/pkg/extensions/aloud/koreader
	cp -R extras/koreader/aloud.koplugin build/pkg/extensions/aloud/koreader/
	chmod +x build/pkg/extensions/aloud/bin/*
	cp third_party/fonts/Literata-Regular.ttf \
	   third_party/fonts/Literata-SemiBold.ttf \
	   third_party/fonts/Literata-OFL.txt \
	   third_party/fonts/Inter-Regular.ttf \
	   third_party/fonts/Inter-SemiBold.ttf \
	   third_party/fonts/Inter-OFL.txt \
	   build/pkg/extensions/aloud/fonts/
	cp package/audiobooks-README.txt build/pkg/audiobooks/README.txt
	cp LICENSE build/pkg/extensions/aloud/LICENSE.txt
	cd build/pkg && zip -q -r -X ../../dist/aloud-$(VERSION).zip extensions audiobooks documents
	@ls -lh dist/aloud-$(VERSION).zip
	@$(MAKE) --no-print-directory fontpacks

# Optional language font packs, unzipped onto the Kindle like the main zip. Without
# them, characters the base fonts lack draw as "?" (src/text.c loads them if present).
FONTPACK_arabic := NotoNaskhArabic-Regular.ttf NotoNaskhArabic-Bold.ttf \
	NotoSansArabicUI-Regular.ttf NotoSansArabicUI-Bold.ttf Noto-OFL.txt
FONTPACK_chinese-japanese := NotoSansCJK-Regular.otf NotoSansCJK-Bold.otf NotoSansCJK-OFL.txt
FONTPACKS := arabic chinese-japanese

fontpacks: $(foreach p,$(FONTPACKS),dist/aloud-fonts-$(p)-$(VERSION).zip)

dist/aloud-fonts-%-$(VERSION).zip: FORCE
	@mkdir -p build/fontpacks/$*/extensions/aloud/fonts dist
	cp $(addprefix third_party/fonts/,$(FONTPACK_$*)) build/fontpacks/$*/extensions/aloud/fonts/
	@rm -f $@
	cd build/fontpacks/$* && zip -q -r -X ../../../$@ extensions
	@ls -lh $@

clean:
	rm -rf build/host/test_aloud build/host/aloud-sim build/host/sonic.o \
	       build/kindle/aloud build/kindle/aloud-sink-* build/kindle/fbink_objs \
	       build/kindle/libtp.a build/kindle/tp.o build/pkg build/test-fixtures

FORCE:
