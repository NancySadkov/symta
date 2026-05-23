#!/usr/bin/env bash
# build-dist.sh -- assemble a milestone release archive for Symta.
#
# Run from a WSL or Linux shell with access to:
#   - this repo (cwd or any subdir of ~/code/som/symta)
#   - the Windows-side build of symta.exe (assumed pre-built and
#     located in /mnt/c/...; configurable via SYMTA_EXE_PATH)
#
# Outputs:
#   ~/code/som/dist/symta-<STAMP>-linux.tar.gz
#   ~/code/som/dist/symta-<STAMP>-windows.zip
#   ~/code/som/dist/symta-latest-linux.tar.gz   (copy of dated)
#   ~/code/som/dist/symta-latest-windows.zip
#
# Usage:
#   ./symta/dev/build-dist.sh                 # uses today's date
#   ./symta/dev/build-dist.sh 2026-06-15      # explicit stamp
#   STRIP=1 ./symta/dev/build-dist.sh         # strip binaries (smaller)
#
# Optional env vars:
#   SYMTA_EXE_PATH   default: /mnt/c/Users/nangl/d/code/som/symta/symta.exe
#   DIST_OUT_DIR     default: ~/code/som/dist
#   STAGING_DIR      default: /tmp/symta-dist-<STAMP>
#   STRIP            default: 0; if "1", run strip(1) on both binaries
#                    after staging (saves ~30-40% on archive size)
#
# After this script runs, push to the live site:
#   rsync -av symta-site/downloads/ nancy@<vps>:~/aermia.com/symta-site/downloads/
# ...where symta-site/downloads/ lives in the aermia.com repo and is
# populated by the snippet at the bottom of this script (-p / --publish
# does it automatically).

set -euo pipefail

# --- args + defaults -------------------------------------------------------

STAMP="${1:-$(date +%F)}"
SYMTA_EXE_PATH="${SYMTA_EXE_PATH:-/mnt/c/Users/nangl/d/code/som/symta/symta.exe}"
DIST_OUT_DIR="${DIST_OUT_DIR:-$HOME/code/som/dist}"
STAGING_DIR="${STAGING_DIR:-/tmp/symta-dist-$STAMP}"
STRIP="${STRIP:-0}"
PUBLISH="${PUBLISH:-0}"   # if "1", also rsync to the VPS at the end

# Allow `-p` / `--publish` as a friendlier toggle for the rsync step.
for arg in "$@"; do
  case "$arg" in
    -p|--publish) PUBLISH=1 ;;
  esac
done

# --- locate repo root ------------------------------------------------------

# Resolve this script's location so it works whether you invoke it from
# the repo root, the symta/ subdir, or anywhere else.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SYMTA_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SOM_ROOT="$(cd "$SYMTA_DIR/.." && pwd)"

cd "$SYMTA_DIR"

COMMIT="$(git rev-parse --short HEAD)"

echo "=== symta release build ==="
echo "  stamp:   $STAMP"
echo "  commit:  $COMMIT"
echo "  symta/:  $SYMTA_DIR"
echo "  out:     $DIST_OUT_DIR"
echo "  strip:   $STRIP"
echo "  publish: $PUBLISH"
echo

# --- step 1: rebuild Linux binary from current HEAD ----------------------

echo "=== build Linux binary ==="
rm -rf build/rt
make -f Makefile.linux 2>&1 | tail -3
test -x ./symta || { echo "ERROR: Linux build did not produce ./symta" >&2; exit 1; }
ls -lh ./symta | sed 's/^/  /'

# --- step 2: sanity-check Windows binary exists --------------------------

echo
echo "=== check Windows binary ==="
if [ ! -f "$SYMTA_EXE_PATH" ]; then
  echo "ERROR: $SYMTA_EXE_PATH not found." >&2
  echo "  Build it first by running build.bat from a Windows shell." >&2
  echo "  Or set SYMTA_EXE_PATH= to point at the location of symta.exe." >&2
  exit 1
fi
ls -lh "$SYMTA_EXE_PATH" | sed 's/^/  /'

