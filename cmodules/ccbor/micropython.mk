# ccbor for ports built with make (unix, stm32, rp2 with make, ...).
CCBOR_MOD_DIR := $(USERMOD_DIR)

SRC_USERMOD_C += $(CCBOR_MOD_DIR)/modccbor.c
CFLAGS_USERMOD += -I$(CCBOR_MOD_DIR)
