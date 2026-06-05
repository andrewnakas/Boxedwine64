ifndef BUILD_DIR

.PHONY: default all clean release test multiThreaded wasm64-selftest wasm64-runelf wasm64-selftest-mem64 wasm64-mt

ZIP_ASSET_FILES := $(wildcard *.zip)

default all: release

release: export BUILD_DIR := Build/Release
release: export EXTRA_CPP_FLAGS := $(RELEASE_FLAGS)
release: export SHELL_FILE := shell.html
release: export ASSET_FILES := boxedwine.css boxedwine-shell.js $(ZIP_ASSET_FILES)
test: export BUILD_DIR := Build/Test
test: export EXTRA_CPP_FLAGS := -D__TEST -DASMJIT_STATIC
test: export SHELL_FILE := shelltest.html
test: export EXTRA_LD_FLAGS := --emrun
multiThreaded: export EXTRA_CPP_FLAGS := -DBOXEDWINE_MULTI_THREADED -pthread
multiThreaded: export BUILD_DIR := Build/MultiThreaded
multiThreaded: export SHELL_FILE := shell.html
multiThreaded: export EXTRA_LD_FLAGS := -pthread -sPTHREAD_POOL_SIZE=32 -sPROXY_TO_PTHREAD=1 -sOFFSCREENCANVAS_SUPPORT=0 --js-library=boxedwine-multithreaded-audio.js
multiThreaded: export ASSET_FILES := boxedwine.css boxedwine-shell.js $(ZIP_ASSET_FILES)

# 64-bit guest core, headless, for Node. Builds the BOXEDWINE_GUEST_X64 sources
# (lit up by the -D below — they are #ifdef-guarded and compiled-but-empty in the
# other targets) and reaches the in-process interpreter smoke test via the
# existing main -> boxedmain --x64-selftest path (source/sdl/main.cpp:220).
# Emits a plain .js (no --shell-file: SHELL_LDFLAGS only triggers on a .html
# TARGET_EXEC) so it runs under `node`. wasm32 host is fine: KMemory64 is a
# software page table keyed on U64 guest addresses, so guest pointer width is
# independent of host pointer width (-sMEMORY64 is a later milestone).
wasm64-selftest: export BUILD_DIR := Build/Wasm64SelfTest
wasm64-selftest: export EXTRA_CPP_FLAGS := -DBOXEDWINE_GUEST_X64=1
wasm64-selftest: export TARGET_EXEC := boxedwine64-selftest.js
wasm64-selftest: export EXTRA_LD_FLAGS := -sENVIRONMENT=node -sEXIT_RUNTIME=1

# Same 64-bit core, but loads + runs a real x86_64 ELF off the host disk:
#   node Build/Wasm64RunElf/boxedwine64-runelf.js --x64-run-elf /path/to/elf
# Uses NODERAWFS so the guest loader's fopen() hits the host filesystem (so the
# same static/dynamic ELFs the native --x64-run-elf accepts work unchanged).
# With no path arg it runs the embedded hand-built hello-world ELF.
wasm64-runelf: export BUILD_DIR := Build/Wasm64RunElf
wasm64-runelf: export EXTRA_CPP_FLAGS := -DBOXEDWINE_GUEST_X64=1
wasm64-runelf: export TARGET_EXEC := boxedwine64-runelf.js
wasm64-runelf: export FS_LDFLAGS := -sNODERAWFS=1
wasm64-runelf: export EXTRA_LD_FLAGS := -sENVIRONMENT=node -sEXIT_RUNTIME=1

