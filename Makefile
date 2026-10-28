# libresend. The shared library links libcurl and OpenSSL; it does not ship them.

CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -pthread
CPPFLAGS += -Iinclude -D_GNU_SOURCE $(shell pkg-config --cflags libcurl openssl)

CURL_LIBS := $(shell pkg-config --libs libcurl openssl)
ifeq ($(CURL_LIBS),)
$(error pkg-config did not find libcurl and openssl)
endif

VERSION := $(shell sed -n 's/^\#define LIBRESEND_VERSION "\([^"]*\)".*/\1/p' src/internal.h)
ifeq ($(VERSION),)
$(error LIBRESEND_VERSION missing from src/internal.h)
endif

PREFIX ?= /usr/local

BUILD := build
LIB   := $(BUILD)/libresend.a
SO    := $(BUILD)/libresend.so.0
CHECK := $(BUILD)/check

LIB_SRCS := $(wildcard src/*.c)
LIB_OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(LIB_SRCS))
PIC_OBJS := $(patsubst src/%.c,$(BUILD)/pic/%.o,$(LIB_SRCS))

.PHONY: all check clean install

all: $(LIB)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/pic:
	mkdir -p $(BUILD)/pic

$(BUILD)/%.o: src/%.c include/resend.h src/internal.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/pic/%.o: src/%.c include/resend.h src/internal.h | $(BUILD)/pic
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c $< -o $@

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(SO): $(PIC_OBJS)
	$(CC) $(CFLAGS) -shared -Wl,-soname,libresend.so.0 -Wl,-z,defs -o $@ $(PIC_OBJS) $(CURL_LIBS)

$(BUILD)/fake.o: tests/fake.c tests/fake.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c tests/fake.c -o $@

$(BUILD)/check.o: tests/check.c tests/fake.h include/resend.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c tests/check.c -o $@

$(CHECK): $(BUILD)/check.o $(BUILD)/fake.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $(BUILD)/check.o $(BUILD)/fake.o $(LIB) $(CURL_LIBS)

check: $(CHECK)
	$(CHECK)
	$(SHELL) tests/install.sh '$(MAKE)'

install: $(SO)
	install -d $(DESTDIR)$(PREFIX)/lib/pkgconfig $(DESTDIR)$(PREFIX)/include
	install -m 755 $(SO) $(DESTDIR)$(PREFIX)/lib/libresend.so.0
	ln -sfn libresend.so.0 $(DESTDIR)$(PREFIX)/lib/libresend.so
	install -m 644 include/resend.h $(DESTDIR)$(PREFIX)/include/resend.h
	@{ \
	  printf '%s\n' \
	    'prefix=$(PREFIX)' \
	    'exec_prefix=$${prefix}' \
	    'libdir=$${exec_prefix}/lib' \
	    'includedir=$${prefix}/include' \
	    '' \
	    'Name: libresend' \
	    'Description: Unofficial C client for the Resend API' \
	    'Version: $(VERSION)' \
	    'Requires.private: libcurl openssl' \
	    'Libs: -L$${libdir} -lresend' \
	    'Cflags: -I$${includedir}'; \
	} > $(DESTDIR)$(PREFIX)/lib/pkgconfig/libresend.pc
	chmod 644 $(DESTDIR)$(PREFIX)/lib/pkgconfig/libresend.pc

clean:
	rm -rf $(BUILD)
