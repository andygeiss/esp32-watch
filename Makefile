# Copied from the baseline (stack/makefile.md) with CMake in place of the Go
# toolchain. The dropped targets are recorded in CLAUDE.md.

# The simulator binary CMake writes.
BIN = build/kai_sim

# Targets are alphabetical, so the default is named rather than first.
.DEFAULT_GOAL = check
.PHONY: build check ci clean run test

# CMake owns the dependency tracking. Re-running the configure step costs
# nothing and is what makes a fresh clone build in one command.
build:
	cmake -S . -B build
	cmake --build build -j

# Default. Every gate, in this order, against the working tree. Run before
# every commit. The lv_conf.h grep is first because it is instant and because
# a dead lv_conf.h still builds, it just builds the wrong thing. The last two
# lines are the host/device boundary check: ui.c compiled alone must leave
# nothing but lv_* and libc undefined.
check:
	test "$$(grep -c '^#if 1 /\* Set this' lv_conf.h)" = 1
	cmake -S . -B build
	cmake --build build -j
	clang -std=c11 -Wall -Wextra -Werror -DLV_CONF_INCLUDE_SIMPLE -I. -c ui.c -o build/ui-portable.o
	! nm -u build/ui-portable.o | grep -i 'sdl\|esp_'
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

# Close the window to exit: LV_SDL_DIRECT_EXIT is 1.
run: build
	./$(BIN)

# The inner loop: the last gate of check, without the rest of them.
test: build
	./build/kai_test
