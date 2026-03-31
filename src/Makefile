CC ?= cc
SONG ?= xbcx
AUDIO_BACKEND ?= DUMMY
OUTFILE ?= $(SONG)

CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lm

# Backend selection and library linking
ifeq ($(AUDIO_BACKEND),ALSA)
	CFLAGS += -DUSE_ALSA
	LDFLAGS += -lasound
else ifeq ($(AUDIO_BACKEND),SDL3)
	CFLAGS += -DUSE_SDL3 $(shell pkg-config --cflags sdl3 2>/dev/null)
	LDFLAGS += $(shell pkg-config --libs sdl3 2>/dev/null)
else ifeq ($(AUDIO_BACKEND),SDL2)
	CFLAGS += -DUSE_SDL2 $(shell pkg-config --cflags sdl2 2>/dev/null)
	LDFLAGS += $(shell pkg-config --libs sdl2 2>/dev/null)
else ifeq ($(AUDIO_BACKEND),SDL)
	CFLAGS += -DUSE_SDL $(shell sdl-config --cflags 2>/dev/null)
	LDFLAGS += $(shell sdl-config --libs 2>/dev/null)
else ifeq ($(AUDIO_BACKEND),WINMM)
	CFLAGS += -D_WIN32
	LDFLAGS += -lwinmm
endif

# The primary target uses the OUTFILE variable provided by the build scripts
all: $(OUTFILE)

ifeq ($(PLATFORM),nintendo)
$(OUTFILE): "$(SONG)"".c mmlplay.c
	$(CC) $(CFLAGS) -o $(basename $@).elf $^ $(LDFLAGS)
	@if [ "$(ARCH)" = "arm" ]; then \
		echo "Packaging CIA for 3DS..."; \
		makerom -f cia -o $@ -elf $(basename $@).elf; \
	else \
		echo "Packaging NSP for Switch..."; \
		makerom -f nsp -o $@ -elf $(basename $@).elf; \
	fi
else
$(OUTFILE): ../songs/$(SONG).c mmlplay.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
endif

# Tools
tools: mml2c.c
	$(CC) -O2 -o ../mml2c mml2c.c

# Target for building the standalone player utility
player: mmlplay.c
	$(CC) $(CFLAGS) -DSTANDALONE_PLAYER -o ../player mmlplay.c $(LDFLAGS)

clean:
	rm -f ../compiles/*.exe ../compiles/*.nro ../compiles/*.cia ../compiles/*.nsp ../compiles/*.elf ../mml2c
	rm -f ../compiles/linux_* ../compiles/windows_* ../compiles/nintendo_* ../compiles/player ../compiles/converted_song.c

.PHONY: all clean player
