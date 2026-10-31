# mod_orx -- build the Apache module and the Rexx host.
#
#   make                  build both
#   make install          install both (needs root)
#
# APXS and OOREXX_PREFIX can be overridden on the command line.

APXS          ?= apxs
OOREXX_PREFIX ?= /usr/local
CXX           ?= g++
CXXFLAGS      ?= -O2 -Wall
BINDIR        ?= /usr/local/bin

all: module host

module: module/.libs/mod_orx.so

module/.libs/mod_orx.so: module/mod_orx.c include/oorexx_protocol.h include/mod_orx.h build/config.h
	$(APXS) -c -Wc,-Wall module/mod_orx.c

# Where the module looks for oorexx-host when OoRexxHost is not set.
build/config.h: Makefile
	mkdir -p build
	printf '#define OORX_DEFAULT_HOST "%s"\n' "$(BINDIR)/oorexx-host" > $@

host: build/oorexx-host

build/oorexx-host: host/oorexx-host.cpp include/oorexx_protocol.h include/orx_host_plugin.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ host/oorexx-host.cpp \
	    -I$(OOREXX_PREFIX)/include -L$(OOREXX_PREFIX)/lib \
	    -Wl,-rpath,$(OOREXX_PREFIX)/lib -lrexx -lrexxapi -lpthread

install: all
	$(APXS) -i -n orx module/.libs/mod_orx.so
	install -m 755 build/oorexx-host $(BINDIR)/oorexx-host

clean:
	rm -rf build module/.libs module/*.lo module/*.la module/*.slo module/*.o

.PHONY: all module host install clean
