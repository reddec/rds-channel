NAME=rds-channel
CC?=gcc
PREFIX?=/usr/local/bin
WIZARD=rds-add-channel
CFLAGS?=-O3 -DNDEBUG -pedantic -std=c99 -Wall -Wextra -static
TARGET=$(shell uname -m)
BUILDIR=build/$(TARGET)

all: package
	echo "Done"

clean: ./hiredis/Makefile
	cd ./hiredis && $(MAKE) clean
	rm -rf $(BUILDIR)
	rm -rf $(NAME).tar.gz

$(BUILDIR)/$(NAME): main.c ./hiredis/libhiredis.a
	mkdir -p $(BUILDIR)
	rm -rf $(BUILDIR)/*
	$(CC) $(CFLAGS) -I./hiredis main.c ./hiredis/libhiredis.a -o $(BUILDIR)/$(NAME)

$(BUILDIR)/$(WIZARD): $(WIZARD)
	cp $(WIZARD) $(BUILDIR)/

$(BUILDIR)/info.md: $(BUILDIR)/$(WIZARD) $(BUILDIR)/$(NAME)
	echo "# Project: $(NAME)" > $(BUILDIR)/info.md
	echo "" >> $(BUILDIR)/info.md
	echo "## Meta information" >> $(BUILDIR)/info.md
	echo "" >> $(BUILDIR)/info.md
	echo "* **Builder**: $(USER)"                >> $(BUILDIR)/info.md
	echo "* **Time**   : $(shell date)"          >> $(BUILDIR)/info.md
	echo "* **Machine**: $(shell uname -m)"      >> $(BUILDIR)/info.md
	echo "* **Tool**   : $(shell $(CC) --version | head -n 1)" >> $(BUILDIR)/info.md
	echo ""	                                     >> $(BUILDIR)/info.md
	echo "## Package listing"                    >> $(BUILDIR)/info.md
	echo ""                                      >> $(BUILDIR)/info.md
	cd $(BUILDIR) && ls -nph | grep -v / | grep -v Makefile       >> info.md
	echo ""                                      >> $(BUILDIR)/info.md
	echo "## Source listing"                     >> $(BUILDIR)/info.md 
	echo ""                                      >> $(BUILDIR)/info.md 
	ls -nph | grep -v / | grep -v Makefile       >> $(BUILDIR)/info.md


package: $(BUILDIR)/$(NAME) $(BUILDIR)/$(WIZARD) $(BUILDIR)/info.md
	tar -zcvf $(NAME).tar.gz -C $(BUILDIR) `ls $(BUILDIR)`

install: $(BUILDIR)/$(NAME) $(BUILDIR)/$(WIZARD)
	install $(BUILDIR)/$(NAME) $(PREFIX)
	install $(BUILDIR)/$(WIZARD) $(PREFIX)

./hiredis/libhiredis.a: ./hiredis/hiredis.h                                                                   
	cd hiredis && make static 