# --- step 3: stage source trees (one per platform) -----------------------

echo
echo "=== stage source tree to $STAGING_DIR ==="
rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR"/{linux,windows}

# Files / directories to leave OUT of the distribution.
EXCLUDES=(
  --exclude=build/
  --exclude=tmp/
  --exclude='.DS_Store'
  --exclude=symta.exe
  --exclude=symta
  --exclude=go.exe
  --exclude=go
  --exclude='*.o'
  --exclude='snaps/'
  --exclude='examples/*/sbc/'
  --exclude='examples/*/lib/'
  --exclude='examples/*/cache/'
  --exclude='examples/*/go.exe'
  --exclude='tests/*/build/'
  --exclude='tests/*/sbc/'
  --exclude='tests/*/lib/'
  --exclude='tests/*/cache/'
  --exclude='tests/*/go.exe'
)
rsync -a "${EXCLUDES[@]}" "$SYMTA_DIR/" "$STAGING_DIR/linux/symta/"
rsync -a "${EXCLUDES[@]}" "$SYMTA_DIR/" "$STAGING_DIR/windows/symta/"

# --- step 4: drop platform binaries into the respective tree -------------

echo
echo "=== drop platform binaries ==="
cp "$SYMTA_DIR/symta"    "$STAGING_DIR/linux/symta/symta"
cp "$SYMTA_EXE_PATH"     "$STAGING_DIR/windows/symta/symta.exe"
chmod +x "$STAGING_DIR/linux/symta/symta"

if [ "$STRIP" = "1" ]; then
  strip "$STAGING_DIR/linux/symta/symta"
  command -v strip >/dev/null && strip "$STAGING_DIR/windows/symta/symta.exe" 2>/dev/null || \
    echo "  (could not strip symta.exe; that's fine, mingw symbols are small)"
  ls -lh "$STAGING_DIR/linux/symta/symta" "$STAGING_DIR/windows/symta/symta.exe" | sed 's/^/  /'
fi

# --- step 5: write per-archive READMEs -----------------------------------

echo
echo "=== write READMEs ==="

cat > "$STAGING_DIR/linux/README.md" <<README
# Symta -- Linux distribution

Milestone snapshot from commit ${COMMIT} (${STAMP}).

## Quick start

