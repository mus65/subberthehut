VERSION   = 4

DISTFILES = Makefile subberthehut.c

PREFIX ?= /usr/local

CC      = gcc -std=gnu99

CFLAGS := -Wall -Wextra -pedantic -O2 \
          $(shell xmlrpc-c-config client --cflags) \
          $(shell pkg-config --cflags glib-2.0 zlib) \
	  -DVERSION=\"$(VERSION)\" \
          $(CFLAGS)

LDLIBS  = $(shell xmlrpc-c-config client --libs) \
          $(shell pkg-config --libs glib-2.0 zlib)

subberthehut: subberthehut.o

install: subberthehut
	install -D -m 755 subberthehut $(DESTDIR)$(PREFIX)/bin/subberthehut

uninstall:
	$(RM) $(DESTDIR)$(PREFIX)/bin/subberthehut

clean:
	$(RM) subberthehut subberthehut.o subberthehut-$(VERSION).tar.gz

dist:
	mkdir subberthehut-$(VERSION)
	cp $(DISTFILES) subberthehut-$(VERSION)
	tar czf subberthehut-$(VERSION).tar.gz subberthehut-$(VERSION)
	rm -rf subberthehut-$(VERSION)
