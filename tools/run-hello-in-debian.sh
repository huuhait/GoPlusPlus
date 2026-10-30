#!/usr/bin/env bash
# P04.02 Done-when, M0 on an FHS distro: build hello with gopp inside a Debian
# container, then run the program in a second, clean container.
#
#   tools/run-hello-in-debian.sh [gopp] [build-image] [run-image]
#
# gopp defaults to build/bin/gopp; build-image to debian:stable-slim; the
# run image to the build image. A gopp built on NixOS loads its libraries
# from the Nix store, so /nix is mounted read-only. Host sysroot discovery
# (P00.12) asks the system C compiler where crt1.o is, so the build container
# needs gcc and libc6-dev: they are installed with apt (network) unless the
# image has a `cc` already (golang:1.25 has). The run container has neither:
# the program needs only the C library and the loader.
#
# Needs docker or podman. Not part of `ninja check`: lit runs it when
# GOPP_TEST_DEBIAN_IMAGE names the image (tests/run/debian-container.long.test).
set -euo pipefail

gopp=$(realpath "${1:-build/bin/gopp}")
build_image=${2:-debian:stable-slim}
run_image=${3:-$build_image}
engine=$(command -v docker || command -v podman || true)
[ -n "$engine" ] || { echo "error: needs docker or podman" >&2; exit 2; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
chmod 0777 "$work"
cat > "$work/hello.gopp" <<'GOPP'
package main

func main() {
	println("hello, world", 42, true)
}
GOPP

mounts=(-v "$(dirname "$gopp"):/gopp:ro" -v "$work:/w")
if [ -d /nix ]; then
  mounts+=(-v /nix:/nix:ro)
fi
"$engine" run --rm "${mounts[@]}" "$build_image" sh -ec '
  if ! command -v cc >/dev/null; then
    apt-get update -qq >/dev/null
    apt-get install -y -qq gcc libc6-dev >/dev/null
  fi
  /gopp/'"$(basename "$gopp")"' host-toolchain
  /gopp/'"$(basename "$gopp")"' build -o /w/hello /w/hello.gopp
  chmod 0755 /w/hello'
"$engine" run --rm -v "$work:/w:ro" "$run_image" /w/hello
echo "ok: hello ran on $run_image"
