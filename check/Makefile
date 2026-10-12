# Explicit host qualification. These tools never run as part of a game build.
include src/transport.mk
include src/control.mk
OPENSSL ?= /opt/homebrew/opt/openssl@3
CC ?= cc
CHECK_CPPFLAGS = $(addprefix -I,$(TAILNET_INCDIR) $(CONTROL_INCDIR)) $(TAILNET_DEFS) $(CONTROL_DEFS)
CHECK_CFLAGS = -std=gnu11 -O2 -g
CHECK_TLS = -I$(OPENSSL)/include -L$(OPENSSL)/lib -lssl -lcrypto

build/check/control-host: src/control_host.c src/json.c $(TAILNET_SRC) $(CONTROL_SRC)
	@mkdir -p $(@D)
	$(CC) $(CHECK_CFLAGS) $(CHECK_CPPFLAGS) $^ $(CHECK_TLS) -o $@

build/check/tailnet-host: src/tailnet_host.c src/json.c $(TAILNET_SRC)
	@mkdir -p $(@D)
	$(CC) $(CHECK_CFLAGS) $(CHECK_CPPFLAGS) $^ $(CHECK_TLS) -o $@

CHECK_STORAGE = check/platform.c src/settings.c src/storage.c src/recording.c src/json.c src/pairing_protocol.c vendor/tweetnacl/tweetnacl.c

build/check/network-socket: check/network_socket.c check/platform.c src/net_socket.c
	@mkdir -p $(@D)
	$(CC) $(CHECK_CFLAGS) -DTARGET_EXTENSION=1 -I$(PLAYDATE_SDK_PATH)/C_API -Isrc $^ -o $@

build/check/storage: check/storage.c $(CHECK_STORAGE)
	@mkdir -p $(@D)
	$(CC) $(CHECK_CFLAGS) -DTARGET_EXTENSION=1 -I$(PLAYDATE_SDK_PATH)/C_API -Isrc -Ivendor/tweetnacl $^ -o $@

build/check/receiver: check/receiver.c check/network.c $(CHECK_STORAGE) src/pairing.c src/pairing_http.c src/chirp.c src/audio.c vendor/qrcodegen/qrcodegen.c
	@mkdir -p $(@D)
	$(CC) $(CHECK_CFLAGS) -DTARGET_EXTENSION=1 -I$(PLAYDATE_SDK_PATH)/C_API -Isrc -Ivendor/tweetnacl -Ivendor/qrcodegen $^ -lm -o $@
