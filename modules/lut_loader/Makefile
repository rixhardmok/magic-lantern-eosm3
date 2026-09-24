# define the module name - make sure name is max 8 characters
#
# Deliberately "lutload" and not "lut_loader": MODULE_NAME_LENGTH in
# src/module.h is 8, the loader takes the module name from the FAT 8.3
# short name of the .mo file, and MODULE_NAME names every exported module
# symbol.  A 10 character name lands on the card as LUT_LO~1.MO, the
# loader then reads "lut_lo~1" and the module silently fails to load.
# If the .mo name really has to change, it still may not exceed 8 chars.
MODULE_NAME = lutload

MODULE_OBJS = $(BUILD_DIR)/lut_loader.o \
              $(BUILD_DIR)/mlut_parse.o \
              $(BUILD_DIR)/mlut_map.o \
              $(BUILD_DIR)/mlut_apply.o
TOP_DIR = ../..

# include modules environment
include $(TOP_DIR)/modules/Makefile
.DEFAULT_GOAL := $(BUILD_DIR)/module_complete
