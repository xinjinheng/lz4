/*
 * Copyright (c) 2024, The LZ4 Authors
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree),
 * meaning you may select, at your option, one of the above-listed licenses.
 */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "lz4.h"
#include "lz4frame.h"
#include "lz4chunked.h"

#define TEST_DATA "This is a test string for the chunked LZ4 compression module."
#define TEST_DATA_SIZE sizeof(TEST_DATA) - 1

#define TEMP_FILE "test_lz4chunked.tmp"
#define CHUNKED_FILE "test_lz4chunked.lz4c"
#define INDEX_FILE "test_lz4chunked.lz4i"
#define DECOMPRESSED_FILE "test_lz4chunked.out"

static void cleanup(void) {
    remove(TEMP_FILE);
    remove(CHUNKED_FILE);
    remove(INDEX_FILE);
    remove(DECOMPRESSED_FILE);
}

static void write_test_data(const char* filename) {
    FILE* f = fopen(filename, "wb");
    assert(f != NULL);
    fwrite(TEST_DATA, 1, TEST_DATA_SIZE, f);
    fclose(f);
}

static int compare_files(const char* file1, const char* file2) {
    FILE* f1 = fopen(file1, "rb");
    FILE* f2 = fopen(file2, "rb");
    assert(f1 != NULL && f2 != NULL);

    int ret = 0;
    int c1, c2;
    do {
        c1 = fgetc(f1);
        c2 = fgetc(f2);
        if (c1 != c2) {
            ret = 1;
            break;
        }
    } while (c1 != EOF && c2 != EOF);

    fclose(f1);
    fclose(f2);
    return ret;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    // Clean up any existing test files
    cleanup();

    // Create test data file
    write_test_data(TEMP_FILE);

    // Test 1: Compress with chunked
    printf("Test 1: Chunked compression\n");
    LZ4_chunkedConfig config = LZ4_createChunkedConfig();
    assert(config != NULL);

    LZ4_setChunkSize(config, 32 * 1024); // 32KB chunks
    LZ4_setChecksum(config, LZ4_CHECKSUM_XXH32);
    LZ4_setThreads(config, 4);

    int result = LZ4_compressChunked(TEMP_FILE, CHUNKED_FILE, config);
    assert(result == 0);
    printf("✓ Compression successful\n");

    // Test 2: Decompress with chunked
    printf("Test 2: Chunked decompression\n");
    result = LZ4_decompressChunked(CHUNKED_FILE, DECOMPRESSED_FILE);
    assert(result == 0);
    printf("✓ Decompression successful\n");

    // Verify decompressed data matches original
    if (compare_files(TEMP_FILE, DECOMPRESSED_FILE) == 0) {
        printf("✓ Decompressed data matches original\n");
    } else {
        fprintf(stderr, "✗ Decompressed data does not match original\n");
        cleanup();
        return 1;
    }

    // Test 3: Verify chunk
    printf("Test 3: Verify chunk\n");
    LZ4_chunkedIndex* index = LZ4_loadChunkedIndex(CHUNKED_FILE);
    assert(index != NULL);

    result = LZ4_verifyChunk(CHUNKED_FILE, 0);
    assert(result == 0);
    printf("✓ Chunk 0 verified\n");

    LZ4_freeChunkedIndex(index);

    // Clean up
    cleanup();

    printf("\nAll tests passed!\n");
    return 0;
}
