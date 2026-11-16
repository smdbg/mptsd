CC ?= $(CROSS)$(TARGET)gcc
PYTHON ?= python3
PREFIX ?= /usr/local
STRIP = $(CROSS)$(TARGET)strip
VERSION="v1.1"
ifndef REPRODUCIBLE_BUILD
BUILD_ID = $(shell date +%F_%R)
GIT_VER = $(shell git describe --tags --dirty --always 2>/dev/null)
endif
CFLAGS ?= -ggdb -O2
CFLAGS += -Wall -Wextra -Wshadow -Wformat-security -Wno-strict-aliasing -D_GNU_SOURCE -DBUILD_ID=\"$(BUILD_ID)\"
ifneq "$(GIT_VER)" ""
CFLAGS += -DGIT_VER=\"$(GIT_VER)\"
else
CFLAGS += -DGIT_VER=\"$(VERSION)\"
endif

RM = /bin/rm -f
Q = @

uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')
ifeq ($(uname_S),Darwin)
LIBS = -lpthread -lm
else
LIBS = -lpthread -lm -lrt
endif

FUNCS_DIR = libfuncs
FUNCS_LIB = $(FUNCS_DIR)/libfuncs.a

TSFUNCS_DIR = libtsfuncs
TSFUNCS_LIB = $(TSFUNCS_DIR)/libtsfuncs.a

mptsd_OBJS =  $(FUNCS_LIB) $(TSFUNCS_LIB) \
	iniparser.o inidict.o pidref.o data.o config.o \
	sleep.o network.o \
	input.o eit_forward.o cas_remux.o \
	output_psi.o output_mix.o output_write.o output_status.o \
	web_pages.o web_server.o \
	mptsd.o

PROGS = mptsd
CLEAN_OBJS = $(PROGS) $(mptsd_OBJS) *~

all: $(PROGS)

$(FUNCS_LIB): FORCE
	$(Q)echo "  MAKE	$(FUNCS_LIB)"
	$(Q)$(MAKE) -s -C $(FUNCS_DIR)

$(TSFUNCS_LIB): FORCE
	$(Q)echo "  MAKE	$(TSFUNCS_LIB)"
	$(Q)$(MAKE) -s -C $(TSFUNCS_DIR)

mptsd: $(mptsd_OBJS)
	$(Q)echo "  LINK	mptsd"
	$(Q)$(CC) $(CFLAGS) $(mptsd_OBJS) $(LIBS) -o mptsd

%.o: %.c data.h config.h eit_forward.h output_status.h cas_remux.h
	$(Q)echo "  CC	mptsd		$<"
	$(Q)$(CC) $(CFLAGS)  -c $<

strip:
	$(Q)echo "  STRIP	$(PROGS)"
	$(Q)$(STRIP) $(PROGS)

clean:
	$(Q)echo "  RM	$(CLEAN_OBJS)"
	$(Q)$(RM) $(CLEAN_OBJS)

distclean: clean
	$(Q)$(MAKE) -s -C $(TSFUNCS_DIR) clean
	$(Q)$(MAKE) -s -C $(FUNCS_DIR) clean

.PHONY: FORCE
FORCE:

test: mptsd
	$(PYTHON) tests/mptsd_protocol_test.py
	$(PYTHON) tests/mptsd_cas_test.py

install: mptsd
	install -Dm755 mptsd $(DESTDIR)$(PREFIX)/bin/mptsd
	install -Dm644 COPYING $(DESTDIR)$(PREFIX)/share/doc/mptsd/COPYING
	install -Dm644 README.md $(DESTDIR)$(PREFIX)/share/doc/mptsd/README.md
	install -Dm644 CHANGELOG.md $(DESTDIR)$(PREFIX)/share/doc/mptsd/CHANGELOG.md
	install -Dm644 docs/CONFIGURATION.md $(DESTDIR)$(PREFIX)/share/doc/mptsd/docs/CONFIGURATION.md
	install -Dm644 docs/CAS.md $(DESTDIR)$(PREFIX)/share/doc/mptsd/docs/CAS.md
	install -Dm644 docs/PROVENANCE.md $(DESTDIR)$(PREFIX)/share/doc/mptsd/docs/PROVENANCE.md
	install -Dm644 libfuncs/LICENSE-MIT.txt $(DESTDIR)$(PREFIX)/share/doc/mptsd/libfuncs-LICENSE-MIT.txt
	install -Dm644 libtsfuncs/LICENSE-MIT.txt $(DESTDIR)$(PREFIX)/share/doc/mptsd/libtsfuncs-LICENSE-MIT.txt
	install -d $(DESTDIR)$(PREFIX)/share/doc/mptsd/examples
	install -m644 examples/*.conf $(DESTDIR)$(PREFIX)/share/doc/mptsd/examples/

.PHONY: all clean distclean install test
