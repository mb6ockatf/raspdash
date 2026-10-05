# Task: build raspdash's libraries on the Pi with GCC 16

Build fresh, current versions of every library raspdash uses, natively on
the Raspberry Pi, with the self-built GCC 16, into one private prefix, and
make raspdash's build use them instead of Raspberry Pi OS Bullseye's old
packages.

## Context

- Host: `ssh pi` (192.168.1.96), Raspberry Pi 4 Model B, 4 cores, 3.6 GiB
  RAM, 99 MiB swap. Raspbian 11 Bullseye, **32-bit armhf userland**
  (`arm-linux-gnueabihf`) on an aarch64 kernel, glibc 2.31.
- Compiler: GCC 16 built by the user. Before starting, ask the user for its
  install prefix if it isn't obvious (`ls /opt`, `which -a gcc-16 g++-16`).
  Below it is `$GCC16`, e.g. `/opt/gcc-16`.
- The system's `-dev` packages (libncurses-dev, libcurl4-openssl-dev,
  libssl-dev, nlohmann-json3-dev) stay installed and untouched. The new
  libraries go into their own prefix and never into `/usr` or `/usr/local`.
- Passwordless `sudo` works on the Pi, but nothing here should need it
  except creating the prefix directory.
- Memory is tight: use `make -j4` for C code. If anything is killed for
  lack of memory, enlarge swap first (`/etc/dphys-swapfile`,
  `CONF_SWAPSIZE=2048`, then `sudo dphys-swapfile setup && sudo
  dphys-swapfile swapon`); ask the user before changing swap.

## Libraries and versions

Latest stable releases as of 2026-10-05. Check for newer point releases
before downloading, and verify every tarball (signature or published
SHA-256) before building.

| Library | Version | Source | Why raspdash needs it |
|---|---|---|---|
| zlib | 1.3.2 | https://zlib.net/ | gzip/deflate for libcurl |
| OpenSSL | 4.0.3 | https://github.com/openssl/openssl/releases | TLS for libcurl; MD5/SHA-256 (`EVP_Digest*`) for the Keenetic login and Google OAuth PKCE |
| curl (libcurl) | 8.22.0 | https://curl.se/download/ | all HTTP(S) requests |
| ncurses | 6.6 | https://ftp.gnu.org/gnu/ncurses/ | terminal UI, wide-character build (`ncursesw`) |
| nlohmann/json | 3.12.0 | https://github.com/nlohmann/json/releases | JSON parsing (header-only) |
| toml++ | 3.4.0 | https://github.com/marzer/tomlplusplus/releases | already bundled in `third_party/toml.hpp`; only update it if a newer release exists |

If OpenSSL 4.0 turns out to be incompatible with curl 8.22 (curl's
`configure` or test suite fails on it), fall back to the OpenSSL 3.5 LTS
series and record that in the report.

## Common settings

```sh
export GCC16=/opt/gcc-16                 # ask the user
export PREFIX=/opt/raspdash-deps
export CC=$GCC16/bin/gcc CXX=$GCC16/bin/g++
# Pi 4 only: Cortex-A72, NEON, hard float (the 32-bit userland's ABI).
export CFLAGS="-O2 -pipe -mcpu=cortex-a72 -mfpu=neon-fp-armv8 -mfloat-abi=hard"
export CXXFLAGS="$CFLAGS"
export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig:/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig
sudo install -d -o "$USER" -g "$USER" "$PREFIX"
mkdir -p ~/src/raspdash-deps && cd ~/src/raspdash-deps
```

Build **static libraries only** (`.a`, position-independent). raspdash is
then a single binary that depends only on glibc and the system's
terminfo database, so nothing breaks when Bullseye's own `libssl` or
`libcurl` are upgraded, and no `LD_LIBRARY_PATH` or rpath is needed.

GCC 16 compiles C as C23 by default. If an older code base fails on that
(`bool`/`true` as keywords, old-style prototypes), add `-std=gnu17` to
that library's `CFLAGS` only, and note it in the report.

Build in this order, since each step depends on the previous ones.

### 1. zlib

```sh
./configure --prefix=$PREFIX --static
make -j4 && make test && make install
```

### 2. OpenSSL

```sh
./Configure linux-armv4 --prefix=$PREFIX --libdir=lib \
    --openssldir=/usr/lib/ssl \
    no-shared no-tests no-docs zlib --with-zlib-include=$PREFIX/include \
    --with-zlib-lib=$PREFIX/lib -fPIC
make -j4 && make install_sw
```

`--openssldir=/usr/lib/ssl` reuses the system's `openssl.cnf` and CA
certificates (`/usr/lib/ssl/certs` → `/etc/ssl/certs`), so the system's
`ca-certificates` package keeps the trust store up to date.  Building
with `no-tests` keeps build time reasonable; afterwards run a quick check
with `$PREFIX/bin/openssl version -a` and
`$PREFIX/bin/openssl s_client -connect ruz.spbstu.ru:443 </dev/null`
(it must say `Verify return code: 0 (ok)`).

### 3. curl

Build only what raspdash uses: HTTP and HTTPS over OpenSSL with zlib.

