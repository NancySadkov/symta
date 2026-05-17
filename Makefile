# Symta — standalone build orchestrator.
#
# This Makefile makes ./symta a self-contained portfolio project:
# the runtime, C FFI plugins, examples, and tests live under symta/
# and have no dependency on the parent SoM repo. The only external
# requirement is a working POSIX toolchain (w64devkit on Windows,
# Xcode CLT on macOS, build-essential on Linux). See BUILDING.md.
#
# Quick reference:
#   make                build runtime + plugins + examples
#   make plugins        just the C FFI plugins (gfx, ui, ttf, svg)
#   make runtime        just symta(.exe)
#   make examples       compile every example under examples/
#   make test-gfx       gfx-FFI golden image tests
#   make test-tokenizer tokenizer regression tests
#   make test-reader    reader/parser regression tests
#   make test-macros    macro/DSL behavior tests
#   make test-runtime   Symta single-file regression tests
#   make test-compiler  compiler-output (.sbc) regression tests
#   make test-uim       UIM widget regression tests (headless)
#   make test-ffi       FFI dispatcher (sffi) regression suite
#   make test-drift     5-stage bootstrap drift test (no codegen drift)
#   make test-all       all of the above (bottom-up order)
#   make screenshots    recapture UIM baselines (needs a display)
#   make clean          delete every build artifact

# ------------------------------------------------------------------ platform

UNAME := $(shell uname -s 2>/dev/null || echo Windows_NT)
ifeq ($(findstring MINGW,$(UNAME)),MINGW)
  PLATFORM := w64
endif
ifeq ($(findstring MSYS,$(UNAME)),MSYS)
  PLATFORM := w64
endif
ifeq ($(findstring CYGWIN,$(UNAME)),CYGWIN)
  PLATFORM := w64
endif
ifeq ($(UNAME),Windows_NT)
  PLATFORM := w64
endif
ifeq ($(UNAME),Darwin)
  PLATFORM := osx
endif
ifeq ($(UNAME),Linux)
  PLATFORM := linux
endif
PLATFORM ?= w64

SUFFIX_w64 := Makefile.w64
SUFFIX_osx := Makefile.osx
SUFFIX_linux := Makefile

# ------------------------------------------------------------------ plugins
#
# The standalone Symta distribution ships gfx, ui, ttf, svg. vfx
# (sparse voxel octree, transpiled through the niche New-C
# compiler) lives at the SoM project root — see ../vfx — and is
# only built when SoM the game or VoxPie need it. A fresh symta
# checkout doesn't pull vfx; if you `use slb` from your project
# without it, `ffi_begin export vfx` errors out at compile time
# with a clear "Missing ffi/vfx.ffi" message.

PLUGINS := gfx ui ttf svg

gfx_DIR := c/gfx
ui_DIR  := c/ui
ttf_DIR := c/ttf
svg_DIR := c/svg

gfx_MAKEFILE := $(SUFFIX_$(PLATFORM))
ui_MAKEFILE  := $(SUFFIX_$(PLATFORM))
ttf_MAKEFILE := Makefile.w64
svg_MAKEFILE := Makefile.w64

gfx_OUT := lib/gfx.ffi
ui_OUT  := lib/ui.ffi
ttf_OUT := lib/ttf.ffi
svg_OUT := out/svg.ffi

FFI_DIR := ffi

ifeq ($(PLATFORM),w64)
  SYMTA_EXE := ./symta.exe
else
  SYMTA_EXE := ./symta
endif

# ------------------------------------------------------------------ targets

.PHONY: all help plugins runtime examples \
        test-gfx test-tokenizer test-reader test-runtime test-compiler \
        test-macros test-uim test-ffi test-am test-drift test-all \
        screenshots check-tools \
        clean clean-plugins clean-runtime clean-examples clean-tests \
        $(PLUGINS)

all: plugins runtime examples

help:
	@echo "Symta build — platform: $(PLATFORM)"
	@echo ""
	@echo "  make                    build everything (default)"
	@echo "  make plugins            build + install $(PLUGINS)"
	@echo "  make <plugin>           build a single plugin: $(PLUGINS)"
	@echo "  make runtime            build symta executable"
	@echo "  make examples           compile every examples/*/ project"
	@echo "  make test-gfx           gfx-FFI golden image tests"
	@echo "  make test-tokenizer     tokenizer regression tests"
	@echo "  make test-reader        reader/parser regression tests"
	@echo "  make test-macros        macro/DSL regression tests"
	@echo "  make test-runtime       Symta single-file regression tests"
	@echo "  make test-compiler      compiler-output (.sbc) regression"
	@echo "  make test-uim           UIM widget regression tests (headless)"
	@echo "  make test-ffi           FFI dispatcher (sffi) regression suite"
	@echo "  make test-am            adaptive-map regression suite"
	@echo "  make test-drift         5-stage bootstrap byte-equality check"
	@echo "  make test-all           run every test suite bottom-up"
	@echo "  make screenshots        capture baseline UIM PNGs"
	@echo "  make clean              remove all build outputs"

check-tools:
	@command -v gcc >/dev/null  || (echo "ERROR: gcc not in PATH"; exit 1)
	@command -v make >/dev/null || (echo "ERROR: make not in PATH"; exit 1)
	@command -v cp >/dev/null   || (echo "ERROR: cp not in PATH"; exit 1)
	@echo "tools OK (platform=$(PLATFORM))"

