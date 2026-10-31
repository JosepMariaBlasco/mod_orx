# mod_orx -- build the Apache module and the Rexx host, and, optionally,
# the mod_rexx layer (an Apache module and a plugin of the host).
#
#   make                  build the core: mod_orx.so and oorexx-host
#   make install          install the core (needs root)
#   make modrexx          build the mod_rexx layer: mod_orx_rexx.so and
#                         orx_rexx_plugin.so
#   make install-modrexx  install the mod_rexx layer (needs root)
#
# APXS, OOREXX_PREFIX, BINDIR and PLUGINDIR can be overridden on the
# command line.

APXS          ?= apxs
OOREXX_PREFIX ?= /usr/local
CXX           ?= g++
CXXFLAGS      ?= -O2 -Wall
BINDIR        ?= /usr/local/bin
PLUGINDIR     ?= /usr/local/lib/mod_orx

OOREXX_FLAGS = -I$(OOREXX_PREFIX)/include -L$(OOREXX_PREFIX)/lib \
               -Wl,-rpath,$(OOREXX_PREFIX)/lib -lrexx -lrexxapi

all: module host

module: module/.libs/mod_orx.so

module/.libs/mod_orx.so: module/mod_orx.c include/oorexx_protocol.h include/mod_orx.h build/config.h
	$(APXS) -c -Wc,-Wall module/mod_orx.c

# Where the modules look for oorexx-host (when OoRexxHost is not set) and
# for the plugin of the mod_rexx layer.
build/config.h: Makefile
	mkdir -p build
	printf '#define OORX_DEFAULT_HOST "%s"\n#define ORX_REXX_PLUGIN "%s"\n' \
	    "$(BINDIR)/oorexx-host" "$(PLUGINDIR)/orx_rexx_plugin.so" > $@

host: build/oorexx-host

build/oorexx-host: host/oorexx-host.cpp include/oorexx_protocol.h include/orx_host_plugin.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ host/oorexx-host.cpp $(OOREXX_FLAGS) -lpthread -ldl

install: all
	$(APXS) -i -n orx module/.libs/mod_orx.so
	install -m 755 build/oorexx-host $(BINDIR)/oorexx-host

modrexx: modrexx/.libs/mod_orx_rexx.so build/orx_rexx_plugin.so

modrexx/.libs/mod_orx_rexx.so: modrexx/mod_orx_rexx.c include/oorexx_protocol.h include/mod_orx.h build/config.h
	$(APXS) -c -Wc,-Wall modrexx/mod_orx_rexx.c

build/orx_rexx_plugin.so: modrexx/orx_rexx_plugin.cpp include/oorexx_protocol.h include/orx_host_plugin.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -shared -fPIC -o $@ modrexx/orx_rexx_plugin.cpp $(OOREXX_FLAGS)

install-modrexx: modrexx
	$(APXS) -i -n orx_rexx modrexx/.libs/mod_orx_rexx.so
	mkdir -p $(PLUGINDIR)
	install -m 644 build/orx_rexx_plugin.so $(PLUGINDIR)/orx_rexx_plugin.so

clean:
	rm -rf build module/.libs module/*.lo module/*.la module/*.slo module/*.o \
	       modrexx/.libs modrexx/*.lo modrexx/*.la modrexx/*.slo modrexx/*.o

.PHONY: all module host install modrexx install-modrexx clean
