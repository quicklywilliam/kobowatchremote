include NickelHook/NickelHook.mk

override LIBRARY  := libkobo-ble-remote.so
override SOURCES  += src/kobo_ble_remote.cc
override CFLAGS   += -Wall -Wextra -Werror
override CXXFLAGS += -Wall -Wextra -Werror -Wno-missing-field-initializers
override PKGCONF  += Qt5Widgets
override MOCS     += src/resume_handler.h

include NickelHook/NickelHook.mk

# Build ble_peripheral alongside the plugin
ble_peripheral: ble_peripheral.c
	$(CC) $(CFLAGS) -o $@ $< -ldl -lpthread

all: ble_peripheral

.PHONY: all
