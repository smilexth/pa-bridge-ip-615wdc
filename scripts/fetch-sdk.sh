#!/bin/sh
# Fetch BUENA's IP- series SDK and put the x86-64 build where the Makefile expects it.
#
# The SDK is the vendor's proprietary property and is NOT redistributed in this
# repository. This script only downloads it from their own public download page,
# verifies it, and unpacks the one build that links on x86-64.
#
#   ./scripts/fetch-sdk.sh                     # download from the vendor
#   SDK_ZIP=/path/to/ip-series-sdk.zip ./scripts/fetch-sdk.sh   # use a local copy
set -eu

URL='https://www.buenapa.com/static/upload/other/20250818/1755512660485720.zip'
SHA='b8598ca1e4304d30b0764bde1f34b3118f14f5939a99692a6e63abae20adf5a3'
BUILD='SDK__Linux__X64__V308__New'
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DEST="$ROOT/vendor/sdk"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

if [ -f "$DEST/Include/libCtsSdk.a" ]; then
    echo "SDK already present at $DEST/Include — nothing to do."
    exit 0
fi

ZIP="${SDK_ZIP:-}"
if [ -n "$ZIP" ]; then
    echo "Using local copy: $ZIP"
else
    ZIP="$TMP/sdk.zip"
    echo "Downloading the vendor SDK (about 11 MB)..."
    echo "  $URL"
    curl -fL --retry 3 -o "$ZIP" "$URL"
fi

echo "Verifying checksum..."
if command -v sha256sum >/dev/null 2>&1; then
    got=$(sha256sum "$ZIP" | cut -d' ' -f1)
else
    got=$(shasum -a 256 "$ZIP" | cut -d' ' -f1)
fi
if [ "$got" != "$SHA" ]; then
    echo "  checksum mismatch!" >&2
    echo "    expected $SHA" >&2
    echo "    got      $got" >&2
    echo "  The vendor may have replaced the package. Compare it by hand before using it," >&2
    echo "  and update SHA in this script once you are satisfied." >&2
    exit 1
fi

echo "Unpacking $BUILD..."
# Extract ONLY the build we need. The package also carries Windows SDKs whose
# filenames are GBK-encoded: unpacking the whole archive makes macOS unzip fail
# on those names and then sit at an interactive "Continue? (y/n)" prompt.
unzip -q -o -j "$ZIP" "SDK/SDK Linux/$BUILD.tar.xz" -d "$TMP" < /dev/null
tar xJf "$TMP/$BUILD.tar.xz" -C "$TMP"
mkdir -p "$DEST"
cp -R "$TMP/$BUILD/Include" "$DEST/"

echo
echo "Done. $DEST/Include now holds:"
ls -1 "$DEST/Include"
echo
echo "Build with:  docker build -t pa-bridge-build . && docker run --rm -v \"\$PWD:/src\" -w /src/src pa-bridge-build make"
