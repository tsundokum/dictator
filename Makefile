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

install: dictator
	sudo install -Dm755 dictator /usr/local/bin/dictator
	@mkdir -p ~/.config/dictator
	@if [ -f .env ] && [ ! -f ~/.config/dictator/.env ]; then \
		cp .env ~/.config/dictator/.env; \
		chmod 600 ~/.config/dictator/.env; \
		echo "Copied .env to ~/.config/dictator/.env"; \
	elif [ ! -f .env ]; then \
		echo "Warning: no .env file found. Create ~/.config/dictator/.env with GROQ=gsk_..."; \
	fi
	@mkdir -p ~/.config/systemd/user
	cp dictator.service ~/.config/systemd/user/dictator.service
	systemctl --user daemon-reload
	systemctl --user enable dictator.service
	@echo "Start with: systemctl --user start dictator.service"

uninstall:
	-systemctl --user stop dictator.service
	-systemctl --user disable dictator.service
	rm -f ~/.config/systemd/user/dictator.service
	systemctl --user daemon-reload
	sudo rm -f /usr/local/bin/dictator
	@echo "Removed binary and service. ~/.config/dictator/ left intact (contains API key)."

.PHONY: clean test install uninstall
