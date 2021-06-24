DESTDIR ?=

TESTDIRS := $(shell find -mindepth 1 -maxdepth 1 -type d)

KOLA_TESTDIR ?= $(DESTDIR)/usr/lib/coreos-assembler/tests/kola/rpm-ostree/

rpm-repos: kolainst-build.sh
	./kolainst-build.sh

all: rpm-repos

install: all
	install -d -m 0755 $(KOLA_TESTDIR)
	rsync -prlv ./nondestructive $(KOLA_TESTDIR)/
	rsync -prlv ./destructive $(KOLA_TESTDIR)/
	rsync -prlv ../common/*.sh $(KOLA_TESTDIR)/nondestructive/data/
	rsync -prlv ../common/*.sh $(KOLA_TESTDIR)/destructive/data/
	rsync -prlv rpm-repos/ $(KOLA_TESTDIR)/destructive/data/rpm-repos/

localinstall: all
	rm -rf ../kola
	make install KOLA_TESTDIR=../kola
