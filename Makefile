obj-m := selfs.o

KDIR  ?= /lib/modules/$(shell uname -r)/build
PWD   := $(shell pwd)

# ---------------------------------------------------------------------------
# Userspace part
# ---------------------------------------------------------------------------

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
TEST_BIN := test_selfs
TEST_SRC := test_selfs.cpp

# ---------------------------------------------------------------------------
# Top-level targets
# ---------------------------------------------------------------------------

.PHONY: all module test clean help

all: module test

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

test: $(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) selfs_ioctl.h
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f $(TEST_BIN)

help:
	@echo "Targets: all | module | test | clean"
	@echo "Override KDIR=... to build against a different kernel tree."
