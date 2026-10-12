HEAP_SIZE = 8388208
PRODUCT = Evener.pdx
SDK = $(PLAYDATE_SDK_PATH)
ifeq ($(SDK),)
$(error Set PLAYDATE_SDK_PATH to your Playdate SDK directory)
endif
include src/transport.mk
include src/control.mk
VPATH += src
SRC = src/main.c src/json.c src/websocket.c src/net_socket.c src/random.c src/tailnet_device.c \
    src/voice.c src/audio.c src/recording.c src/storage.c src/cache.c src/hub.c \
    src/settings.c src/pairing.c src/pairing_http.c src/pairing_protocol.c src/chirp.c vendor/qrcodegen/qrcodegen.c \
    $(TAILNET_SRC) $(CONTROL_SRC)
UINCDIR = src vendor/qrcodegen $(TAILNET_INCDIR) $(CONTROL_INCDIR)
UDEFS = -std=gnu11 $(TAILNET_DEFS) $(CONTROL_DEFS)
ULIBS = --specs=nosys.specs
include $(SDK)/C_API/buildsupport/common.mk
# The firmware owns a fixed 10 KB game-task stack, including nested SDK calls.
CPFLAGS += -fstack-usage -Werror=stack-usage=1536
build/vendor/tweetnacl/tweetnacl.o: CPFLAGS += -fno-inline
