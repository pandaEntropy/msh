CC = gcc

HSHOBJ = hsh.o overlayrb.o
DEPFILES = hsh.d overlayrb.d

CFLAGS = -Wall -Wextra -MD -MP

all: hsh

hsh: $(HSHOBJ)
	$(CC) -o hsh $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f hsh $(HSHOBJ) $(DEPFILES)

-include $(DEPFILES)

.PHONY: clean
