CC = cc
PKG_CONFIG ?= pkg-config
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libxxhash)
LDLIBS += $(shell $(PKG_CONFIG) --libs libxxhash)
USE_PANDOC ?= 0
ifeq ($(USE_PANDOC),1)
RENDERER_CPPFLAGS = -DUSE_PANDOC
else ifeq ($(USE_PANDOC),0)
RENDERER_CPPFLAGS = $(shell $(PKG_CONFIG) --cflags md4c-html)
LDLIBS += $(shell $(PKG_CONFIG) --libs md4c-html)
else
$(error USE_PANDOC must be 0 or 1)
endif
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow

.PHONY: all clean test
all: blog-server

blog-server: main.c .build/style.inc .build/copy.inc .build/renderer-$(USE_PANDOC)
	$(CC) $(CPPFLAGS) $(RENDERER_CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ main.c $(LDLIBS)

# Switching renderers must rebuild even when no source file has changed.
.build/renderer-$(USE_PANDOC):
	mkdir -p .build
	rm -f .build/renderer-*
	touch $@

.build/embed: tools/embed.c
	mkdir -p .build
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -O2 -o $@ $<

.build/style.inc: style.css .build/embed
	.build/embed stylesheet < style.css > $@.tmp
	mv $@.tmp $@

.build/copy.inc: copy.js .build/embed
	.build/embed copy_script < copy.js > $@.tmp
	mv $@.tmp $@

test: blog-server
	python3 tests/test_server.py ./blog-server

clean:
	rm -rf blog-server .build
