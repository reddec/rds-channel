NAME=rds-channel
PREFIX=/usr/local/bin/
WIZARD=rds-add-channel
CFLAGS=-O3 -DNDEBUG -pedantic -Wall -Wextra -std=c11 -static
BUILDIR=build/`gcc --print-multiarch`
all: main.c ../hiredis/hiredis.h
	mkdir -p $(BUILDIR)
	rm -rf $(BUILDIR)/*
	gcc $(CFLAGS) -I../hiredis main.c ../hiredis/libhiredis.a -o $(BUILDIR)/$(NAME)
	cp $(WIZARD) $(BUILDIR)/
	echo "# Project: $(NAME)" > $(BUILDIR)/info.md
	echo "" >> $(BUILDIR)/info.md
	echo "## Meta information" >> $(BUILDIR)/info.md
	echo "" >> $(BUILDIR)/info.md
	echo "* **Author** : $(USER)"                >> $(BUILDIR)/info.md
	echo "* **Time**   : $(shell date)"          >> $(BUILDIR)/info.md
	echo "* **Machine**: $(shell uname -m)"      >> $(BUILDIR)/info.md
	echo "* **Target** : $(shell gcc --print-multiarch)" >> $(BUILDIR)/info.md
	echo "* **Tool**   : $(shell gcc --version | head -n 1)" >> $(BUILDIR)/info.md
	echo ""	                                     >> $(BUILDIR)/info.md
	echo "## Package listing"                    >> $(BUILDIR)/info.md
	echo ""                                      >> $(BUILDIR)/info.md
	cd $(BUILDIR) && ls -nph | grep -v / | grep -v Makefile       >> info.md
	echo "## Source listing"                     >> $(BUILDIR)/info.md 
	echo ""                                      >> $(BUILDIR)/info.md 
	ls -nph | grep -v / | grep -v Makefile       >> $(BUILDIR)/info.md

install: all
	install $(BUILDIR)/$(NAME) $(PREFIX)
	install $(BUILDIR)/$(WIZARD) $(PREFIX)
