PKG_CFLAGS := $(shell pkg-config --cflags lv2)
PKG_LIBS := $(shell pkg-config --libs lv2)

CFLAGS := -fPIC -O2 $(PKG_CFLAGS)
LDFLAGS := -shared $(PKG_LIBS)

all: atom-logger.so

atom-logger.so: atom-logger.c
	$(CC) $(CFLAGS) -o $@ atom-logger.c $(LDFLAGS)

clean:
	rm -f atom-logger.so