```sh
./configure --prefix=$PREFIX --disable-shared --enable-static --with-pic \
    --with-openssl=$PREFIX --with-zlib=$PREFIX \
    --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt \
    --with-ca-path=/etc/ssl/certs \
    --disable-ldap --disable-ldaps --disable-rtsp --disable-dict \
    --disable-telnet --disable-tftp --disable-pop3 --disable-imap \
    --disable-smtp --disable-gopher --disable-mqtt --disable-smb \
    --without-libpsl --without-brotli --without-zstd --without-nghttp2 \
    --without-libidn2 --without-librtmp --without-libssh2 \
    --disable-manual --disable-docs
make -j4 && make install
```

Check: `$PREFIX/bin/curl --version` lists OpenSSL 4.0.3 and zlib 1.3.2,
and `$PREFIX/bin/curl -sI https://api.met.no/` gets an HTTP response
(no certificate error).  `pkg-config --static --libs libcurl` must list
`-lssl -lcrypto -lz`.

### 4. ncurses

Wide-character only, static, using the **system's** terminfo database so
`TERM=xterm-256color` and `alacritty` resolve as before.

```sh
./configure --prefix=$PREFIX --enable-widec \
    --without-shared --with-normal --without-debug --without-ada \
    --without-cxx-binding --without-manpages --without-tests \
    --enable-pc-files --with-pkg-config-libdir=$PREFIX/lib/pkgconfig \
    --with-default-terminfo-dir=/usr/share/terminfo \
    --with-terminfo-dirs=/etc/terminfo:/lib/terminfo:/usr/share/terminfo \
    --disable-db-install --without-progs
make -j4 && make install
```

Check that `$PREFIX/lib/pkgconfig/ncursesw.pc` exists and that
`pkg-config --cflags ncursesw` points into `$PREFIX/include`.

### 5. nlohmann/json

Header-only. Install the single header and a pkg-config file; no CMake
needed.

```sh
install -D -m 644 single_include/nlohmann/json.hpp \
    $PREFIX/include/nlohmann/json.hpp
install -D -m 644 single_include/nlohmann/json_fwd.hpp \
    $PREFIX/include/nlohmann/json_fwd.hpp
cat >$PREFIX/lib/pkgconfig/nlohmann_json.pc <<EOF
prefix=$PREFIX
includedir=\${prefix}/include
Name: nlohmann_json
Description: JSON for Modern C++
Version: 3.12.0
Cflags: -I\${includedir}
EOF
```

### 6. toml++

Only if a release newer than 3.4.0 exists: replace `third_party/toml.hpp`
and `third_party/toml++-LICENSE` in the raspdash tree (on the laptop, as a
normal commit), and mention it in `THANKS` if the license changed.

## Changes to raspdash's build

Do these in the raspdash source tree on the laptop, keeping GNU style and
the gnits rules. Then sync the tree to the Pi.

1. **Static dependencies.** Add `--enable-static-deps` to `configure.ac`.
   It makes the `PKG_CHECK_MODULES` calls use `pkg-config --static`, so
   static libcurl brings in `-lssl -lcrypto -lz -ldl -lpthread`. Keep the
   default (shared, system libraries) working for the laptop.
2. **nlohmann/json via pkg-config.** Prefer `PKG_CHECK_MODULES([JSON],
   [nlohmann_json >= 3.11])` with the current header check as the
   fallback, and add `$(JSON_CFLAGS)` to `AM_CPPFLAGS`. Bullseye's 3.9.1
   must no longer be picked up when the private prefix is present.
3. **install-pi.sh.** When `/opt/raspdash-deps` (or `$DEPS_PREFIX`)
   exists, export `PKG_CONFIG_PATH`/`PKG_CONFIG_LIBDIR` as above and pass
   `--enable-static-deps` along with `--enable-static-libstdcxx`.
4. **Manual.** Document the option and this procedure in
   `doc/raspdash.texi` (node "Building", and a new section under
   "Installing on the Pi"). Update `NEWS` and `ChangeLog`.

## Done when

- `$PREFIX` holds the static `libz.a`, `libssl.a`, `libcrypto.a`,
  `libcurl.a` and `libncursesw.a` plus headers and `.pc` files. No `.so`
  files are in `$PREFIX/lib`.
- On the Pi, `deploy/install-pi.sh` with GCC 16 configures with the new
  options, builds without warnings, and passes `make check` and
  `make distcheck`.
- `ldd ~/.local/bin/raspdash` lists only glibc libraries (`libc`, `libm`,
  `libpthread`/`libdl` if separate, `ld-linux-armhf`), no `libcurl`,
  `libssl`, `libncursesw` or `libstdc++`.
- `raspdash --version` runs and `raspdash --check` reports `ok` for the
  timetable, weather and markets sources over HTTPS.
- The kiosk starts at login and shows Cyrillic, box-drawing, Braille and
  Nerd Font icons correctly (take a `scrot` screenshot as evidence).
- A short report lists the exact versions built, any deviations (C23
  workarounds, OpenSSL fallback), and the build times per library.

## Don'ts

- Don't install anything into `/usr` or `/usr/local`, and don't remove or
  upgrade the system's packages.
- Don't touch GCC 16's installation.
- Don't stop or reconfigure networking on the Pi.
- Don't commit build products, tarballs or anything from `$PREFIX`.
