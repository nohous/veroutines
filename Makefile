VERILATOR ?= verilator

OUT_DIR = verilator_out
SV_SRC := src/axibox.sv
CPP_SRC := src/main.cpp
TOP_MODULE := axibox
TARGET_NAME := V$(TOP_MODULE)
TARGET := $(OUT_DIR)/$(TARGET_NAME)
VMAKEFILE_NAME := $(TARGET_NAME).mk
VMAKEFILE := $(OUT_DIR)/$(VMAKEFILE_NAME)
VM_USER_CFLAGS := -CFLAGS "--std=c++20 -g3"

all: $(TARGET)

$(TARGET): $(VMAKEFILE) | Makefile
	make -C $(OUT_DIR) -f $(VMAKEFILE_NAME)

$(VMAKEFILE): $(SV_SRC) $(CPP_SRC) | Makefile
	rm -f $(VMAKEFILE)
	$(VERILATOR) \
		-Mdir $(OUT_DIR)/ \
		--cc --exe --timing \
		--trace-vcd \
		$(VM_USER_CFLAGS) \
		$(SV_SRC) \
		$(CPP_SRC) \
		-Wno-lint \
		--top-module $(TOP_MODULE) 

clean:
	rm -rf $(OUT_DIR)

