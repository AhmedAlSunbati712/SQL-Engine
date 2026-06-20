CXX = g++
CXXFLAGS = -Wall --std=c++23 -Iinclude -Iinclude/disk -Iinclude/encoding -I/opt/homebrew/include
LDFLAGS = -L/opt/homebrew/lib
LDLIBS = -lgtest -lgtest_main
AR = ar
ARFLAGS = rcs

LIB = build/libsqlengine.a

SRC = \
	src/DLList.cpp \
	src/JournalCodec.cpp \
	src/PCache.cpp \
	src/Pager.cpp \
	src/disk/DiskIO.cpp \
	src/encoding/Endian.cpp

OBJ = \
	build/DLList.o \
	build/JournalCodec.o \
	build/PCache.o \
	build/Pager.o \
	build/disk/DiskIO.o \
	build/encoding/Endian.o

UNIT_TEST_SRC := $(wildcard tests/unit/*.cpp)
UNIT_TEST_OBJ := $(patsubst tests/unit/%.cpp,build/tests/unit/%.o,$(UNIT_TEST_SRC))
UNIT_TEST_BIN := $(patsubst tests/unit/%.cpp,build/tests/unit/%,$(UNIT_TEST_SRC))

.PHONY: all test clean
.SECONDARY: $(UNIT_TEST_OBJ)

all: $(LIB) $(UNIT_TEST_BIN)

$(LIB): $(OBJ)
	mkdir -p $(dir $@)
	$(AR) $(ARFLAGS) $@ $^

build/%.o: src/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/tests/unit/%.o: tests/unit/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/tests/unit/%: build/tests/unit/%.o $(LIB)
	mkdir -p $(dir $@)
	$(CXX) $^ -o $@ $(LDFLAGS) $(LDLIBS)

test: $(UNIT_TEST_BIN)
	for test_bin in $(UNIT_TEST_BIN); do ./$$test_bin; done

clean:
	rm -rf build
