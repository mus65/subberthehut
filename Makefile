VERSION   = $(shell git describe)
DISTFILES = Makefile subberthehut.c

CC      = gcc -std=gnu99
CFLAGS := -Wall -Wextra -pedantic $(shell xmlrpc-c-config client --cflags) $(shell pkg-config --cflags glib-2.0 zlib) $(CFLAGS)
LDLIBS  = $(shell xmlrpc-c-config client --libs) $(shell pkg-config --libs glib-2.0 zlib)

subberthehut: subberthehut.o

clean:
	$(RM) subberthehut subberthehut.o subberthehut-$(VERSION).tar.gz

dist:
	mkdir subberthehut-$(VERSION)
	cp $(DISTFILES) subberthehut-$(VERSION)
	tar czf subberthehut-$(VERSION).tar.gz subberthehut-$(VERSION)
	rm -rf subberthehut-$(VERSION)
