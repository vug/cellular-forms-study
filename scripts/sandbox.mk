# Plain GNU make build for machines without CMake (the Linux sandbox M1 was first developed in).
# The real build is CMake (CMakeLists.txt, CMakePresets.json).
#   make -f scripts/sandbox.mk BUILD=/some/dir -j2 [OPT=-O0]
BUILD ?= build/sandbox
CXX ?= g++
OPT ?= -O2
# -ffp-contract=off: no fused multiply-add, so results do not depend on how the compiler
# chooses to contract expressions.
CXXFLAGS ?= -std=c++23 $(OPT) -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -ffp-contract=off -pthread
CPPFLAGS := -Isrc

CORE_SRC := $(wildcard src/core/*.cpp)
TEST_SRC := $(wildcard tests/*.cpp)
CORE_OBJ := $(CORE_SRC:%.cpp=$(BUILD)/%.o)
TEST_OBJ := $(filter-out $(BUILD)/tests/oracle_tool.o,$(TEST_SRC:%.cpp=$(BUILD)/%.o))
APPS := $(BUILD)/cf_tests $(BUILD)/cf_oracle $(BUILD)/cf_run $(BUILD)/cf_render

all: $(APPS)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/cf_tests: $(CORE_OBJ) $(TEST_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@
$(BUILD)/cf_oracle: $(CORE_OBJ) $(BUILD)/tests/oracle_tool.o
	$(CXX) $(CXXFLAGS) $^ -o $@
$(BUILD)/cf_run: $(CORE_OBJ) $(BUILD)/apps/cf_run/main.o
	$(CXX) $(CXXFLAGS) $^ -o $@
$(BUILD)/cf_render: $(CORE_OBJ) $(BUILD)/apps/cf_render/main.o
	$(CXX) $(CXXFLAGS) $^ -o $@

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
.PHONY: all
