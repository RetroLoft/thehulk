CC         = m68k-atari-mint-gcc

# -m68000          : strict 68000 instruction set (no 68020/030/FPU)
# -O1              : safe optimisation level, avoids 68020 idioms from -O2
# -fno-strict-aliasing : safe ob_spec union access via .index / .tedinfo
# -D__GEMLIB_OLDNAMES  : expose NONE, EXIT, SELECTED, etc. from mt_gem.h
CFLAGS     = -Wall -Wextra -O1 -m68000 -fno-strict-aliasing \
             -I include -D__GEMLIB_OLDNAMES

# -s : strip symbols from the final executable
LDFLAGS    = -s -lgem

TARGET      = THEHULK.PRG
TARGET_DEMO = DEMOTNFS.PRG
SRCS        = src/main.c src/drive.c src/netconfig.c src/rtcconfig.c src/dialog.c src/sidetnfs_probe.c
# Where 'make install' copies the program, e.g. the folder Hatari mounts as a drive:
#   make install INSTALLDIR=~/hatari/C/CONFIG
INSTALLDIR ?=

# Files to copy on 'make install' (extend as the project grows).
# README.TXT ships on the SETTINGS disk alongside THEHULK.PRG and MUST
# be included as a release asset whenever a release is published -- see
# README.md.
DISTFILES  = $(TARGET) README.TXT

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS)

# Demo build: DEMO_MODE guards every cartridge-communication call site in
# dialog.c so DEMOTNFS.PRG never touches ROM3 at all -- for recording a
# demo video in Hatari with no SideTNFS cartridge/Pico present. Unlike
# 'all', this never installs: DEMOTNFS.PRG stays local to the project dir.
demo: $(SRCS)
	$(CC) $(CFLAGS) -DDEMO_MODE -o $(TARGET_DEMO) $(SRCS) $(LDFLAGS)

install: $(DISTFILES)
	@test -n "$(INSTALLDIR)" || { echo "set INSTALLDIR, e.g. make install INSTALLDIR=~/hatari/C"; exit 1; }
	cp -v $(DISTFILES) $(INSTALLDIR)/

clean:
	rm -f $(TARGET) $(TARGET_DEMO)

.PHONY: all install clean demo
