CXX = g++
CXXFLAGS = -Wall --std=c++23 -Iinclude -Iinclude/disk -Iinclude/encoding -Iinclude/containers -I/opt/homebrew/include
LDFLAGS = -L/opt/homebrew/lib
LDLIBS = -lgtest -lgtest_main
AR = ar
ARFLAGS = rcs

LIB = build/libsqlengine.a

SRC = \
	src/containers/BTree.cpp \
	src/containers/BTreePage.cpp \
	src/containers/DLList.cpp \
	src/JournalCodec.cpp \
	src/LockMgr.cpp \
	src/PCache.cpp \
	src/Pager.cpp \
	src/disk/DiskIO.cpp \
	src/encoding/Endian.cpp \
	src/DBHeaderCodec.cpp

OBJ = \
	build/containers/BTree.o \
	build/containers/BTreePage.o \
	build/containers/DLList.o \
	build/JournalCodec.o \
	build/LockMgr.o \
	build/PCache.o \
	build/Pager.o \
	build/disk/DiskIO.o \
	build/encoding/Endian.o \
	build/DBHeaderCodec.o

UNIT_TEST_SRC := $(wildcard tests/unit/*.cpp)
UNIT_TEST_OBJ := $(patsubst tests/unit/%.cpp,build/tests/unit/%.o,$(UNIT_TEST_SRC))
UNIT_TEST_BIN := $(patsubst tests/unit/%.cpp,build/tests/unit/%,$(UNIT_TEST_SRC))

INTEGRATION_TEST_SRC := $(wildcard tests/integration/*.cpp)
INTEGRATION_TEST_OBJ := $(patsubst tests/integration/%.cpp,build/tests/integration/%.o,$(INTEGRATION_TEST_SRC))
INTEGRATION_TEST_BIN := $(patsubst tests/integration/%.cpp,build/tests/integration/%,$(INTEGRATION_TEST_SRC))

.PHONY: all test test-unit test-integration clean
.SECONDARY: $(UNIT_TEST_OBJ) $(INTEGRATION_TEST_OBJ)

all: $(LIB) $(UNIT_TEST_BIN) $(INTEGRATION_TEST_BIN)

$(LIB): $(OBJ)
	mkdir -p $(dir $@)
	$(AR) $(ARFLAGS) $@ $^

build/%.o: src/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/tests/integration/%.o: tests/integration/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/tests/integration/%: build/tests/integration/%.o $(LIB)
	mkdir -p $(dir $@)
	$(CXX) $^ -o $@ $(LDFLAGS) $(LDLIBS)

build/tests/unit/%.o: tests/unit/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/tests/unit/%: build/tests/unit/%.o $(LIB)
	mkdir -p $(dir $@)
	$(CXX) $^ -o $@ $(LDFLAGS) $(LDLIBS)

test-unit: $(UNIT_TEST_BIN)
	for test_bin in $(UNIT_TEST_BIN); do ./$$test_bin; done

test-integration: $(INTEGRATION_TEST_BIN)
	for test_bin in $(INTEGRATION_TEST_BIN); do ./$$test_bin; done

test: test-unit test-integration

clean:
	rm -rf build
