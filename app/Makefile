TARGET = hbstore
OBJS = src/main.o src/ui.o src/net.o src/catalogue.o src/zipx.o src/install.o src/util.o src/tweetnacl.o

# Set the relay at build time: make RELAY=http://psp-homebrew-relay.<you>.workers.dev
RELAY ?= http://psp-homebrew-relay.YOUR-SUBDOMAIN.workers.dev

CFLAGS = -O2 -G0 -Wall -DHB_DEFAULT_RELAY=\"$(RELAY)\"
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBS = -lz -lpspgu -lpsputility -lpspnet_resolver -lpspnet_inet -lpspnet_apctl -lpspnet

BUILD_PRX = 1
EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PSP Homebrew Store

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
