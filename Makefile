# libresend. Dynamic libcurl and libssl. install and pkg-config come later.
#
# src/pending.c fails every call so this suite can link before the client
# exists. It is left out once any other src/*.c is present.

CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -pthread
CPPFLAGS += -Iinclude -D_GNU_SOURCE $(shell pkg-config --cflags libcurl openssl)

CURL_LIBS := $(shell pkg-config --libs libcurl openssl)
ifeq ($(CURL_LIBS),)
$(error pkg-config did not find libcurl and openssl)
endif

BUILD := build
LIB   := $(BUILD)/libresend.a
CHECK := $(BUILD)/check

REAL_SRCS := $(filter-out src/pending.c,$(wildcard src/*.c))
ifeq ($(REAL_SRCS),)
LIB_SRCS := src/pending.c
else
LIB_SRCS := $(REAL_SRCS)
endif
LIB_OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(LIB_SRCS))

.PHONY: all check clean

all: $(LIB)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c include/resend.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(BUILD)/fake.o: tests/fake.c tests/fake.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c tests/fake.c -o $@

$(BUILD)/check.o: tests/check.c tests/fake.h include/resend.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c tests/check.c -o $@

$(CHECK): $(BUILD)/check.o $(BUILD)/fake.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $(BUILD)/check.o $(BUILD)/fake.o $(LIB) $(CURL_LIBS)

check: $(CHECK)
	$(CHECK)

clean:
	rm -rf $(BUILD)
