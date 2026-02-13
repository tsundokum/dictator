CC = gcc
CFLAGS = -O2 -Wall -Wextra $(shell pkg-config --cflags libevdev)
BACKEND_FLAGS = -DUSE_X11 -DUSE_EVDEV
LIBS = -lX11 -lasound -lcurl -lpthread $(shell pkg-config --libs libevdev)

dictator: dictator.c
	$(CC) $(CFLAGS) $(BACKEND_FLAGS) -o $@ $< $(LIBS)

test_config: test_config.c dictator.c
	$(CC) $(CFLAGS) $(BACKEND_FLAGS) -o $@ test_config.c $(LIBS)

test_audio: test_audio.c dictator.c
	$(CC) $(CFLAGS) $(BACKEND_FLAGS) -o $@ test_audio.c $(LIBS)

test: test_config test_audio
	./test_config && ./test_audio

test_e2e: test_e2e.c dictator.c
	$(CC) $(CFLAGS) $(BACKEND_FLAGS) -o $@ test_e2e.c $(LIBS)

e2e: test_e2e
	./test_e2e

clean:
	rm -f dictator test_config test_audio test_e2e

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
	@echo ""
	@if [ "$$(echo $$XDG_SESSION_TYPE)" = "wayland" ]; then \
		command -v wl-copy  >/dev/null || echo "Warning: wl-copy not found — install wl-clipboard: sudo apt install wl-clipboard"; \
		command -v ydotool  >/dev/null || echo "Warning: ydotool not found — sudo apt install ydotool"; \
	else \
		command -v xclip   >/dev/null || echo "Warning: xclip not found — sudo apt install xclip"; \
		command -v xdotool >/dev/null || echo "Warning: xdotool not found — sudo apt install xdotool"; \
	fi

uninstall:
	-systemctl --user stop dictator.service
	-systemctl --user disable dictator.service
	rm -f ~/.config/systemd/user/dictator.service
	systemctl --user daemon-reload
	sudo rm -f /usr/local/bin/dictator
	@echo "Removed binary and service. ~/.config/dictator/ left intact (contains API key)."

.PHONY: clean test e2e install uninstall
