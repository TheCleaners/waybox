#!/bin/sh
# meson dist: what the release tarball leaves out (git keeps them): the
# README's screenshots, the documentation's gallery, and CI files.
set -eu
cd "$MESON_DIST_ROOT"
rm -rf screenshots doc/gallery .github .build.yml
