# --- Directories ---
# Using relative paths is usually safer for Make across different environments
SRC_DIR        := src
OBJ_DIR        := obj
BIN_DIR        := bin
INC_DIR        := inc
CHAMPSIM_DIR   := ../ChampSim

# --- Build type ---
# BUILD selects the flag set / object dir / binary suffix: release (default), debug, asan
BUILD          ?= release

ifeq ($(BUILD),debug)
  BUILD_CXXFLAGS := -std=c++20 -Wall -Wextra -O0 -g -MMD -MP
  BUILD_LDFLAGS  :=
  SUFFIX         := _debug
else ifeq ($(BUILD),asan)
  BUILD_CXXFLAGS := -fsanitize=address -fno-omit-frame-pointer -fno-common -std=c++20 -Wall -Wextra -O2 -g -MMD -MP
  BUILD_LDFLAGS  := -fsanitize=address
  SUFFIX         := _asan
else
  BUILD_CXXFLAGS := -std=c++20 -Wall -Wextra -O2 -g -MMD -MP
  BUILD_LDFLAGS  :=
  SUFFIX         :=
endif

BUILD_OBJ_DIR  := $(OBJ_DIR)/$(BUILD)

# --- Files ---
# 1. Handle your local files with wildcards
LOCAL_SRCS     := $(wildcard $(SRC_DIR)/*.cc)
LOCAL_OBJS_BASE  := $(LOCAL_SRCS:$(SRC_DIR)/%.cc=$(BUILD_OBJ_DIR)/base/%.o)
LOCAL_OBJS_MULTI := $(LOCAL_SRCS:$(SRC_DIR)/%.cc=$(BUILD_OBJ_DIR)/multi/%.o)

# 2. Handle tracereader explicitly
TRACEREADER_SRC := $(CHAMPSIM_DIR)/src/tracereader.cc
TRACEREADER_OBJ := $(BUILD_OBJ_DIR)/tracereader.o

# 3. Combine them
OBJS_BASE  := $(LOCAL_OBJS_BASE) $(TRACEREADER_OBJ)
OBJS_MULTI := $(LOCAL_OBJS_MULTI) $(TRACEREADER_OBJ)

# --- Compiler & Flags ---
CXX            := gcc
CXXFLAGS       := $(BUILD_CXXFLAGS)

INC_FLAGS      := -I$(CHAMPSIM_DIR) -I$(CHAMPSIM_DIR)/inc \
                  -I$(CHAMPSIM_DIR)/vcpkg_installed/x64-linux/include -I$(INC_DIR)
LDFLAGS        := $(BUILD_LDFLAGS) -L$(CHAMPSIM_DIR)/vcpkg_installed/x64-linux/lib \
                  -L$(CHAMPSIM_DIR)/vcpkg_installed/x64-linux/lib/manual-link

LDLIBS         := -llzma -lfmt -lbz2 -lz -lCLI11 -lstdc++ -lm

# --- Binary names (suffixed per build type so debug/asan don't clobber release) ---
CACHESIM_BIN       := $(BIN_DIR)/cachesim$(SUFFIX)
CACHESIM_MULTI_BIN := $(BIN_DIR)/cachesim_multi$(SUFFIX)

# --- Targets ---
# `all`: every build type, both binaries (six binaries total).
all: release debug asan

# `both`: cachesim + cachesim_multi for whatever BUILD is currently set.
# Internal helper so release/debug/asan each cover both binaries in one build type.
both: $(CACHESIM_BIN) $(CACHESIM_MULTI_BIN)

# release/debug/asan: both binaries, one build type each.
release:
	$(MAKE) BUILD=release both

debug:
	$(MAKE) BUILD=debug both

asan:
	$(MAKE) BUILD=asan both

# cachesim/cachesim_multi: release build, single binary each.
cachesim: $(CACHESIM_BIN)

cachesim_multi: $(CACHESIM_MULTI_BIN)

$(CACHESIM_BIN): $(OBJS_BASE)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) $(LDLIBS) -o $@

$(CACHESIM_MULTI_BIN): $(OBJS_MULTI)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) $(LDLIBS) -o $@ -DMULTI_LEVEL

# --- Rules ---

# Explicit rule for tracereader (This fixes the "No rule to make target" error)
$(BUILD_OBJ_DIR)/tracereader.o: $(TRACEREADER_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC_FLAGS) -c $< -o $@

# Pattern rules for everything in your src/ folder
$(BUILD_OBJ_DIR)/base/%.o: $(SRC_DIR)/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC_FLAGS) -c $< -o $@

$(BUILD_OBJ_DIR)/multi/%.o: $(SRC_DIR)/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC_FLAGS) -DMULTI_LEVEL -c $< -o $@

-include $(OBJS_BASE:.o=.d)
-include $(OBJS_MULTI:.o=.d)

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

.PHONY: all both release debug asan cachesim cachesim_multi clean