# wasm64 (Memory64) variant of the self-test: same sources, but the WASM module
# uses 64-bit linear-memory addressing (-sMEMORY64=1), so the host `void*` is
# 64-bit too. This is the build that surfaces any genuine 32-bit-host-pointer
# assumption the wasm32 build hid. Needs a Memory64-capable engine (recent Node /
# d8 / Chrome / Firefox). -sMEMORY64 must be on both compile and link.
wasm64-selftest-mem64: export BUILD_DIR := Build/Wasm64SelfTestMem64
wasm64-selftest-mem64: export EXTRA_CPP_FLAGS := -DBOXEDWINE_GUEST_X64=1 -sMEMORY64=1
wasm64-selftest-mem64: export TARGET_EXEC := boxedwine64-selftest-mem64.js
wasm64-selftest-mem64: export EXTRA_LD_FLAGS := -sMEMORY64=1 -sENVIRONMENT=node -sEXIT_RUNTIME=1

# Browser target for the 64-bit guest: the BOXEDWINE_GUEST_X64 core wired into the
# same multi-threaded browser machinery the 32-bit web build already uses
# (pthreads -> Web Workers, PROXY_TO_PTHREAD, SharedArrayBuffer for the
# clone/futex threading model, canvas via shell.html). This is the foundation for
# running wine64 in a tab; full GL (WebGL backend) and lazy rootfs come on top.
# Produces boxedwine64.html — serve cross-origin-isolated (node server.mjs).
wasm64-mt: export EXTRA_CPP_FLAGS := -DBOXEDWINE_GUEST_X64=1 -DBOXEDWINE_MULTI_THREADED -pthread
wasm64-mt: export BUILD_DIR := Build/Wasm64Mt
wasm64-mt: export SHELL_FILE := shell.html
wasm64-mt: export TARGET_EXEC := boxedwine64.html
# ALLOW_MEMORY_GROWTH: wineboot --init forks wineserver64 + several wine64
# children, each mounting the 205MB rootfs zip; the full boot blows past a fixed
# 512MB heap (observed OOM at "enlarge memory arrays to 536875008"). Growth lets
# the (shared, pthread) heap expand at runtime. Scoped to this target so the
# headless selftest builds keep the cheaper fixed heap.
#
# LEGACY_GL_EMULATION: the gl64 host bridge (source/opengl/gl64bridge.cpp) drives
# the guest's fixed-function GL (glBegin/glEnd, matrix stack, glColor/glVertex,
# lighting) — none of which exist in WebGL2/GLES3. Emscripten's glemu layer
# synthesizes shaders for the FFP, so the bridge's existing native-GL code path
# keeps working under WASM. It pulls in FULL_ES2 and needs WebGL2 allowed, hence
# MAX_WEBGL_VERSION=2. We render the cube into an app-created FBO and glReadPixels
# it back through the X11-wire present sink, so we do NOT need OFFSCREEN_FRAMEBUFFER
# (that's only for compositing a pthread's GL straight to the page canvas).
wasm64-mt: export EXTRA_LD_FLAGS := -pthread -sPTHREAD_POOL_SIZE=32 -sPROXY_TO_PTHREAD=1 -sOFFSCREENCANVAS_SUPPORT=0 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2147483648 -sLEGACY_GL_EMULATION=1 -sMAX_WEBGL_VERSION=2 --js-library=boxedwine-multithreaded-audio.js
wasm64-mt: export ASSET_FILES := boxedwine.css boxedwine-shell.js wine64.html wine64-launcher.js $(ZIP_ASSET_FILES)

cpus := $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || grep -c ^processor /proc/cpuinfo 2>/dev/null || echo 1)
make_jobs := $(shell n='$(cpus)'; expr "$$n" : '^[0-9][0-9]*$$' >/dev/null && [ "$$n" -gt 2 ] && expr "$$n" - 1)

ifneq ($(make_jobs),)
export MAKEFLAGS := -j $(make_jobs)
$(info MAKEFLAGS is $(MAKEFLAGS))
endif
release test multiThreaded wasm64-selftest wasm64-runelf wasm64-selftest-mem64 wasm64-mt:
	@$(MAKE)

clean:
	$(RM) -r Build

else

CC = emcc
CXX = em++
TARGET_EXEC ?= boxedwine.html
PROJDIR := $(realpath $(CURDIR)/../..)
TEST_BUILD_DIR ?= ./build/build_test
$(info MAKEFLAGS $(MAKE_FLAGS))

