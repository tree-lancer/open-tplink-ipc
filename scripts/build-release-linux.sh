#!/bin/sh
set -eu

# Build the LAN-only HTTP client without curl's optional protocol libraries.
# This keeps the release executable small and avoids runtime dependencies.
apk add --no-cache build-base pkgconf curl openssl-dev openssl-libs-static \
  libxml2-dev libxml2-static nlohmann-json zlib-static xz-static

curl_version=8.14.1
curl_sha256=6766ada7101d292b42b8b15681120acd68effa4a9660935853cf6d61f0d984d4
curl_archive="/tmp/curl-${curl_version}.tar.gz"
curl -fsSL --retry 3 "https://curl.se/download/curl-${curl_version}.tar.gz" -o "$curl_archive"
printf '%s  %s\n' "$curl_sha256" "$curl_archive" | sha256sum -c -
tar -xzf "$curl_archive" -C /tmp
curl_source="/tmp/curl-${curl_version}"
(
  cd "$curl_source"
  ./configure --disable-shared --enable-static --disable-docs --disable-manual \
    --without-ca-embed --without-ssl --without-zlib --without-libpsl \
    --without-libidn2 --without-brotli --without-zstd --without-nghttp2 \
    --without-nghttp3 --disable-ftp --disable-ldap --disable-ldaps \
    --disable-rtsp --disable-file >/tmp/tplink-curl-config.log
  make -j2 -C lib >/tmp/tplink-curl-build.log
)

version=0.2.0
case "${GITHUB_REF:-}" in
  refs/tags/v*) version="${GITHUB_REF_NAME#v}" ;;
esac

mkdir -p dist
c++ -std=c++17 -O2 -DNDEBUG -static -Wall -Wextra -Wpedantic \
  -DTPLINK_IPC_VERSION="\"$version\"" \
  -I"$curl_source/include" $(pkg-config --cflags libxml-2.0 openssl) \
  src/main.cpp -o dist/tplink-ipc-mcp-linux-x86_64 \
  "$curl_source/lib/.libs/libcurl.a" \
  $(pkg-config --static --libs libxml-2.0 openssl)
strip dist/tplink-ipc-mcp-linux-x86_64
if readelf -l dist/tplink-ipc-mcp-linux-x86_64 | grep -q INTERP; then
  echo 'Release binary is dynamically linked' >&2
  exit 1
fi
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
  | dist/tplink-ipc-mcp-linux-x86_64 \
  | grep -q '"name":"tplink-ipc-mcp"'
(cd dist && sha256sum tplink-ipc-mcp-linux-x86_64 > SHA256SUMS)
