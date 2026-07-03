#!/bin/bash
# Generate test data files (C/H/CMake files)

set -e

# Configuration
NUM_C_FILES=${1:-10}
NUM_H_FILES=${2:-10}
OUTPUT_DIR=${3:-.}

# Create subdirectories
mkdir -p "${OUTPUT_DIR}/src"
mkdir -p "${OUTPUT_DIR}/include"
mkdir -p "${OUTPUT_DIR}/build"

# Function to generate a C source file
generate_c_file() {
	local file_num=$1
	local output_path="${OUTPUT_DIR}/src/module_${file_num}.c"

	cat > "${output_path}" << EOF
/* module_${file_num}.c - Generated test module */
#include "../include/module_${file_num}.h"
#include <stdio.h>
#include <stdlib.h>

int module_${file_num}_init(void) {
	printf("Initializing module ${file_num}\\n");
	return 0;
}

int module_${file_num}_process(const char* data) {
	if (data == NULL) {
		fprintf(stderr, "Error: Invalid input to module_${file_num}\\n");
		return -1;
	}
	printf("Processing data in module_${file_num}: %s\\n", data);
	return 0;
}

void module_${file_num}_cleanup(void) {
	printf("Cleaning up module_${file_num}\\n");
}
EOF
}

# Function to generate a header file
generate_h_file() {
	local file_num=$1
	local header_guard="MODULE_${file_num}_H"
	local output_path="${OUTPUT_DIR}/include/module_${file_num}.h"

	cat > "${output_path}" << EOF
#ifndef ${header_guard}
#define ${header_guard}

/* module_${file_num}.h - Header for test module ${file_num} */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize module ${file_num}
 * @return 0 on success, negative on error
 */
int module_${file_num}_init(void);

/**
 * Process data in module ${file_num}
 * @param data Input data to process
 * @return 0 on success, negative on error
 */
int module_${file_num}_process(const char* data);

/**
 * Clean up module ${file_num}
 */
void module_${file_num}_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* ${header_guard} */
EOF
}

# Generate C files
echo "Generating ${NUM_C_FILES} C source files..."
for ((i = 1; i <= NUM_C_FILES; i++)); do
	generate_c_file "$i"
	echo "  Generated src/module_${i}.c"
done

# Generate header files
echo "Generating ${NUM_H_FILES} header files..."
for ((i = 1; i <= NUM_H_FILES; i++)); do
	generate_h_file "$i"
	echo "  Generated include/module_${i}.h"
done

# Generate root CMakeLists.txt
echo "Generating CMakeLists.txt files..."
cat > "${OUTPUT_DIR}/CMakeLists.txt" << 'EOF'
cmake_minimum_required(VERSION 3.10)
project(TestProject C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Add include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Collect all source files
file(GLOB SOURCES "src/*.c")

# Create library
add_library(testlib STATIC ${SOURCES})

# Set build output directory
set_target_properties(testlib PROPERTIES
	ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# Enable testing
enable_testing()

# Add tests directory if it exists
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
	add_subdirectory(tests)
endif()
EOF

echo "  Generated CMakeLists.txt"

# Generate build/CMakeLists.txt
cat > "${OUTPUT_DIR}/build/CMakeLists.txt" << 'EOF'
cmake_minimum_required(VERSION 3.10)
project(TestBuild C)

# Include parent project
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include)

# This is a placeholder for sub-build configurations
set(CMAKE_BUILD_TYPE Release)
EOF

echo "  Generated build/CMakeLists.txt"

# Generate Makefile placeholder
cat > "${OUTPUT_DIR}/Makefile" << 'EOF'
.PHONY: all clean build test

all: build

build:
	@mkdir -p build
	@cd build && cmake .. && make

test:
	@cd build && ctest

clean:
	@rm -rf build

.DEFAULT_GOAL := all
EOF

echo "  Generated Makefile"

# Generate README
cat > "${OUTPUT_DIR}/README.md" << 'EOF'
# Test Project

This is an auto-generated test project with multiple C modules.

## Structure

- `src/` - C source files (module_1.c through module_N.c)
- `include/` - Header files (module_1.h through module_N.h)
- `build/` - Build directory
- `CMakeLists.txt` - CMake build configuration

## Building

```bash
make build
```

## Testing

```bash
make test
```

## Cleaning

```bash
make clean
```
EOF

echo "  Generated README.md"
echo "Test data generation complete!"