\`\`\`sh
cd symta/
./symta -f examples/00-hello.s
\`\`\`

Dynamic-glibc ELF, x86_64.  Runs on any modern Linux (Ubuntu
22.04+, Debian 12+, RHEL 9+, etc.) without extra dependencies.

To rebuild from source:

\`\`\`sh
cd symta/
make -f Makefile.linux clean && make -f Makefile.linux
\`\`\`

Requires gcc + make.  Optional SDL2 / SDL_mixer for the gfx
plugin, loaded lazily.

## Layout

  symta/symta           the runtime binary
  symta/src/            the compiler in Symta itself
  symta/sbc/            pre-compiled bytecode bootstrap
  symta/runtime/        C runtime + JIT + FFI
  symta/examples/       40+ runnable example programs
  symta/tests/          test suites for runtime / macros / compiler
  symta/Makefile.linux  build recipe

## License

Apache 2.0 / MIT dual.  See \`symta/LICENSE\`.

## Where to find the next release

https://symta.aermia.com
README

cat > "$STAGING_DIR/windows/README.md" <<README
# Symta -- Windows distribution

Milestone snapshot from commit ${COMMIT} (${STAMP}).

## Quick start

\`\`\`cmd
cd symta
symta.exe -f examples\\00-hello.s
\`\`\`

\`symta.exe\` is a static-mingw PE32+, x86_64.  Runs on Windows 10+.
No installer, no registry changes, no DLL setup -- the executable
plus the bundled SDL2/etc. DLLs in \`symta\\sdl\\\` are all that's
needed.

To rebuild from source, install w64devkit
(https://github.com/skeeto/w64devkit), open its shell, then:

\`\`\`cmd
cd symta
make -f Makefile.w64 clean && make -f Makefile.w64
\`\`\`

## Layout

  symta\\symta.exe      the runtime binary
  symta\\sdl\\*.dll      SDL2 + dependencies (auto-staged on demand)
  symta\\src\\           the compiler in Symta itself
  symta\\sbc\\           pre-compiled bytecode bootstrap
  symta\\runtime\\       C runtime + JIT + FFI
  symta\\examples\\      40+ runnable example programs
  symta\\tests\\         test suites
  symta\\Makefile.w64   build recipe

## License

Apache 2.0 / MIT dual.  See \`symta\\LICENSE\`.

## Where to find the next release

https://symta.aermia.com
README

# --- step 6: pack archives ------------------------------------------------

echo
echo "=== pack archives ==="
mkdir -p "$DIST_OUT_DIR"

# Linux: gnu tar with gzip.
cd "$STAGING_DIR/linux"
tar czf "$DIST_OUT_DIR/symta-$STAMP-linux.tar.gz" README.md symta/

# Windows: python3's zipfile (works without installing apt zip).
cd "$STAGING_DIR/windows"
python3 - <<PY
import os, zipfile
OUT = "$DIST_OUT_DIR/symta-$STAMP-windows.zip"
with zipfile.ZipFile(OUT, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
    for root, dirs, files in os.walk(".", followlinks=False):
        dirs.sort()
        for fname in sorted(files):
            full = os.path.join(root, fname)
            arc  = os.path.relpath(full, ".")
            zf.write(full, arc)
PY

cd "$DIST_OUT_DIR"
cp "symta-$STAMP-linux.tar.gz"   "symta-latest-linux.tar.gz"
cp "symta-$STAMP-windows.zip"    "symta-latest-windows.zip"

ls -lh "$DIST_OUT_DIR" | sed 's/^/  /'

# --- step 7: sanity-check extraction -------------------------------------

echo
echo "=== verify Linux tarball extracts and runs ==="
VERIFY=/tmp/symta-dist-verify-$$
rm -rf "$VERIFY" && mkdir -p "$VERIFY"
cd "$VERIFY"
tar xzf "$DIST_OUT_DIR/symta-$STAMP-linux.tar.gz"
./symta/symta -f symta/examples/00-hello.s | head -1
cd /
rm -rf "$VERIFY"

# --- step 8 (optional): rsync to the live VPS ----------------------------

if [ "$PUBLISH" = "1" ]; then
  AERMIA_DIR="$HOME/code/aermia.com"
  if [ ! -d "$AERMIA_DIR/symta-site/downloads" ]; then
    echo "  publish requested but $AERMIA_DIR/symta-site/downloads not found." >&2
    echo "  Skipping publish step." >&2
  else
    echo
    echo "=== publish to VPS ==="
    cp "$DIST_OUT_DIR/symta-$STAMP-linux.tar.gz"      "$AERMIA_DIR/symta-site/downloads/"
    cp "$DIST_OUT_DIR/symta-$STAMP-windows.zip"        "$AERMIA_DIR/symta-site/downloads/"
    cp "$DIST_OUT_DIR/symta-latest-linux.tar.gz"       "$AERMIA_DIR/symta-site/downloads/"
    cp "$DIST_OUT_DIR/symta-latest-windows.zip"        "$AERMIA_DIR/symta-site/downloads/"
    rsync -avz "$AERMIA_DIR/symta-site/downloads/" \
      nancy@178.104.157.152:~/aermia.com/symta-site/downloads/
    echo "  published.  live URLs:"
    echo "    https://symta.aermia.com/downloads/symta-$STAMP-linux.tar.gz"
    echo "    https://symta.aermia.com/downloads/symta-$STAMP-windows.zip"
    echo "    https://symta.aermia.com/downloads/symta-latest-linux.tar.gz"
    echo "    https://symta.aermia.com/downloads/symta-latest-windows.zip"
  fi
fi

echo
echo "=== done ==="
echo "Local artifacts: $DIST_OUT_DIR"
echo "If you didn't pass --publish, push to VPS with:"
echo "  PUBLISH=1 ./symta/dev/build-dist.sh $STAMP"
