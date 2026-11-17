CXX ?= c++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: test clean

test: build/test_gart_range_allocator build/test_gart_mapping_lifecycle build/test_gart_range_bounds build/test_amdgpu_vram_allocator
	./build/test_gart_range_allocator
	./build/test_gart_mapping_lifecycle
	./build/test_gart_range_bounds
	./build/test_amdgpu_vram_allocator

build/test_gart_range_allocator: ../../src/amd/gart_range_allocator.cpp test_gart_range_allocator.cpp ../../src/amd/gart_range_allocator.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -I../../src/amd ../../src/amd/gart_range_allocator.cpp test_gart_range_allocator.cpp -o $@

build/test_gart_mapping_lifecycle: ../../src/amd/gart_mapping_lifecycle.cpp test_gart_mapping_lifecycle.cpp ../../src/amd/gart_mapping_lifecycle.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -I../../src/amd ../../src/amd/gart_mapping_lifecycle.cpp test_gart_mapping_lifecycle.cpp -o $@

build/test_gart_range_bounds: test_gart_range_bounds.cpp ../../src/amd/gart_range_bounds.h ../../src/amd/amdgpu_range_bounds.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -I../../src/amd test_gart_range_bounds.cpp -o $@

build/test_amdgpu_vram_allocator: test_amdgpu_vram_allocator.cpp ../../src/amd/amdgpu_vram.h ../../src/amd/amdgpu_range_bounds.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -I../../src/amd test_amdgpu_vram_allocator.cpp -o $@

clean:
	rm -rf build
