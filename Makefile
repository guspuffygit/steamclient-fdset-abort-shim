CFLAGS = -O2 -Wall -Wextra -U_FORTIFY_SOURCE -fno-stack-protector

all: fdelt_shim.so repro

fdelt_shim.so: fdelt_shim.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $<

repro: repro.c
	$(CC) $(CFLAGS) -rdynamic -o $@ $< -ldl -lpthread

clean:
	rm -f fdelt_shim.so repro
