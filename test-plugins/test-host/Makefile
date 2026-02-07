PKG_CFLAGS := $(shell pkg-config --cflags lv2)
PKG_LIBS := $(shell pkg-config --libs lv2)

CFLAGS := -O2 $(PKG_CFLAGS)
LDFLAGS := $(PKG_LIBS) -ldl

all: test-host

test-host: test-host.c
	$(CC) $(CFLAGS) -o $@ test-host.c $(LDFLAGS)

clean:
	rm -f test-host
