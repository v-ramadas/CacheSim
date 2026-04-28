# --- Directories ---
# Using relative paths is usually safer for Make across different environments
SRC_DIR        := src
OBJ_DIR        := obj
INC_DIR        := inc
CHAMPSIM_DIR   := ../ChampSim

# --- Files ---
# 1. Handle your local files with wildcards
LOCAL_SRCS     := $(wildcard $(SRC_DIR)/*.cc)
LOCAL_OBJS_BASE  := $(LOCAL_SRCS:$(SRC_DIR)/%.cc=$(OBJ_DIR)/base/%.o)
LOCAL_OBJS_MULTI := $(LOCAL_SRCS:$(SRC_DIR)/%.cc=$(OBJ_DIR)/multi/%.o)

# 2. Handle tracereader explicitly
TRACEREADER_SRC := $(CHAMPSIM_DIR)/src/tracereader.cc
TRACEREADER_OBJ := $(OBJ_DIR)/tracereader.o

# 3. Combine them
OBJS_BASE  := $(LOCAL_OBJS_BASE) $(TRACEREADER_OBJ)
OBJS_MULTI := $(LOCAL_OBJS_MULTI) $(TRACEREADER_OBJ)

# --- Compiler & Flags ---
CXX            := gcc
CXXFLAGS       := -std=c++20 -Wall -Wextra -O2 -g -MMD -MP
INC_FLAGS      := -I$(CHAMPSIM_DIR) -I$(CHAMPSIM_DIR)/inc \
                  -I$(CHAMPSIM_DIR)/vcpkg_installed/x64-linux/include -I$(INC_DIR)
LDFLAGS        := -L$(CHAMPSIM_DIR)/vcpkg_installed/x64-linux/lib \
                  -L$(CHAMPSIM_DIR)/vcpkg_installed/x64-linux/lib/manual-link
LDLIBS         := -llzma -lfmt -lbz2 -lz -lCLI11 -lstdc++

# --- Targets ---
all: cachesim cachesim_multi

cachesim: $(OBJS_BASE)
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) $(LDLIBS) -o $@

cachesim_multi: $(OBJS_MULTI)
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) $(LDLIBS) -o $@ -DMULTI_LEVEL

# --- Rules ---

# Explicit rules for tracereader (This fixes the "No rule to make target" error)
$(TRACEREADER_OBJ): $(TRACEREADER_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC_FLAGS) -c $< -o $@

# Pattern rules for everything in your src/ folder
$(OBJ_DIR)/base/%.o: $(SRC_DIR)/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC_FLAGS) -c $< -o $@

$(OBJ_DIR)/multi/%.o: $(SRC_DIR)/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC_FLAGS) -DMULTI_LEVEL -c $< -o $@

-include $(OBJS_BASE:.o=.d)
-include $(OBJS_MULTI:.o=.d)

clean:
	rm -rf $(OBJ_DIR) cachesim cachesim_multi

.PHONY: all clean
