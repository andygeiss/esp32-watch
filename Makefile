# Copied from the baseline (stack/makefile.md) with CMake in place of the Go
# toolchain. The dropped targets are recorded in CLAUDE.md.

# The simulator binary CMake writes.
BIN = build/kai_sim

# Targets are alphabetical, so the default is named rather than first.
.DEFAULT_GOAL = check
.PHONY: bench build check ci clean firmware flash run test

# Where a turn's seconds go, against whichever server .env points at. Takes a
# run count and a brain: `make bench RUNS=5 BRAIN=some-faster-model`. Sources
# .env the way run does and for the same reason — it needs an address and a
# key — which is exactly why it is not in check: a gate that reads one
# machine's file is a gate that passes on one machine, and this one also needs
# a server, a network and voices/.
#
# BRAIN goes on after the sourcing rather than before it, because .env sets
# WATCH_BRAIN_MODEL and would otherwise win. That ordering is the whole point:
# comparing two brains has to be one command, or it is not a comparison
# anybody makes twice.
bench: build
	set -a; if [ -f .env ]; then . ./.env; fi; set +a; \
	$(if $(BRAIN),WATCH_BRAIN_MODEL="$(BRAIN)") ./build/kai_bench $(RUNS)

# CMake owns the dependency tracking. Re-running the configure step costs
# nothing and is what makes a fresh clone build in one command.
build:
	cmake -S . -B build
	cmake --build build -j

# Default. Every gate, in this order, against the working tree. Run before
# every commit. The lv_conf.h grep is first because it is instant and because
# a dead lv_conf.h still builds, it just builds the wrong thing. The four
# lines after the build are the host/device boundary check, once per portable
# directory: ui.c compiled alone must leave nothing but lv_* and libc
# undefined, and turn.c nothing but libc.
check:
	test "$$(grep -c '^#if 1 /\* Set this' lv_conf.h)" = 1
	cmake -S . -B build
	cmake --build build -j
	clang -std=c11 -Wall -Wextra -Werror -DLV_CONF_INCLUDE_SIMPLE -I. -c ui/ui.c -o build/ui-portable.o
	! nm -u build/ui-portable.o | grep -i 'sdl\|esp_'
	clang -std=c11 -Wall -Wextra -Werror -Ivoice -c voice/turn.c -o build/turn-portable.o
	! nm -u build/turn-portable.o | grep -i 'sdl\|esp_'
	./build/kai_test

# The same gates against the commit: a file never added, such as the generated
# font, cannot make it green. Run before every push. lvgl/ is gitignored, so
# the copy borrows this checkout's. cmake --version runs first, inside the
# copy, so the run records which toolchain ran. The archive goes through a
# file so git's exit status stops the run; one shell line so the trap cleans
# up however check ends.
ci:
	t=$$(mktemp); d=$$(mktemp -d); trap 'rm -rf "$$t" "$$d"' EXIT; git archive -o "$$t" HEAD && tar -xf "$$t" -C "$$d" && ln -s "$(CURDIR)/lvgl" "$$d/lvgl" && cmake --version && $(MAKE) -C "$$d" check

clean:
	rm -rf build/

# The firmware, for the board itself. Needs ESP-IDF exported into the shell
# first (. ~/esp/esp-idf/export.sh). Deliberately not part of check: that has
# to stay runnable on a Mac with nothing on it but Homebrew.
firmware:
	idf.py -C firmware build

# The firmware onto the board, then its log. Ctrl-] leaves the monitor.
flash:
	idf.py -C firmware flash monitor

# Loads .env when it is there, so a local start is one command — the address
# of the speech server, its key, and which models answer. Only run: check and
# test MUST NOT depend on a developer's machine, which is the baseline's rule
# 6, and neither of them speaks to a server anyway. One shell line, because
# each recipe line gets its own shell.
#
# Close the window to exit: LV_SDL_DIRECT_EXIT is 1.
run: build
	set -a; if [ -f .env ]; then . ./.env; fi; set +a; ./$(BIN)

# The inner loop: the last gate of check, without the rest of them.
test: build
	./build/kai_test