SOURCES += $(wildcard $(PROJDIR)/source/sdl/*.cpp)
SOURCES += $(wildcard $(PROJDIR)/source/sdl/emscripten/*.cpp)
SOURCES += $(wildcard $(PROJDIR)/platform/sdl/*.cpp)
SOURCES += $(wildcard $(PROJDIR)/platform/linux/*.cpp)
SOURCES += $(shell find $(PROJDIR)/source/emulation -type f -name '*.cpp')
SOURCES += $(wildcard $(PROJDIR)/source/io/*.cpp)
SOURCES += $(wildcard $(PROJDIR)/source/kernel/*.cpp)
SOURCES += $(wildcard $(PROJDIR)/source/kernel/**/*.cpp)
SOURCES += $(wildcard $(PROJDIR)/source/util/*.cpp)
SOURCES += $(wildcard $(PROJDIR)/source/opengl/sdl/*.cpp)
SOURCES += $(wildcard $(PROJDIR)/source/opengl/*.cpp)
SOURCES += $(wildcard $(PROJDIR)/source/vulkan/*.cpp)
SOURCES += $(wildcard $(PROJDIR)/source/x11/*.cpp)
# XWire X11-over-unix-socket server: kunixsocket/startupArgs/knativeinputSDL
# reference XWireServer + installXWireSink/xwireForwardSdlEvent, whose defs live
# here. Most are no-ops without BOXEDWINE_GUEST_X64; xwirepresentSDL.cpp is fully
# GUEST_X64-gated. Needed so the single-threaded 64-bit build links.
SOURCES += $(wildcard $(PROJDIR)/source/x11wire/*.cpp)
SOURCES += $(PROJDIR)/lib/zlib/contrib/minizip/ioapi.c
SOURCES += $(PROJDIR)/lib/zlib/contrib/minizip/mztools.c
SOURCES += $(PROJDIR)/lib/zlib/contrib/minizip/unzip.c
SOURCES += $(PROJDIR)/lib/zlib/contrib/minizip/zip.c

SOFT_SOURCES = $(wildcard $(PROJDIR)/lib/softfloat/source/*.c)
SOFT_SOURCES += $(wildcard $(PROJDIR)/lib/softfloat/source/8086-SSE/*.c)

TEST_SOURCES = $(shell find $(PROJDIR)/lib/asmjit/asmjit -type f -name '*.cpp')
TEST_SOURCES += $(wildcard $(PROJDIR)/source/test/*.cpp)
TEST_SOURCES += $(wildcard $(PROJDIR)/source/test/**/*.cpp)

