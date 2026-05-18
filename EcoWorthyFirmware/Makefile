CC      := sdcc
TARGET  := ecoworthy
BUILD   := build
SRC     := src
INC     := include

# SDCC for STC15F2K60S2 (8051 family, 60K flash / 2K RAM).
# --std-sdcc11: SDCC's C11 dialect (looser than --std-c11, allows __sfr etc).
# --opt-code-size: prefer smaller code over speed -- 60K is generous for now
#   but we'll get tight when LCD + bridge + tracking logic land in Phase 3.
CFLAGS  := -mmcs51 \
           --std-sdcc11 \
           --opt-code-size \
           --stack-auto \
           --lib-path "C:/Program Files/SDCC/lib/small-stack-auto" \
           -I$(INC)
# --stack-auto: reentrant calling convention -- function locals live on
# the internal-RAM stack (~223 bytes available) instead of SDCC's
# default fixed-overlay model.  The state machine + helper call graph
# exceeds the 128-byte overlay budget; this removes that ceiling.
# (--xstack was rejected: the bundled small-stack-auto libs aren't
# built with it.)  Explicit --lib-path because SDCC didn't auto-resolve
# the small-stack-auto lib dir on this Windows install.

SOURCES := $(SRC)/main.c
HEADERS := $(wildcard $(INC)/*.h)
HEX     := $(BUILD)/$(TARGET).ihx

.PHONY: all clean

all: $(HEX)

# Rebuild whenever any source or header changes. SDCC produces the .ihx
# alongside .lst/.map/.mem/.rst -- those land in $(BUILD)/ as a side effect.
$(HEX): $(SOURCES) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SOURCES)

$(BUILD):
	-mkdir $(BUILD)

clean:
	-rmdir /s /q $(BUILD)
