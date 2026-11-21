#!/bin/bash
# mtk.mk as a client takes it: a client's Makefile as README.md shows it, and one
# that includes mtk.mk before its first target, builds the client with a plain
# make. And the units are rebuilt with what they link.
MTK_DIR=$(cd "$(dirname "$0")/.." && pwd)
MAKE=${MAKE:-make}
fail=0
check() {
	if eval "$2"; then echo "ok: $1"; else echo "FAIL: $1"; fail=1; fi
}

tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/app.c" <<'EOF'
#include "mtk.h"
int main(void)
{
	uint32_t c;
	return !(mtk_parse_color("gold", &c) && c == 0xffd700);
}
EOF
rule='app: app.c $(MTK_DEPS)
	$(CC) $(MTK_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ app.c \
		$(MTK_SRC) $(MTK_LIBS) $(LDLIBS)'
for first in "all: app" ""; do
	rm -rf "$tmp/build" "$tmp/app" "$tmp/mtk"
	ln -s "$MTK_DIR" "$tmp/mtk"
	printf '%s\nMTK_DIR = mtk\ninclude $(MTK_DIR)/mtk.mk\n\n%s\n' "$first" "$rule" \
		> "$tmp/Makefile"
	$MAKE -s -C "$tmp" > "$tmp/out" 2>&1
	check "make builds the client${first:+ after \"$first\"}" \
		'[ -x "$tmp/app" ] && "$tmp/app"'
	[ -x "$tmp/app" ] || cat "$tmp/out"
done

# A unit is rebuilt when a module it links changes, and not for one it doesn't.
cd "$MTK_DIR" || exit 1
check "build/unit/rc is rebuilt for color.c" \
	'$MAKE -n -W color.c build/unit/rc | grep -q rc_test.c'
check "build/unit/rc is not rebuilt for input.c" \
	'! $MAKE -n -W input.c build/unit/rc | grep -q rc_test.c'
check "build/unit/input is not rebuilt for color.c" \
	'! $MAKE -n -W color.c build/unit/input | grep -q input_test.c'
exit $fail
