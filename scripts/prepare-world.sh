#!/usr/bin/env bash
# Prepares a Gaussian splat scene for SplatKit. The engine reads only .spz and .lodsplat, so a
# PLY is converted here, on a computer, never on the phone. The splat-core tools it runs are
# built into build/world-tools on the first call and kept up to date on later ones.
#
#   scripts/prepare-world.sh scene.ply out/              out/scene.spz
#   scripts/prepare-world.sh scene.ply out/ --collider   + out/scene.collider.glb, to walk
#   scripts/prepare-world.sh scene.ply out/ --lod        + out/scene.lodsplat, for huge scenes
#   scripts/prepare-world.sh scene.spz out/ --collider   an SPZ skips the conversion
#
# --sh N       keeps spherical harmonics up to degree N (0 to 3); the file's degree by default
# --collider   builds a walk collider; it assumes a space to walk through, not a lone object
# --lod        builds the level-of-detail tree offline instead of on the phone at load time
#
# Each tool takes more options than these; packages/splat-core/README.md lists them.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build/world-tools"
readonly minimum_cmake="3.22"
readonly lod_depth=10

usage() {
  sed -n '2,/^set /s/^# \{0,1\}//p' "$0"
  exit "${1:-2}"
}

fail() {
  echo "prepare-world: $*" >&2
  exit 1
}

input=""
out=""
sh=""
collider=false
lod=false
while [ $# -gt 0 ]; do
  case "$1" in
    -h | --help) usage 0 ;;
    --sh)
      [ $# -ge 2 ] || usage
      sh="$2"
      shift
      ;;
    --collider) collider=true ;;
    --lod) lod=true ;;
    -*) usage ;;
    *)
      if [ -z "$input" ]; then
        input="$1"
      elif [ -z "$out" ]; then
        out="$1"
      else
        usage
      fi
      ;;
  esac
  shift
done
[ -n "$input" ] && [ -n "$out" ] || usage
case "$sh" in "" | [0-3]) ;; *) fail "--sh takes 0, 1, 2 or 3, not $sh" ;; esac
[ -f "$input" ] || fail "no such file: $input"

name="$(basename "$input")"
extension="$(printf '%s' "${name##*.}" | tr '[:upper:]' '[:lower:]')"
name="${name%.*}"
case "$extension" in
  ply | spz) ;;
  *) fail "$input is not a .ply or .spz file" ;;
esac

command -v cmake > /dev/null || fail "CMake $minimum_cmake or newer is required (brew install cmake)"
cmake_version="$(cmake --version | sed -n 's/^cmake version \([0-9.]*\).*/\1/p')"
if [ "$(printf '%s\n%s\n' "$minimum_cmake" "$cmake_version" | sort -V | head -1)" != "$minimum_cmake" ]; then
  fail "CMake $minimum_cmake or newer is required, found $cmake_version"
fi

if [ ! -f "$build/CMakeCache.txt" ]; then
  echo "Building the conversion tools into $build (first run only)"
  cmake -S "$root/packages/splat-core" -B "$build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSPLAT_CORE_BUILD_TESTS=OFF \
    -DSPLAT_CORE_BUILD_TOOLS=ON \
    > /dev/null
fi
cmake --build "$build" --parallel --target ply2spz splat_collider splat_lod_build > /dev/null
tools="$build/tools"

mkdir -p "$out"
out="$(cd "$out" && pwd)"
spz="$out/$name.spz"

if [ "$extension" = ply ]; then
  echo "Converting $input to SPZ"
  "$tools/ply2spz" "$input" "$spz" ${sh:+--sh "$sh"}
elif [ "$(cd "$(dirname "$input")" && pwd)/$(basename "$input")" != "$spz" ]; then
  cp "$input" "$spz"
fi
world="$spz"

if [ "$lod" = true ]; then
  echo "Building the level-of-detail tree"
  world="$out/$name.lodsplat"
  rm -f "$world"  # splat_lod_build refuses to overwrite
  "$tools/splat_lod_build" "$spz" "$world" --depth "$lod_depth" ${sh:+--sh "$sh"}
fi

if [ "$collider" = true ]; then
  echo "Building the walk collider"
  "$tools/splat_collider" "$spz" "$out/$name.collider.glb"
fi

echo
echo "Ready. Copy these to the device and pass their absolute paths:"
echo "  world.filePath     $world"
if [ "$collider" = true ]; then
  echo "  collider.filePath  $out/$name.collider.glb"
fi
