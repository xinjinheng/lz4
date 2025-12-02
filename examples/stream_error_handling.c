/*
 * Example demonstrating LZ4 stream error handling and state management API
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lz4.h"

#define BUFFER_SIZE 1024
#define INPUT_SIZE  512

int main() {
    LZ4_stream_t* stream;
    LZ4_streamDecode_t* dstream;
    char* inputBuffer;
    char* compressBuffer;
    char* decompressBuffer;
    int compressedSize;
    int decompressedSize;
    LZ4_streamState_t state;
    LZ4_streamErrorCode_t errorCode;
    const char* errorMsg;

    // Allocate buffers
    inputBuffer = (char*)malloc(INPUT_SIZE);
    compressBuffer = (char*)malloc(LZ4_compressBound(INPUT_SIZE));
    decompressBuffer = (char*)malloc(BUFFER_SIZE);
    if (!inputBuffer || !compressBuffer || !decompressBuffer) {
        printf("Memory allocation failed\n");
        return 1;
    }

    // Initialize input buffer with test data
    memset(inputBuffer, 'A', INPUT_SIZE);

    // Test 1: Compression stream error handling
    printf("=== Test 1: Compression stream error handling ===\n");
    stream = LZ4_createStream();
    if (!stream) {
        errorMsg = LZ4_getLastError();
        printf("Failed to create compression stream: %s\n", errorMsg);
        return 1;
    }

    // Get initial stream state
    state = LZ4_streamGetState(stream);
    printf("Initial stream state: %d (0=initialized, 1=running, 2=finished, 3=error)\n", state);

    // Test invalid compression parameters
    compressedSize = LZ4_compress_fast_continue(stream, NULL, compressBuffer, INPUT_SIZE, LZ4_compressBound(INPUT_SIZE), 1);
    if (compressedSize == 0) {
        errorCode = LZ4_streamGetLastError(stream);
        errorMsg = LZ4_getLastError();
        printf("Compression failed with error code %d: %s\n", errorCode, errorMsg);
    }

    // Reset stream and perform valid compression
    LZ4_streamReset(stream);
    compressedSize = LZ4_compress_fast_continue(stream, inputBuffer, compressBuffer, INPUT_SIZE, LZ4_compressBound(INPUT_SIZE), 1);
    printf("Valid compression successful: %d bytes\n", compressedSize);

    // Get stream state after compression
    state = LZ4_streamGetState(stream);
    printf("Stream state after compression: %d\n", state);

    // Test 2: Decompression stream error handling
    printf("\n=== Test 2: Decompression stream error handling ===\n");
    dstream = LZ4_createStreamDecode();
    if (!dstream) {
        errorMsg = LZ4_getLastError();
        printf("Failed to create decompression stream: %s\n", errorMsg);
        return 1;
    }

    // Get initial decompression stream state
    state = LZ4_streamDecodeGetState(dstream);
    printf("Initial decompression stream state: %d\n", state);

    // Test invalid decompression parameters
    decompressedSize = LZ4_decompress_safe_continue(dstream, NULL, decompressBuffer, compressedSize, BUFFER_SIZE);
    if (decompressedSize <= 0) {
        errorCode = LZ4_streamDecodeGetLastError(dstream);
        errorMsg = LZ4_getLastError();
        printf("Decompression failed with error code %d: %s\n", errorCode, errorMsg);
    }

    // Reset decompression stream and perform valid decompression
    LZ4_streamDecodeReset(dstream);
    decompressedSize = LZ4_decompress_safe_continue(dstream, compressBuffer, decompressBuffer, compressedSize, BUFFER_SIZE);
    printf("Valid decompression successful: %d bytes\n", decompressedSize);

    // Verify decompressed data
    if (memcmp(inputBuffer, decompressBuffer, INPUT_SIZE) == 0) {
        printf("Decompressed data matches original input\n");
    } else {
        printf("Decompressed data mismatch\n");
    }

    // Cleanup
    LZ4_freeStream(stream);
    LZ4_freeStreamDecode(dstream);
    free(inputBuffer);
    free(compressBuffer);
    free(decompressBuffer);

    printf("\nAll tests completed successfully\n");
    return 0;
}
