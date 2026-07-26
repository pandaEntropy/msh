PREFIX ?= /usr/local

CC = gcc

HSHOBJ = hsh.o overlayrb.o
DEPFILES = hsh.d overlayrb.d

CFLAGS = -Wall -Wextra -MD -MP

all: hsh

hsh: $(HSHOBJ)
	$(CC) -o hsh $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: hsh
	install -d $(PREFIX)/bin/
	install -m 4755 -o root -g root hsh $(PREFIX)/bin/

uninstall:
	rm -f $(PREFIX)/bin/hsh

clean:
	rm -f hsh $(HSHOBJ) $(DEPFILES)

-include $(DEPFILES)

.PHONY: clean all install uninstall
