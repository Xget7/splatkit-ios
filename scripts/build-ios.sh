#!/usr/bin/env bash
# Builds the iOS static libraries (splat-core, splatkit-engine, splatkit-ios and their
# fetched dependencies) for a device, into build/ios. The dev app and the Swift package
# link them from there.
#
#   scripts/build-ios.sh                         Release, arm64 device
#   scripts/build-ios.sh Debug
#   scripts/build-ios.sh Release iphonesimulator  arm64 simulator, into build/ios-simulator
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
config="${1:-Release}"
sdk="${2:-iphoneos}"
case "$sdk" in
  iphoneos) build="$root/build/ios" ;;
  iphonesimulator) build="$root/build/ios-simulator" ;;
  *) echo "unknown SDK $sdk; use iphoneos or iphonesimulator" >&2; exit 2 ;;
esac

cmake -S "$root/packages/splatkit-ios" -B "$build" -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE="$config" \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT="$sdk" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
  -DCMAKE_C_COMPILER_WORKS=ON -DCMAKE_CXX_COMPILER_WORKS=ON \
  > /dev/null
cmake --build "$build" --parallel

# One folder of libraries for the linker, whatever subdirectory CMake put them in.
mkdir -p "$build/lib"
find "$build" -name '*.a' -not -path "$build/lib/*" -exec cp {} "$build/lib/" \;
ls "$build/lib"
