NAME=rds-channel
PREFIX=/usr/local/bin/
WIZARD=rds-add-channel
CFLAGS=-O3 -DNDEBUG -pedantic -std=c99 -Wall -Wextra -static
TARGET=$(shell gcc --print-multiarch)
BUILDIR=build/$(TARGET)

all: main.c ./hiredis/libhiredis.a
	mkdir -p $(BUILDIR)
	rm -rf $(BUILDIR)/*
	gcc $(CFLAGS) -I./hiredis main.c ./hiredis/libhiredis.a -o $(BUILDIR)/$(NAME)
	cp $(WIZARD) $(BUILDIR)/
	echo "# Project: $(NAME)" > $(BUILDIR)/info.md
	echo "" >> $(BUILDIR)/info.md
	echo "## Meta information" >> $(BUILDIR)/info.md
	echo "" >> $(BUILDIR)/info.md
	echo "* **Builder**: $(USER)"                >> $(BUILDIR)/info.md
	echo "* **Time**   : $(shell date)"          >> $(BUILDIR)/info.md
	echo "* **Machine**: $(shell uname -m)"      >> $(BUILDIR)/info.md
	echo "* **Target** : $(shell gcc --print-multiarch)" >> $(BUILDIR)/info.md
	echo "* **Tool**   : $(shell gcc --version | head -n 1)" >> $(BUILDIR)/info.md
	echo ""	                                     >> $(BUILDIR)/info.md
	echo "## Package listing"                    >> $(BUILDIR)/info.md
	echo ""                                      >> $(BUILDIR)/info.md
	cd $(BUILDIR) && ls -nph | grep -v / | grep -v Makefile       >> info.md
	echo ""                                      >> $(BUILDIR)/info.md
	echo "## Source listing"                     >> $(BUILDIR)/info.md 
	echo ""                                      >> $(BUILDIR)/info.md 
	ls -nph | grep -v / | grep -v Makefile       >> $(BUILDIR)/info.md

package: all
	tar -zcvf $(NAME)-$(TARGET).tar.gz -C $(BUILDIR) `ls $(BUILDIR)`

install: all
	install $(BUILDIR)/$(NAME) $(PREFIX)
	install $(BUILDIR)/$(WIZARD) $(PREFIX)

./hiredis/libhiredis.a: ./hiredis/hiredis.h                                                                   
	cd hiredis && make static 
