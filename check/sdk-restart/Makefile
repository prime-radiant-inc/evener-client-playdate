PRODUCT = TCPRestart.pdx
HEAP_SIZE = 8388208
SDK = $(PLAYDATE_SDK_PATH)
ifeq ($(SDK),)
$(error Set PLAYDATE_SDK_PATH to the Playdate SDK 3.1.2 directory)
endif
SRC = main.c
UINCDIR = .
UDEFS = -std=gnu11
ULIBS = --specs=nosys.specs
include $(SDK)/C_API/buildsupport/common.mk
CPFLAGS += -fstack-usage -Werror=stack-usage=1536
