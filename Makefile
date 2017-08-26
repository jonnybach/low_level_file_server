CFLAGS := -Wall --std=gnu99 -g3 -Wno-format-security -Werror -fsanitize=address -fno-omit-frame-pointer
CURL_LIBS := $(shell curl-config --libs)
CURL_CFLAGS := $(shell curl-config --cflags)

ARCH := $(shell uname)
ifneq ($(ARCH),Darwin)
  LDFLAGS += -lpthread -lrt -static-libasan
endif

all: gfclient_download webproxy simplecached

gfclient_download: gfclient_download.c gfclient.c workload.c steque.c

webproxy: webproxy.o gfserver.o handlers.o shm_channel.o steque.o
	$(CC) -o $@ $(CFLAGS) $(CURL_CFLAGS) $^ $(LDFLAGS) $(CURL_LIBS)

simplecached: simplecache.o simplecached.o shm_channel.o steque.o
	$(CC) -o $@ $(CFLAGS) $^ $(LDFLAGS)

.PHONY: clean

clean:
	rm -rf *.o gfclient_download webproxy simplecached
