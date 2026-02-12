CC = gcc
CFLAGS = -O2 -Wall -Wextra
LIBS = -lX11 -lasound -lcurl -lpthread

dictator: dictator.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

test_config: test_config.c dictator.c
	$(CC) $(CFLAGS) -o $@ test_config.c $(LIBS)

test: test_config
	./test_config

clean:
	rm -f dictator test_config

.PHONY: clean test
