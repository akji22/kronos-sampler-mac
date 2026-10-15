CXX ?= g++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -ISource
ASAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g

CORE_SRCS := \
	Source/AudioEngine/SamplerEngine.cpp \
	Source/Voices/SamplerVoice.cpp \
	Source/Streaming/DiskSampleStreamer.cpp \
	Source/Effects/EffectsChain.cpp \
	Source/Browser/LibraryBrowserModel.cpp \
	Source/Formats/WavAiffFormatLoader.cpp \
	Source/Formats/SfzFormatLoader.cpp \
	Source/Formats/Sf2FormatLoader.cpp \
	Source/Formats/KontaktFormatInspector.cpp

TEST_SRCS := Tests/TestMain.cpp

BUILD_DIR := build_make
TEST_BIN := $(BUILD_DIR)/KonosEngineTests
ASAN_BIN := $(BUILD_DIR)/KonosEngineTests_ASan

.PHONY: all test test-asan clean

all: $(TEST_BIN)

$(TEST_BIN): $(CORE_SRCS) $(TEST_SRCS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CORE_SRCS) $(TEST_SRCS) -o $(TEST_BIN) -lm -lpthread

$(ASAN_BIN): $(CORE_SRCS) $(TEST_SRCS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(ASAN_FLAGS) $(CORE_SRCS) $(TEST_SRCS) -o $(ASAN_BIN) -lm -lpthread

test: $(TEST_BIN)
	./$(TEST_BIN)

test-asan: $(ASAN_BIN)
	./$(ASAN_BIN)

clean:
	rm -rf $(BUILD_DIR) Tests/*.wav Tests/*.sfz Tests/*.sf2 Tests/*.nki Tests/*.nkx Tests/*.ncw
