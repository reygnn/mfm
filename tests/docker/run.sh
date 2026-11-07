#!/bin/sh
# Build mfm and mwc in a container and run the tests there, for hosts without grim
# or mwc's build dependencies, or that restrict user namespaces (Ubuntu's AppArmor
# does). Both sources are mounted read-only and built from copies. MWC_DIR is mwc's
# source tree (default ../mwc). Takes podman or docker; the image is mfm-test.
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
mwc=$(cd "${MWC_DIR:-$repo/../mwc}" && pwd)
[ -f "$mwc/tests/vinput.c" ] || { echo "FAIL: no mwc sources in $mwc (MWC_DIR)"; exit 1; }
engine=$(command -v podman || command -v docker) ||
	{ echo "FAIL: neither podman nor docker found"; exit 1; }
"$engine" build -q -t mfm-test "$here" >/dev/null
# The nested user namespaces need the default seccomp and AppArmor profiles out of
# the way.
exec "$engine" run --rm --network=none --security-opt seccomp=unconfined \
	--security-opt apparmor=unconfined -v "$repo":/src/mfm:ro -v "$mwc":/src/mwc:ro \
	mfm-test sh -ec '
		mkdir -p ~/w/mfm ~/w/mwc
		cp -r /src/mfm/. ~/w/mfm/
		cp -r /src/mwc/. ~/w/mwc/
		cd ~/w/mwc
		rm -rf build mwc .git
		make -s -j"$(nproc)" mwc build/vinput build/pngtool >/dev/null
		cd ~/w/mfm
		rm -rf .git
		make -s clean
		make -s
		MWC_DIR=~/w/mwc make -s test'
