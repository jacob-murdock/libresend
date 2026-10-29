# libresend. The shared library links libcurl and OpenSSL; it does not ship them.

CFLAGS ?= -std=c99 -Wall -Wextra -Werror -pthread -O2
# Command-line CFLAGS replaces the line above. This still applies.
override CFLAGS += -fstack-protector-strong
CPPFLAGS += -Iinclude -D_GNU_SOURCE

VERSION := $(shell sed -n 's/^\#define LIBRESEND_VERSION "\([^"]*\)".*/\1/p' include/resend.h)
ifeq ($(VERSION),)
$(error LIBRESEND_VERSION missing from include/resend.h)
endif

# dist and clean need only git and sha256sum.
ifneq ($(filter-out dist clean,$(or $(MAKECMDGOALS),all)),)
CURL_LIBS := $(shell pkg-config --libs libcurl openssl)
ifeq ($(CURL_LIBS),)
$(error pkg-config did not find libcurl and openssl)
endif
CPPFLAGS += $(shell pkg-config --cflags libcurl openssl)
endif

# After any distro -D_FORTIFY_SOURCE, so -Werror does not see a redefinition.
CPPFLAGS += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2

PREFIX ?= /usr/local
# Override for lib64. install, uninstall, and the .pc libdir use this.
LIBDIR ?= $(PREFIX)/lib

BUILD := build
LIB   := $(BUILD)/libresend.a
SO_NAME := libresend.so.$(VERSION)
SO    := $(BUILD)/$(SO_NAME)
CHECK := $(BUILD)/check
HEADER_C99 := $(BUILD)/header_c99.o

# Hide internals in the shared library. Public symbols use RESEND_API.
LIB_CFLAGS := $(CFLAGS) -fvisibility=hidden

LIB_SRCS := $(wildcard src/*.c)
LIB_OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(LIB_SRCS))
PIC_OBJS := $(patsubst src/%.c,$(BUILD)/pic/%.o,$(LIB_SRCS))

EXAMPLES := send_email send_batch_email schedule_email with_attachments \
	receiving domains api_keys webhook
EXAMPLE_BINS := $(addprefix $(BUILD)/examples/,$(EXAMPLES))

.PHONY: all check examples clean install uninstall dist

all: $(LIB)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/pic:
	mkdir -p $(BUILD)/pic

$(BUILD)/%.o: src/%.c include/resend.h src/internal.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(LIB_CFLAGS) -c $< -o $@

$(BUILD)/pic/%.o: src/%.c include/resend.h src/internal.h | $(BUILD)/pic
	$(CC) $(CPPFLAGS) $(LIB_CFLAGS) -fPIC -c $< -o $@

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(SO): $(PIC_OBJS)
	$(CC) $(CFLAGS) -shared -Wl,-soname,libresend.so.0 -Wl,-z,defs -Wl,-z,relro,-z,now -o $@ $(PIC_OBJS) $(CURL_LIBS)

# Empty translation unit: header is self-contained under C99 -pedantic.
$(HEADER_C99): include/resend.h | $(BUILD)
	printf '%s\n' '#include "resend.h"' | \
	  $(CC) -std=c99 -pedantic -Wall -Wextra -Werror -Iinclude -c -x c - -o $@

$(BUILD)/fake.o: tests/fake.c tests/fake.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c tests/fake.c -o $@

$(BUILD)/check.o: tests/check.c tests/fake.h include/resend.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c tests/check.c -o $@

$(BUILD)/limits.o: tests/limits.c src/internal.h include/resend.h | $(BUILD)
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) -c tests/limits.c -o $@

$(CHECK): $(BUILD)/check.o $(BUILD)/fake.o $(BUILD)/limits.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $(BUILD)/check.o $(BUILD)/fake.o $(BUILD)/limits.o $(LIB) $(CURL_LIBS)

check: $(CHECK) $(HEADER_C99)
	$(CHECK)
	$(SHELL) tests/build.sh '$(MAKE)'
	$(SHELL) tests/install.sh '$(MAKE)'
	$(SHELL) tests/dist.sh '$(MAKE)'

$(BUILD)/examples:
	mkdir -p $(BUILD)/examples

$(BUILD)/examples/%: examples/%.c include/resend.h $(LIB) | $(BUILD)/examples
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LIB) $(CURL_LIBS)

examples: $(EXAMPLE_BINS)

install: $(LIB) $(SO)
	install -d $(DESTDIR)$(LIBDIR)/pkgconfig $(DESTDIR)$(PREFIX)/include
	install -m 644 $(LIB) $(DESTDIR)$(LIBDIR)/libresend.a
	install -m 755 $(SO) $(DESTDIR)$(LIBDIR)/$(SO_NAME)
	ln -sfn $(SO_NAME) $(DESTDIR)$(LIBDIR)/libresend.so.0
	ln -sfn libresend.so.0 $(DESTDIR)$(LIBDIR)/libresend.so
	install -m 644 include/resend.h $(DESTDIR)$(PREFIX)/include/resend.h
	@{ \
	  printf '%s\n' \
	    'prefix=$(PREFIX)' \
	    'exec_prefix=$${prefix}' \
	    'libdir=$${exec_prefix}/$(patsubst $(PREFIX)/%,%,$(LIBDIR))' \
	    'includedir=$${prefix}/include' \
	    '' \
	    'Name: libresend' \
	    'Description: Unofficial C client for the Resend API' \
	    'Version: $(VERSION)' \
	    'Requires.private: libcurl openssl' \
	    'Libs: -L$${libdir} -lresend' \
	    'Cflags: -I$${includedir}'; \
	} > $(DESTDIR)$(LIBDIR)/pkgconfig/libresend.pc
	chmod 644 $(DESTDIR)$(LIBDIR)/pkgconfig/libresend.pc

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/libresend.a \
		$(DESTDIR)$(LIBDIR)/libresend.so \
		$(DESTDIR)$(LIBDIR)/libresend.so.0 \
		$(DESTDIR)$(LIBDIR)/$(SO_NAME) \
		$(DESTDIR)$(LIBDIR)/pkgconfig/libresend.pc \
		$(DESTDIR)$(PREFIX)/include/resend.h

DIST_TAR := $(BUILD)/libresend-$(VERSION).tar.gz
DIST_SUM := $(DIST_TAR).sha256

dist:
	mkdir -p $(BUILD)
	rm -f $(DIST_TAR) $(DIST_SUM)
	git archive --format=tar.gz --prefix=libresend-$(VERSION)/ -o $(DIST_TAR) HEAD
	cd $(BUILD) && sha256sum libresend-$(VERSION).tar.gz > libresend-$(VERSION).tar.gz.sha256

clean:
	rm -rf $(BUILD)
