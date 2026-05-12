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
           -I$(INC)

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
