# mtk - the Motif toolkit of mwc's clients. Its own build: the tests and a demo.
# A client takes mtk.mk; see README.md.

MTK_DIR = .
all:
include mtk.mk

CFLAGS ?= -O2 -g

all: build/mtkdemo

build:
	mkdir -p build

# A window with what mtk does, for tests/ui.sh and to look at.
build/mtkdemo: demo/mtkdemo.c $(MTK_DEPS) | build
	$(CC) $(MTK_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ demo/mtkdemo.c \
		$(MTK_SRC) $(MTK_LIBS) $(LDLIBS)

# make unit: tests/unit, a program per module (NAME_test.c with NAME.c), always
# under ASan and UBSan.
UNIT = color rc draw input field
UNIT_FLAGS = -std=c11 -Wall -Wextra -Wno-unused-parameter -Werror -O1 -g \
	-fno-omit-frame-pointer -fsanitize=address,undefined,float-cast-overflow \
	-fno-sanitize-recover=undefined,float-cast-overflow
UNIT_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(MTK_HEADLESS_PKGS))
UNIT_LIBS = $(shell $(PKG_CONFIG) --libs $(MTK_HEADLESS_PKGS)) -lm
# What a module needs of another: also linked, real.
UNIT_WITH_rc = color.c
UNIT_WITH_draw = color.c

build/unit: | build
	mkdir -p $@

build/unit/%: tests/unit/%_test.c %.c mtk.h tests/unit/unit.h | build/unit
	$(CC) $(UNIT_FLAGS) $(UNIT_CFLAGS) -o $@ $< $*.c $(UNIT_WITH_$*) $(UNIT_LIBS)

unit: $(UNIT:%=build/unit/%)
	@fail=0; for t in $^; do \
		LSAN_OPTIONS=suppressions=tests/unit/lsan.supp:print_suppressions=0 \
			$$t || fail=1; \
	done; exit $$fail

# make check: the units, then the demo under ASan and UBSan with -Werror, so that
# wl.c is built as strictly.
build/mtkdemo-asan: demo/mtkdemo.c $(MTK_DEPS) | build
	$(CC) $(MTK_CFLAGS) $(UNIT_FLAGS) -o $@ demo/mtkdemo.c $(MTK_SRC) $(MTK_LIBS)

check: unit build/mtkdemo-asan

# make test: the demo in a headless mwc from MWC_DIR (default ../mwc), checked in
# screenshots, under ASan and UBSan.
test: build/mtkdemo-asan build/mtkdemo build/failpoll.so
	MTKDEMO=build/mtkdemo-asan tests/ui.sh

# For tests/ui.sh: a poll that fails. Preloaded into build/mtkdemo; ASan's runtime
# must come first and would refuse it.
build/failpoll.so: tests/failpoll.c | build
	$(CC) -std=c11 -Wall -Wextra -Werror -shared -fPIC -O2 -o $@ $< -ldl

# make analyze: GCC's static analyzer over every source file, findings as errors.
analyze: $(MTK_PROTO_H)
	@for f in color.c rc.c draw.c input.c field.c wl.c demo/mtkdemo.c tests/failpoll.c; do \
		echo "analyze $$f"; \
		$(CC) $(MTK_CFLAGS) -O0 -fanalyzer -Werror -c $$f -o /dev/null || exit 1; \
	done

# make lint: C lines longer than LINT_MAX columns, tabs to every eighth column.
LINT_MAX = 100
LINT_FILES = *.c *.h tests/*.c tests/unit/*.c tests/unit/*.h demo/*.c
lint:
	@awk -v max=$(LINT_MAX) '{ \
		n = 0; \
		for (i = 1; i <= length($$0); i++) \
			n = substr($$0, i, 1) == "\t" ? n + 8 - n % 8 : n + 1; \
		if (n > max) { print FILENAME ":" FNR ": " n " columns"; bad = 1 } \
	} END { exit bad }' $(LINT_FILES)

clean:
	rm -rf build

.PHONY: all unit check test analyze lint clean