TEST_SOURCES += $(wildcard $(PROJDIR)/source/sdl/*.cpp)
TEST_SOURCES += $(wildcard $(PROJDIR)/source/sdl/emscripten/*.cpp)
TEST_SOURCES += $(wildcard $(PROJDIR)/platform/sdl/*.cpp)
TEST_SOURCES += $(wildcard $(PROJDIR)/platform/linux/*.cpp)
TEST_SOURCES += $(shell find $(PROJDIR)/source/emulation -type f -name '*.cpp')
TEST_SOURCES += $(wildcard $(PROJDIR)/source/io/*.cpp)
TEST_SOURCES += $(wildcard $(PROJDIR)/source/kernel/*.cpp)
TEST_SOURCES += $(wildcard $(PROJDIR)/source/kernel/**/*.cpp)
TEST_SOURCES += $(wildcard $(PROJDIR)/source/util/*.cpp)
TEST_SOURCES += $(wildcard $(PROJDIR)/source/opengl/sdl/*.cpp)
TEST_SOURCES += $(wildcard $(PROJDIR)/source/opengl/*.cpp)
TEST_SOURCES += $(wildcard $(PROJDIR)/source/vulkan/*.cpp)
TEST_SOURCES += $(wildcard $(PROJDIR)/source/x11/*.cpp)
TEST_SOURCES += $(wildcard $(PROJDIR)/source/x11wire/*.cpp)
TEST_SOURCES += $(PROJDIR)/lib/zlib/contrib/minizip/ioapi.c
TEST_SOURCES += $(PROJDIR)/lib/zlib/contrib/minizip/mztools.c
TEST_SOURCES += $(PROJDIR)/lib/zlib/contrib/minizip/unzip.c
TEST_SOURCES += $(PROJDIR)/lib/zlib/contrib/minizip/zip.c

ifeq ($(BUILD_DIR), Build/Test)
SRCS := $(TEST_SOURCES)
else
SRCS := $(SOURCES)
endif

OBJS := $(SRCS:%=$(BUILD_DIR)/%.o)
SOFT_OBJS := $(SOFT_SOURCES:%=$(BUILD_DIR)/%.o)
ALL := $(OBJS)
ALL += $(SOFT_OBJS)
DEPS := $(ALL:.o=.d)

$(OBJS): INCLUDES := -I../../lib/asmjit -I../../include -I../../lib/simde
$(SOFT_OBJS): INCLUDES := -I../../lib/softfloat/source/include -I../../lib/softfloat/source -I../../lib/softfloat/source/8086-SSE

CXXFLAGS ?= -std=c++20
CPPFLAGS ?= -O3 -s USE_SDL=2 -s USE_ZLIB=1 -DBOXEDWINE_ZLIB -DSDL2=1 -DBOXEDWINE_DISABLE_UI -DSIMDE_SSE2_NO_NATIVE $(GCC_EXTRA_FLAGS) -Wall -Wno-deprecated-pragma -Wno-format-security -Wno-unused-private-field -Wno-invalid-offsetof -Wno-delete-incomplete -Wno-unused-result -Wno-unknown-pragmas -Wno-unused-local-typedefs -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable -mtail-call $(INCLUDES) $(EXTRA_CPP_FLAGS)

SHELL_LDFLAGS := $(if $(filter %.html,$(TARGET_EXEC)),--shell-file $(SHELL_FILE),)
# Filesystem backend. The browser targets persist a guest FS to IndexedDB
# (idbfs). The headless Node run-elf target overrides this with NODERAWFS so the
# guest's loader can fopen() a real x86_64 ELF straight off the host disk
# (NODERAWFS replaces the whole VFS and is mutually exclusive with idbfs).
FS_LDFLAGS ?= -s FORCE_FILESYSTEM -lidbfs.js
LDFLAGS = $(FS_LDFLAGS) -s USE_SDL=2 -s USE_ZLIB=1 -fwasm-exceptions -s INITIAL_MEMORY=536870912 $(SHELL_LDFLAGS) -s EXPORTED_RUNTIME_METHODS='["addRunDependency", "removeRunDependency","ERRNO_CODES","ccall","FS"]' -mtail-call $(EXTRA_LD_FLAGS)

#$(TEST_BUILD_DIR)/boxedwineTest: $(TEST_OBJS)
#	$(CC) $(TEST_OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/$(TARGET_EXEC): $(ALL)
	@$(CC) $(ALL) -o $@ $(LDFLAGS)
ifneq ($(strip $(ASSET_FILES)),)
	@for asset in $(ASSET_FILES); do \
		if [ -e "$$asset" ]; then cp "$$asset" "$(BUILD_DIR)/"; fi; \
	done
endif

# assembly
$(BUILD_DIR)/%.s.o: %.s
	@$(MKDIR_P) $(dir $@)
	@$(AS) $(ASFLAGS) -c $< -o $@

# c source
$(BUILD_DIR)/%.c.o: %.c
	@$(MKDIR_P) $(dir $@)
	@$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# c++ source
$(BUILD_DIR)/%.cpp.o: %.cpp
	@$(MKDIR_P) $(dir $@)
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

.PHONY: clean

clean:
	$(RM) -r $(BUILD_DIR)

-include $(DEPS)

MKDIR_P ?= mkdir -p

endif
