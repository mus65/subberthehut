VERSION = 11

PREFIX ?= /usr/local

override CFLAGS := -std=gnu99 -Wall -Wextra -pedantic -O2 -D_FORTIFY_SOURCE=2 \
                   $(shell xmlrpc-c-config client --cflags) \
                   $(shell pkg-config --cflags glib-2.0 zlib) \
                   -DVERSION=\"$(VERSION)\" \
                   $(CFLAGS)

LDLIBS  = $(shell xmlrpc-c-config client --libs) \
          $(shell pkg-config --libs glib-2.0 zlib)

subberthehut: subberthehut.o

install: subberthehut
	install -pDm755 subberthehut $(DESTDIR)$(PREFIX)/bin/subberthehut

uninstall:
	$(RM) $(DESTDIR)$(PREFIX)/bin/subberthehut

clean:
	$(RM) subberthehut subberthehut.o

.PHONY: install uninstall clean
