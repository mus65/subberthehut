VERSION = 18

PREFIX ?= /usr/local

bash_completion_dir = $(shell pkg-config --silence-errors --variable=completionsdir bash-completion)

override CFLAGS := -std=gnu99 -Wall -Wextra -pedantic -O2 -D_FORTIFY_SOURCE=2 \
                   $(shell xmlrpc-c-config client --cflags) \
                   $(shell pkg-config --cflags glib-2.0 zlib) \
                   -DVERSION=\"$(VERSION)\" \
                   $(CFLAGS)

LDLIBS  = $(shell xmlrpc-c-config client --libs) \
          $(shell pkg-config --libs glib-2.0 zlib) \
          $(LDFLAGS)

subberthehut: subberthehut.o

install: subberthehut check-bash-completion
	install -pDm755 subberthehut $(DESTDIR)$(PREFIX)/bin/subberthehut
	install -pDm644 bash_completion $(DESTDIR)$(bash_completion_dir)/subberthehut

uninstall: check-bash-completion
	$(RM) $(DESTDIR)$(PREFIX)/bin/subberthehut
	$(RM) $(DESTDIR)$(bash_completion_dir)/subberthehut

clean:
	$(RM) subberthehut subberthehut.o

check-bash-completion:
ifeq ($(bash_completion_dir),)
	$(error bash-completion directory not found, please install bash-completion)
endif


.PHONY: install uninstall clean check-bash-completion
