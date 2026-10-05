#!/bin/sh
# install-pi.sh -- build raspdash on the Pi and set it up to start at login.
#
# Copyright (C) 2026 mb6ockatf
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Run from the source tree on the Pi:
#
#   CXX=/opt/gcc-16.2.0/bin/g++-16 deploy/install-pi.sh
#
# Installs into ~/.local (no root needed), links libstdc++ statically so a
# compiler newer than the system's works, and adds an XDG autostart entry.
# The configuration stays in the source tree (config.toml, secrets/).
# When the private libraries of deploy/TASK-build-deps.md exist in
# /opt/raspdash-deps (or $DEPS_PREFIX), they are linked statically.

set -eu

src=$(cd "$(dirname "$0")/.." && pwd)
prefix=${PREFIX:-$HOME/.local}
build=${BUILD_DIR:-$src/_build}
jobs=$(nproc 2>/dev/null || echo 4)

deps=${DEPS_PREFIX:-/opt/raspdash-deps}

# Always regenerate: a tree copied from another machine carries files
# made by a different Automake version, which would break the first time
# make decided to rebuild them.
(cd "$src" && ./bootstrap)

static_deps=
if [ -d "$deps/lib/pkgconfig" ]; then
    export PKG_CONFIG_PATH=$deps/lib/pkgconfig
    export PKG_CONFIG_LIBDIR=$deps/lib/pkgconfig:/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig
    static_deps=--enable-static-deps
    echo "Using the libraries in $deps"
fi

mkdir -p "$build"
cd "$build"
"$src/configure" --prefix="$prefix" --enable-static-libstdcxx $static_deps \
    ${CXX:+CXX="$CXX"}
make -j"$jobs"
make -j"$jobs" check
make install

install -m 755 "$src/deploy/raspdash-kiosk" "$prefix/bin/raspdash-kiosk"
install -D -m 644 "$src/deploy/alacritty-kiosk.yml" \
    "$prefix/share/raspdash/alacritty-kiosk.yml"

autostart=${XDG_CONFIG_HOME:-$HOME/.config}/autostart
mkdir -p "$autostart"
sed "s|\$HOME/.local|$prefix|" "$src/deploy/raspdash.desktop" \
    >"$autostart/raspdash.desktop"

if [ ! -e "$src/config.toml" ]; then
    cp "$src/config.example.toml" "$src/config.toml"
    echo "Created $src/config.toml from config.example.toml; edit it."
fi

# Point the kiosk at the source tree's configuration.
mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/raspdash"
ln -sfn "$src/config.toml" "${XDG_CONFIG_HOME:-$HOME/.config}/raspdash/config.toml"

echo "Installed.  raspdash starts at the next login; to start it now:"
echo "  DISPLAY=:0 $prefix/bin/raspdash-kiosk &"
