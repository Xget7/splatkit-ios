#!/usr/bin/env bash
# Build a self-contained Objective-C++ XCFramework; Swift sources remain open.
set -euo pipefail
splat_root="$(cd "$(dirname "$0")/.." && pwd)"
splat_jobs="${SPLATKIT_BUILD_JOBS:-2}"
mkdir -p "$splat_root/build/ios-distribution"
splat_output="$(mktemp -d "$splat_root/build/ios-distribution/package.XXXXXX")"

for splat_sdk in iphoneos iphonesimulator; do
  splat_archs=arm64
  if [[ "$splat_sdk" == iphonesimulator ]]; then splat_archs='arm64;x86_64'; fi
  splat_build="$splat_root/build/ios-distribution/$splat_sdk"
  cmake -S "$splat_root/packages/splatkit-ios" -B "$splat_build" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT="$splat_sdk" -DCMAKE_OSX_ARCHITECTURES="$splat_archs" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
    -DCMAKE_C_COMPILER_WORKS=ON -DCMAKE_CXX_COMPILER_WORKS=ON
  cmake --build "$splat_build" --parallel "$splat_jobs"
  mkdir -p "$splat_output/$splat_sdk"
  xcrun libtool -static -o "$splat_output/$splat_sdk/libSplatKitCore.a" \
    "$splat_build/libsplatkit_ios.a" \
    "$splat_build/splatkit-engine/libsplatkit_engine.a" \
    "$splat_build/splatkit-engine/splat-core/libsplat_core.a" \
    "$splat_build/_deps/spz-build/libspz.a" \
    "$splat_build/_deps/zstd-build/lib/libzstd.a"
done

splat_headers="$splat_root/packages/splatkit-ios/Sources/SplatKitCore/include"
xcodebuild -create-xcframework \
  -library "$splat_output/iphoneos/libSplatKitCore.a" -headers "$splat_headers" \
  -library "$splat_output/iphonesimulator/libSplatKitCore.a" -headers "$splat_headers" \
  -output "$splat_output/SplatKitCore.xcframework"

# Redistributed dependency licenses accompany the archive, not just the source repo.
mkdir -p "$splat_output/SplatKitCore.xcframework/Notices"
cp "$splat_root/LICENSE" "$splat_output/SplatKitCore.xcframework/Notices/SplatKit.txt"
cp "$splat_build/_deps/spz-src/LICENSE" "$splat_output/SplatKitCore.xcframework/Notices/SPZ.txt"
cp "$splat_build/_deps/nlohmann_json-src/LICENSE.MIT" "$splat_output/SplatKitCore.xcframework/Notices/JSON.txt"
cp "$splat_build/_deps/zstd-src/LICENSE" "$splat_output/SplatKitCore.xcframework/Notices/Zstandard.txt"
ditto -c -k --keepParent "$splat_output/SplatKitCore.xcframework" "$splat_output/SplatKitCore.xcframework.zip"
swift package compute-checksum "$splat_output/SplatKitCore.xcframework.zip"
printf 'Artifact: %s\n' "$splat_output/SplatKitCore.xcframework.zip"
