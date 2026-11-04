# Host (macOS/Linux) build of the portable core, for unit tests and previews.

CXX      ?= c++
CXXFLAGS := -std=c++14 -O1 -g -Wall -Wextra -DPIMAC_HOST -I../src

CORE_SRC := $(wildcard ../src/core/*.cpp)
CORE_HDR := $(wildcard ../src/core/*.h)
MAC_SRC  := $(wildcard ../src/mac/*.cpp)
MAC_HDR  := $(wildcard ../src/mac/*.h)

# Musashi 68000 core (fetched by scripts/fetch-deps.sh)
MUSASHI  := ../deps/musashi
MUSASHI_SRC := $(wildcard $(MUSASHI)/m68kcpu.c $(MUSASHI)/m68kops.c $(MUSASHI)/softfloat/softfloat.c)
MUSASHI_OBJ := $(patsubst $(MUSASHI)/%.c,obj/musashi/%.o,$(MUSASHI_SRC))
CC       ?= cc
# Level-triggered interrupts: Musashi must not drop the IRQ once it takes it
# (the Mac's VIA holds /IRQ until the handler clears the flag).
MUSASHI_DEFS := -DM68K_EMULATE_INT_ACK=2 '-DM68K_INT_ACK_CALLBACK(A)=0xffffffff' -DM68K_EMULATE_PMMU=0
CFLAGS   := -O2 -g -w -I$(MUSASHI) $(MUSASHI_DEFS)

.PHONY: all run preview clean mac

all: run

# The serial devices (SCC, LocalTalk, modem), the sound chip and the SCSI
# controller need no CPU, so they are tested too.
SERIAL_SRC := ../src/mac/scc.cpp ../src/mac/localtalk.cpp ../src/mac/modem.cpp ../src/mac/asc.cpp \
	      ../src/mac/scsi.cpp

test_core: test_core.cpp $(CORE_SRC) $(CORE_HDR) $(SERIAL_SRC) $(MAC_HDR)
	$(CXX) $(CXXFLAGS) -o $@ test_core.cpp $(CORE_SRC) $(SERIAL_SRC)

preview: preview.cpp $(CORE_SRC) $(CORE_HDR)
	$(CXX) $(CXXFLAGS) -o $@ preview.cpp $(CORE_SRC)

# PiMac's MMU instruction decoder replaces Musashi's m68kmmu.h.
$(MUSASHI)/m68kmmu.h: ../src/mac/musashi_mmu.h
	echo '#include "../../src/mac/musashi_mmu.h"' > $@

obj/musashi/%.o: $(MUSASHI)/%.c Makefile $(MUSASHI)/m68kmmu.h ../src/mac/musashi_mmu.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

hostmac: hostmac.cpp $(CORE_SRC) $(CORE_HDR) $(MAC_SRC) $(MAC_HDR) $(MUSASHI_OBJ)
	@test -f $(MUSASHI)/m68kops.c || { echo "Musashi missing: run 'make deps' in the project root"; exit 1; }
	$(CXX) $(CXXFLAGS) -O2 -I$(MUSASHI) -o $@ hostmac.cpp $(CORE_SRC) $(MAC_SRC) $(MUSASHI_OBJ)

mac: hostmac

run: test_core
	./test_core

previews: preview
	./preview screen.bmp
	./preview screen-menu.bmp menu
	./preview screen-diag.bmp diag

clean:
	rm -rf test_core preview hostmac obj out *.bmp *.png *.dSYM
