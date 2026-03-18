# Define the C++ compiler and standard.
CXX = gcc
# Use -O2 for optimization, -Wall -Wextra for warnings, and -std=c++17
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -g

# --- Paths and Files ---

# Assuming you have cloned ChampSim into a directory named 'ChampSim'
OBJ_DIR = obj
CHAMPSIM_DIR = ../ChampSim
CACHE_SRC = src/cachesim.cc
PREDICTOR_SRC = src/predictor.cc
MRC_SRC = src/mrc.cc
MAIN_SRC = src/main.cc
TRACEREADER_SRC = $(CHAMPSIM_DIR)/src/tracereader.cc
TARGET = cachesim
TARGET_MULTI = cachesim_multi

# Object files
CACHE_OBJ = $(OBJ_DIR)/cachesim.o
PREDICTOR_OBJ = $(OBJ_DIR)/predictor.o
MRC_OBJ = $(OBJ_DIR)/mrc.o
MAIN_OBJ = $(OBJ_DIR)/main.o
MULTI_MAIN_OBJ = $(OBJ_DIR)/multi_main.o
TRACEREADER_OBJ = $(OBJ_DIR)/tracereader.o
OBJS = $(TRACEREADER_OBJ) $(CACHE_OBJ) $(MRC_OBJ) $(MAIN_OBJ)
MULTI_LEVEL_OBJS = $(TRACEREADER_OBJ) $(CACHE_OBJ) $(PREDICTOR_OBJ) $(MRC_OBJ) $(MULTI_MAIN_OBJ)
ALL_OBJS = $(TRACEREADER_OBJ) $(CACHE_OBJ) $(MRC_OBJ) $(MAIN_OBJ) $(MULTI_MAIN_OBJ) $(PREDICTOR_OBJ)

# --- VCPKG Integration ---

# Deduces the VCPKG Triplet (e.g., x64-linux, x64-windows)
# You may need to replace this with your actual triplet if 'x64-linux' is incorrect.
# To check, look inside the ChampSim/vcpkg_installed/ directory.
VCPKG_TRIPLET = x64-linux
VCPKG_INSTALLED_DIR = $(CHAMPSIM_DIR)/vcpkg_installed/$(VCPKG_TRIPLET)

# Flags to find VCPKG headers (e.g., for <zstd.h>, <lzma.h>)
VCPKG_INC_FLAGS = -I$(VCPKG_INSTALLED_DIR)/include

# Flags to find VCPKG libraries (e.g., liblzma.a, libzstd.a)
VCPKG_LIB_FLAGS = -L$(VCPKG_INSTALLED_DIR)/lib -L$(VCPKG_INSTALLED_DIR)/lib/manual-link

# List of required VCPKG libraries to link against.
# ChampSim commonly uses lzma (for .xz traces) and zstd (for .zst traces).
# The `-static` flag is often useful to ensure a single portable executable.
VCPKG_LIBS = -llzma -lfmt -lbz2 -lz -lCLI11 -lstdc++

# --- Compiler/Linker Flags ---

# Include directories: ChampSim root for structures + VCPKG headers
INC_FLAGS = -I$(CHAMPSIM_DIR) -I$(CHAMPSIM_DIR)/inc $(VCPKG_INC_FLAGS) -Iinc

# Linker flags: VCPKG library directory + VCPKG libraries
LDFLAGS = $(VCPKG_LIB_FLAGS) $(VCPKG_LIBS)


# --- Rules ---

.PHONY: all clean

# Default rule: builds the target executable
all: $(TARGET) $(TARGET_MULTI)

# 1. Linking rule: Links all object files and VCPKG libraries into the final executable
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

$(TARGET_MULTI): $(MULTI_LEVEL_OBJS)
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@ -DMULTI_LEVEL

# 2. Compilation rule for your C++ file
# Your file needs access to ChampSim headers and VCPKG headers.
$(CACHE_OBJ): $(CACHE_SRC)
	$(CXX) $(CXXFLAGS) $(INC_FLAGS) -c $< -o $@

$(PREDICTOR_OBJ): $(PREDICTOR_SRC)
	$(CXX) $(CXXFLAGS) $(INC_FLAGS) -c $< -o $@

$(MRC_OBJ): $(MRC_SRC)
	$(CXX) $(CXXFLAGS) $(INC_FLAGS) -c $< -o $@

$(MAIN_OBJ): $(MAIN_SRC)
	$(CXX) $(CXXFLAGS) $(INC_FLAGS) -c $< -o $@

$(MULTI_MAIN_OBJ): $(MAIN_SRC)
	$(CXX) $(CXXFLAGS) $(INC_FLAGS) -c $< -o $@ -DMULTI_LEVEL


# 3. Compilation rule for the ChampSim utility file
# This file also needs access to ChampSim headers and VCPKG headers.
$(TRACEREADER_OBJ): $(TRACEREADER_SRC)
	$(CXX) $(CXXFLAGS) $(INC_FLAGS) -c $< -o $@

# Clean up all generated files
clean:
	rm -f $(TARGET) $(ALL_OBJS)
