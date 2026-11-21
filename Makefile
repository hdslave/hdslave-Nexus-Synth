# Nexus: Plaits' 24 models as a CLAP synthesizer for the Akai MPC Sample (64-bit ARM Linux, Cortex-A55).
# Build inside a Linux arm64 image with ./build.sh, which runs `make all test`.
#   make            build/Nexus.clap
#   make test       the tests against the built plug-in (tests/host.cpp) and the sound's fingerprint
#                   (tests/fingerprint.cpp: the output to the bit, against tests/fingerprint.txt)
#   make bench      build/bench: renders busy patches; tools/bench.sh counts their instructions
#   make sanitize   the host tests against a build with AddressSanitizer and UBSan (any error stops it)
#   make render     build/render: renders a note script (tests/render.cpp), for listening and comparing
CXX ?= g++
OUT := build
WARN := -Wall -Wextra -Werror
# The MPC's cores. -ffp-contract=fast lets the compiler fuse multiply-adds; nothing here relies on IEEE errno, traps or
# exceptions.
OPT := -O3 -mcpu=cortex-a55 -ffp-contract=fast -fno-math-errno -fno-trapping-math
# Mutable Instruments' code (third_party/eurorack) builds as the module's test builds do (TEST: no ARM assembly).
BASE := -std=c++17 -fno-exceptions -fno-rtti -fvisibility=hidden -fPIC -DTEST -isystem third_party/eurorack -Ithird_party/clap/include -Isrc
CXXFLAGS := $(BASE) $(OPT)
# Only clap_entry is exported; libstdc++ and libgcc are linked in so the MPC needs nothing beyond libc and libm. -s leaves
# out the symbol table.
LDFLAGS := -shared -s -static-libgcc -static-libstdc++ -Wl,--gc-sections -Wl,-z,now -Wl,-O1 -Wl,--version-script=src/exports.map

VENDOR := $(sort $(filter-out third_party/eurorack/plaits/dsp/voice.cc,$(shell find third_party/eurorack -name '*.cc')))
VENDOR_HEADERS := $(shell find third_party/eurorack -name '*.h')
HEADERS := $(wildcard src/*.h src/ui/*.h) $(VENDOR_HEADERS)
VENDOR_OBJ := $(patsubst %.cc,$(OUT)/obj/%.o,$(VENDOR))
OBJ := $(VENDOR_OBJ) $(OUT)/obj/plaits_voice.o

.PHONY: all test bench clean fingerprint-update sanitize render
all: $(OUT)/Nexus.clap

$(OUT)/obj/%.o: %.cc $(VENDOR_HEADERS) Makefile
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -w -ffunction-sections -fdata-sections -c $< -o $@

$(OUT)/obj/plaits_voice.o: src/plaits_voice.cc src/plaits_voice.h $(VENDOR_HEADERS) Makefile
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(WARN) -ffunction-sections -fdata-sections -c $< -o $@

$(OUT)/obj/nexus.o: src/nexus.cpp $(HEADERS) Makefile
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(WARN) -ffunction-sections -fdata-sections -c $< -o $@

$(OUT)/Nexus.clap: $(OUT)/obj/nexus.o $(OBJ) src/exports.map
	$(CXX) $(CXXFLAGS) $(OUT)/obj/nexus.o $(OBJ) $(LDFLAGS) -o $@

$(OUT)/host: tests/host.cpp tests/wav.h tests/png.h $(HEADERS) Makefile
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 -O2 $(WARN) -Wno-stringop-overflow -isystem third_party/eurorack -Ithird_party/clap/include -DTEST tests/host.cpp -ldl -o $@

$(OUT)/fingerprint: tests/fingerprint.cpp Makefile
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 -O2 $(WARN) -Ithird_party/clap/include tests/fingerprint.cpp -ldl -o $@

$(OUT)/render: tests/render.cpp tests/wav.h src/params.h Makefile
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 -O2 $(WARN) -Ithird_party/clap/include -Isrc tests/render.cpp -ldl -o $@

render: $(OUT)/render $(OUT)/Nexus.clap

test: $(OUT)/Nexus.clap $(OUT)/host $(OUT)/fingerprint
	$(OUT)/host $(OUT)/Nexus.clap $(OUT)
	$(OUT)/fingerprint $(OUT)/Nexus.clap tests/fingerprint.txt > /dev/null

# The sound's hashes after a change meant to change the sound (see tests/fingerprint.cpp).
fingerprint-update: $(OUT)/Nexus.clap $(OUT)/fingerprint
	$(OUT)/fingerprint $(OUT)/Nexus.clap > tests/fingerprint.txt

SAN := -fsanitize=address,undefined -fno-sanitize=shift-base -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g -O1
sanitize: $(HEADERS) src/nexus.cpp src/plaits_voice.cc tests/host.cpp tests/wav.h tests/png.h Makefile
	@mkdir -p $(OUT)/san
	$(CXX) $(BASE) $(SAN) -mcpu=cortex-a55 -w -shared src/nexus.cpp src/plaits_voice.cc $(VENDOR) -o $(OUT)/san/Nexus.clap
	$(CXX) -std=c++17 $(SAN) $(WARN) -Wno-stringop-overflow -isystem third_party/eurorack -Ithird_party/clap/include -DTEST tests/host.cpp -ldl -o $(OUT)/san/host
	ASAN_OPTIONS=abort_on_error=1:detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1:print_stacktrace=1 \
	    $(OUT)/san/host $(OUT)/san/Nexus.clap $(OUT)/san

$(OUT)/bench: tests/bench.cpp src/nexus.cpp $(OBJ) $(HEADERS) Makefile
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(WARN) -g tests/bench.cpp $(OBJ) -o $@

bench: $(OUT)/bench

clean:
	rm -rf $(OUT)