# --- runtime -------------------------------------------------------
# sffi (runtime/sffi/) is the in-tree FFI dispatcher.  Compiled as
# part of the runtime, not as a separate lib.  See
# runtime/sffi/ARCHITECTURE.md for the design.

runtime:
	@echo "[runtime] symta executable"
	@$(MAKE) -s -f Makefile.$(PLATFORM)

# --- plugins -------------------------------------------------------
#
# One template per plugin: build, then install the .ffi into ffi/
# so a freshly-cloned `symta .` picks it up without manual copying.

define PLUGIN_RULE
$(1):
	@echo "[plugin] $(1)"
	@$$(MAKE) -s -C $($(1)_DIR) -f $($(1)_MAKEFILE)
	@mkdir -p $(FFI_DIR)
	@cp -f $($(1)_DIR)/$($(1)_OUT) $(FFI_DIR)/$(1).ffi
endef
$(foreach p,$(PLUGINS),$(eval $(call PLUGIN_RULE,$(p))))

plugins: $(PLUGINS)

# --- examples ------------------------------------------------------
#
# `symta <dir>` compiles every changed `.s` to `.sbc` and (re)links
# the dir's `go.exe`. examples/ contains many subdirs and a handful
# of single-file demos; we compile only the subdir examples here so
# `make examples` is quick. The single-file examples are exercised
# by the runtime test sweep instead.

# `$(wildcard examples/*/)` is meant to glob only directories (the
# trailing slash), but on Windows make / w64devkit it also matches
# single-file `examples/*.s` entries, so `make examples` then tries
# to `cd` into e.g. `examples/00-hello.s` and dies.  Filter the
# match list down to actual directories via `wildcard <each>/.`,
# which only resolves for real dirs (the implicit `.` entry exists
# in a dir but not in a file).
EXAMPLE_DIRS := $(sort $(dir $(wildcard examples/*/.)))

examples: runtime plugins
	@for d in $(EXAMPLE_DIRS); do \
	  echo "[symta] $$d"; \
	  ( cd "$$d" && "../../$(SYMTA_EXE)" . ) || exit 1; \
	done

# --- test suites ---------------------------------------------------
#
# Each suite lives under tests/<name>/ with its own run.sh driver.
# They all use the runtime + (for tests that need plugins) the FFI
# blobs in ffi/, which `make plugins` populates.

test-gfx: runtime plugins
	@echo "[run] gfx-FFI golden-image tests"
	@bash tests/gfx/run.sh

test-tokenizer: runtime
	@echo "[run] tokenizer regression tests"
	@bash tests/tokenizer/run.sh

test-reader: runtime
	@echo "[run] reader regression tests"
	@bash tests/reader/run.sh

test-runtime: runtime
	@echo "[run] runtime regression tests"
	@bash tests/runtime/run.sh
	@echo "[run] line-number regression tests"
	@bash tests/runtime/lineno-check.sh

test-compiler: runtime
	@echo "[run] compiler-output regression tests"
	@bash tests/compiler/run.sh

test-macros: runtime
	@echo "[run] macro/DSL regression tests"
	@bash tests/macros/run.sh

test-uim: runtime plugins
	@echo "[run] UIM widget regression tests"
	@bash tests/uim/run.sh

test-ffi: runtime
	@echo "[run] FFI dispatcher (sffi) regression suite"
	@bash tests/ffi/run.sh

test-am: runtime
	@echo "[run] adaptive-map regression suite"
	@bash tests/am/run.sh

test-drift: runtime
	@echo "[run] self-hosting compiler drift (5-stage bootstrap)"
	@bash tests/bootstrap/drift.sh

test-all: test-tokenizer test-reader test-macros test-runtime \
          test-compiler test-gfx test-uim test-ffi test-am test-drift
	@echo "[ok] all symta test suites passed"

# Recapture every UIM baseline PNG. Useful after an intentional
# rendering change; review with `git diff --stat tests/uim/baselines/`
# afterwards. Requires a display since SDL still opens a window in
# screenshot mode.
screenshots: runtime plugins
	@mkdir -p tests/uim/baselines
	@bash tests/uim/run.sh --capture

# --- clean ---------------------------------------------------------

clean-runtime:
	-@$(MAKE) -s -f Makefile.$(PLATFORM) clean

clean-plugins:
	@for p in $(PLUGINS); do \
	  $(MAKE) -s -C c/$$p -f $$( \
	    case $$p in \
	      gfx|ui) echo Makefile.$(PLATFORM) ;; \
	      ttf|svg) echo Makefile.w64 ;; \
	    esac \
	  ) clean 2>/dev/null || true; \
	done
	@# Only nuke .ffi files we know how to rebuild; leave prebuilt
	@# blobs (e.g. zlib.ffi) alone.
	@for p in $(PLUGINS); do rm -f $(FFI_DIR)/$$p.ffi; done

clean-examples:
	@for d in $(EXAMPLE_DIRS); do \
	  rm -rf "$$d/sbc" "$$d/lib" "$$d/go.exe" "$$d/cache"; \
	done

clean-tests:
	-@rm -rf tests/gfx/sbc tests/gfx/lib tests/gfx/cache tests/gfx/go.exe tests/gfx/out
	-@rm -rf tests/uim/sbc tests/uim/lib tests/uim/ffi tests/uim/cache tests/uim/go.exe
	-@rm -rf tests/uim/actual tests/uim/build.log
	-@rm -rf tests/uim/pic tests/uim/ttf tests/uim/*.dll
	-@rm -rf tests/compiler/build

clean: clean-runtime clean-plugins clean-examples clean-tests
	@echo "all cleaned"
