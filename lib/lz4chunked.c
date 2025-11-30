/*
 * LZ4 Chunked Compression with Resumable Support
 * Implementation File
 * Copyright (c) 2024. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include <pthread.h>
#include "lz4chunked.h"

#ifdef _WIN32
#include <windows.h>
#endif


/* ************************************
 *  Private Structures
 ************************************/ 

/* Thread data structure */
typedef struct {
    const char* inputPath;      /* Input file path */
    const char* tempDir;        /* Temporary directory */
    long long chunkOffset;      /* Offset of the chunk in the input file */
    size_t chunkSize;           /* Size of the chunk */
    int chunkId;                /* Chunk ID */
    int compressionLevel;       /* Compression level */
    int enableChecksum;         /* Enable checksum */
    LZ4Chunked_Chunk* chunk;    /* Chunk metadata to populate */
    int* error;                 /* Error code (shared between threads) */
} LZ4Chunked_ThreadData;


/* ************************************
 *  Private Functions
 ************************************/ 

/**
 * autoDetectThreads - Auto-detect the number of CPU cores
 * 
 * @return Number of CPU cores, or 4 if detection fails
 */
static int autoDetectThreads(void) {
    #ifdef _WIN32
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return sysinfo.dwNumberOfProcessors;
    #else
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    return (nprocs > 0) ? (int)nprocs : 4;
    #endif
}

/**
 * isTextFile - Determine if a file is text or binary
 * 
 * @param filePath Path to the file
 * 
 * @return 1 if text, 0 if binary, negative error code on failure
 */
static int isTextFile(const char* filePath) {
    FILE* file = fopen(filePath, "rb");
    if (!file) return -errno;

    unsigned char buffer[1024];
    size_t bytesRead = fread(buffer, 1, sizeof(buffer), file);
    fclose(file);

    for (size_t i = 0; i < bytesRead; i++) {
        if (buffer[i] == 0) return 0; /* Null byte indicates binary */
        if (iscntrl(buffer[i]) && !isspace(buffer[i])) return 0; /* Control character indicates binary */
    }

    return 1; /* Text file */
}

/**
 * getChunkFileName - Generate the temporary chunk file name
 * 
 * @param tempDir  Temporary directory
 * @param chunkId  Chunk ID
 * @param buffer   Buffer to store the file name
 * @param bufSize  Size of the buffer
 * 
 * @return 0 on success, negative error code on failure
 */
static int getChunkFileName(const char* tempDir, int chunkId, char* buffer, size_t bufSize) {
    if (!tempDir || !buffer || bufSize == 0) return -EINVAL;

    #ifdef _WIN32
    if (snprintf(buffer, bufSize, "%s\\lz4chunk_%d.tmp", tempDir, chunkId) < 0) {
        return -errno;
    }
    #else
    if (snprintf(buffer, bufSize, "%s/lz4chunk_%d.tmp", tempDir, chunkId) < 0) {
        return -errno;
    }
    #endif

    return 0;
}

/**
 * compressChunk - Compress a single chunk
 * 
 * @param threadData Thread data structure
 * 
 * @return 0 on success, negative error code on failure
 */
static void* compressChunk(void* threadData) {
    LZ4Chunked_ThreadData* data = (LZ4Chunked_ThreadData*)threadData;
    if (!data || *data->error != 0) {
        pthread_exit(NULL);
    }

    // Open input file
    FILE* inputFile = fopen(data->inputPath, "rb");
    if (!inputFile) {
        *data->error = -errno;
        pthread_exit(NULL);
    }

    // Seek to chunk offset
    if (fseeko(inputFile, data->chunkOffset, SEEK_SET) != 0) {
        *data->error = -errno;
        fclose(inputFile);
        pthread_exit(NULL);
    }

    // Read chunk data
    unsigned char* chunkData = (unsigned char*)malloc(data->chunkSize);
    if (!chunkData) {
        *data->error = -ENOMEM;
        fclose(inputFile);
        pthread_exit(NULL);
    }

    size_t bytesRead = fread(chunkData, 1, data->chunkSize, inputFile);
    fclose(inputFile);

    if (bytesRead == 0) {
        *data->error = -EIO;
        free(chunkData);
        pthread_exit(NULL);
    }

    // Update chunk metadata
    data->chunk->uncompressedSize = bytesRead;
    data->chunk->state = LZ4CHUNKED_STATE_PROCESSING;

    // Calculate checksum if enabled
    if (data->enableChecksum) {
        data->chunk->checksum = XXH32(chunkData, bytesRead, 0);
    }

    // Create LZ4 frame compression context
    LZ4F_preferences_t prefs = {0};
    prefs.compressionLevel = data->compressionLevel;
    prefs.frameInfo.blockMode = LZ4F_blockLinked;
    prefs.frameInfo.contentSize = bytesRead;
    prefs.frameInfo.checksumFlag = LZ4F_noChecksum; // We use our own checksum

    size_t ctxSize = LZ4F_createCompressionContext(NULL, LZ4F_VERSION);
    LZ4F_cctx* ctx = (LZ4F_cctx*)malloc(ctxSize);
    if (!ctx) {
        *data->error = -ENOMEM;
        free(chunkData);
        pthread_exit(NULL);
    }

    LZ4F_errorCode_t err = LZ4F_createCompressionContext(&ctx, LZ4F_VERSION);
    if (LZ4F_isError(err)) {
        *data->error = (int)err;
        free(ctx);
        free(chunkData);
        pthread_exit(NULL);
    }

    // Calculate maximum compressed size
    size_t maxCompressedSize = LZ4F_compressBound(bytesRead, &prefs);
    unsigned char* compressedData = (unsigned char*)malloc(maxCompressedSize);
    if (!compressedData) {
        *data->error = -ENOMEM;
        LZ4F_freeCompressionContext(ctx);
        free(chunkData);
        pthread_exit(NULL);
    }

    // Compress the chunk
    size_t compressedSize = LZ4F_compressFrame(compressedData, maxCompressedSize, chunkData, bytesRead, &prefs);
    if (LZ4F_isError(compressedSize)) {
        *data->error = (int)compressedSize;
        free(compressedData);
        LZ4F_freeCompressionContext(ctx);
        free(chunkData);
        pthread_exit(NULL);
    }

    // Free resources
    LZ4F_freeCompressionContext(ctx);
    free(chunkData);

    // Write compressed data to temporary file
    char chunkFileName[512];
    err = getChunkFileName(data->tempDir, data->chunkId, chunkFileName, sizeof(chunkFileName));
    if (err != 0) {
        *data->error = err;
        free(compressedData);
        pthread_exit(NULL);
    }

    FILE* chunkFile = fopen(chunkFileName, "wb");
    if (!chunkFile) {
        *data->error = -errno;
        free(compressedData);
        pthread_exit(NULL);
    }

    size_t bytesWritten = fwrite(compressedData, 1, compressedSize, chunkFile);
    fclose(chunkFile);

    if (bytesWritten != compressedSize) {
        *data->error = -EIO;
        free(compressedData);
        pthread_exit(NULL);
    }

    // Update chunk metadata
    data->chunk->compressedSize = compressedSize;
    data->chunk->state = LZ4CHUNKED_STATE_COMPLETED;

    // Free compressed data
    free(compressedData);

    pthread_exit(NULL);
}


/* ************************************
 *  Public API Implementation
 ************************************/ 

/**
 * LZ4_createDefaultConfig - Create a default configuration
 */
LZ4Chunked_Config* LZ4_createDefaultConfig(void) {
    LZ4Chunked_Config* config = (LZ4Chunked_Config*)malloc(sizeof(LZ4Chunked_Config));
    if (!config) return NULL;

    config->chunkSize = LZ4CHUNKED_DEFAULT_CHUNK_SIZE;
    config->compressionLevel = 3; // Default LZ4 compression level
    config->threadCount = LZ4CHUNKED_DEFAULT_THREADS;
    config->enableChecksum = 1; // Enable checksum by default
    config->tempDir = NULL; // Use system temp directory by default

    return config;
}

/**
 * LZ4_freeConfig - Free a configuration
 */
void LZ4_freeConfig(LZ4Chunked_Config* config) {
    if (config) {
        free(config);
    }
}

/**
 * LZ4_loadIndex - Load an index file from disk
 */
LZ4Chunked_Index* LZ4_loadIndex(const char* idxPath) {
    if (!idxPath) return NULL;

    FILE* file = fopen(idxPath, "rb");
    if (!file) return NULL;

    // Get file size
    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (fileSize <= 0) {
        fclose(file);
        return NULL;
    }

    // Read file content
    char* content = (char*)malloc(fileSize + 1);
    if (!content) {
        fclose(file);
        return NULL;
    }

    size_t bytesRead = fread(content, 1, fileSize, file);
    fclose(file);

    if (bytesRead != (size_t)fileSize) {
        free(content);
        return NULL;
    }
    content[fileSize] = '\0';

    // Parse JSON
    LZ4Chunked_Index* index = (LZ4Chunked_Index*)malloc(sizeof(LZ4Chunked_Index));
    if (!index) {
        free(content);
        return NULL;
    }

    memset(index, 0, sizeof(LZ4Chunked_Index));

    // TODO: Implement proper JSON parsing
    // This is a simplified implementation for now
    if (strstr(content, LZ4CHUNKED_MAGIC) == NULL) {
        LZ4_freeIndex(index);
        free(content);
        return NULL;
    }

    // Extract version
    char* versionStr = strstr(content, "\"version\":");
    if (versionStr) {
        index->version = atoi(versionStr + 10);
    }

    // Extract totalChunks
    char* totalChunksStr = strstr(content, "\"totalChunks\":");
    if (totalChunksStr) {
        index->totalChunks = atoi(totalChunksStr + 15);
    }

    // Extract fileSize
    char* fileSizeStr = strstr(content, "\"fileSize\":");
    if (fileSizeStr) {
        index->fileSize = atoll(fileSizeStr + 12);
    }

    // Extract createTime
    char* createTimeStr = strstr(content, "\"createTime\":");
    if (createTimeStr) {
        index->createTime = (time_t)atoll(createTimeStr + 14);
    }

    // Extract config
    char* chunkSizeStr = strstr(content, "\"chunkSize\":");
    if (chunkSizeStr) {
        index->config.chunkSize = atoll(chunkSizeStr + 13);
    }

    char* compressionLevelStr = strstr(content, "\"compressionLevel\":");
    if (compressionLevelStr) {
        index->config.compressionLevel = atoi(compressionLevelStr + 21);
    }

    char* threadCountStr = strstr(content, "\"threadCount\":");
    if (threadCountStr) {
        index->config.threadCount = atoi(threadCountStr + 17);
    }

    char* enableChecksumStr = strstr(content, "\"enableChecksum\":");
    if (enableChecksumStr) {
        index->config.enableChecksum = atoi(enableChecksumStr + 19);
    }

    // Extract chunks
    if (index->totalChunks > 0) {
        index->chunks = (LZ4Chunked_Chunk*)malloc(index->totalChunks * sizeof(LZ4Chunked_Chunk));
        if (!index->chunks) {
            LZ4_freeIndex(index);
            free(content);
            return NULL;
        }

        memset(index->chunks, 0, index->totalChunks * sizeof(LZ4Chunked_Chunk));

        // Simple chunk parsing (for demonstration only)
        char* chunksStr = strstr(content, "\"chunks\":[");
        if (chunksStr) {
            char* chunkStr = chunksStr + 9;
            for (int i = 0; i < index->totalChunks; i++) {
                char* chunkIdStr = strstr(chunkStr, "\"chunkId\":");
                if (chunkIdStr) {
                    index->chunks[i].chunkId = atoi(chunkIdStr + 11);
                }

                char* uncompressedSizeStr = strstr(chunkStr, "\"uncompressedSize\":");
                if (uncompressedSizeStr) {
                    index->chunks[i].uncompressedSize = atoll(uncompressedSizeStr + 22);
                }

                char* compressedSizeStr = strstr(chunkStr, "\"compressedSize\":");
                if (compressedSizeStr) {
                    index->chunks[i].compressedSize = atoll(compressedSizeStr + 20);
                }

                char* offsetStr = strstr(chunkStr, "\"offset\":");
                if (offsetStr) {
                    index->chunks[i].offset = atoll(offsetStr + 10);
                }

                char* checksumStr = strstr(chunkStr, "\"checksum\":");
                if (checksumStr) {
                    index->chunks[i].checksum = (XXH32_hash_t)strtoul(checksumStr + 12, NULL, 10);
                }

                char* stateStr = strstr(chunkStr, "\"state\":");
                if (stateStr) {
                    index->chunks[i].state = (LZ4Chunked_State)atoi(stateStr + 10);
                }

                // Move to next chunk
                chunkStr = strstr(chunkStr, "}");
                if (chunkStr) chunkStr += 2;
            }
        }
    }

    free(content);
    return index;
}

/**
 * LZ4_saveIndex - Save an index file to disk
 */
int LZ4_saveIndex(const char* idxPath, const LZ4Chunked_Index* index) {
    if (!idxPath || !index) return -EINVAL;

    // Calculate required buffer size
    size_t bufSize = 1024 + index->totalChunks * 512;
    char* buffer = (char*)malloc(bufSize);
    if (!buffer) return -ENOMEM;

    // Write JSON header
    int pos = 0;
    pos += snprintf(buffer + pos, bufSize - pos, "{\n");
    pos += snprintf(buffer + pos, bufSize - pos, "  \"magic\": \"%s\",\n", LZ4CHUNKED_MAGIC);
    pos += snprintf(buffer + pos, bufSize - pos, "  \"version\": %d,\n", index->version);
    pos += snprintf(buffer + pos, bufSize - pos, "  \"totalChunks\": %d,\n", index->totalChunks);
    pos += snprintf(buffer + pos, bufSize - pos, "  \"fileSize\": %lld,\n", index->fileSize);
    pos += snprintf(buffer + pos, bufSize - pos, "  \"createTime\": %lld,\n", (long long)index->createTime);

    // Write config
    pos += snprintf(buffer + pos, bufSize - pos, "  \"config\": {\n");
    pos += snprintf(buffer + pos, bufSize - pos, "    \"chunkSize\": %llu,\n", (unsigned long long)index->config.chunkSize);
    pos += snprintf(buffer + pos, bufSize - pos, "    \"compressionLevel\": %d,\n", index->config.compressionLevel);
    pos += snprintf(buffer + pos, bufSize - pos, "    \"threadCount\": %d,\n", index->config.threadCount);
    pos += snprintf(buffer + pos, bufSize - pos, "    \"enableChecksum\": %d,\n", index->config.enableChecksum);
    if (index->config.tempDir) {
        pos += snprintf(buffer + pos, bufSize - pos, "    \"tempDir\": \"%s\"\n", index->config.tempDir);
    } else {
        pos += snprintf(buffer + pos, bufSize - pos, "    \"tempDir\": null\n");
    }
    pos += snprintf(buffer + pos, bufSize - pos, "  },\n");

    // Write chunks
    pos += snprintf(buffer + pos, bufSize - pos, "  \"chunks\": [\n");
    for (int i = 0; i < index->totalChunks; i++) {
        const LZ4Chunked_Chunk* chunk = &index->chunks[i];
        pos += snprintf(buffer + pos, bufSize - pos, "    {\n");
        pos += snprintf(buffer + pos, bufSize - pos, "      \"chunkId\": %d,\n", chunk->chunkId);
        pos += snprintf(buffer + pos, bufSize - pos, "      \"uncompressedSize\": %llu,\n", (unsigned long long)chunk->uncompressedSize);
        pos += snprintf(buffer + pos, bufSize - pos, "      \"compressedSize\": %llu,\n", (unsigned long long)chunk->compressedSize);
        pos += snprintf(buffer + pos, bufSize - pos, "      \"offset\": %lld,\n", chunk->offset);
        pos += snprintf(buffer + pos, bufSize - pos, "      \"checksum\": %u,\n", chunk->checksum);
        pos += snprintf(buffer + pos, bufSize - pos, "      \"state\": %d\n", chunk->state);
        pos += snprintf(buffer + pos, bufSize - pos, "    }%s\n", (i < index->totalChunks - 1) ? "," : "");
    }
    pos += snprintf(buffer + pos, bufSize - pos, "  ]\n");
    pos += snprintf(buffer + pos, bufSize - pos, "}\n");

    // Write to file
    FILE* file = fopen(idxPath, "wb");
    if (!file) {
        free(buffer);
        return -errno;
    }

    size_t bytesWritten = fwrite(buffer, 1, pos, file);
    fclose(file);
    free(buffer);

    if (bytesWritten != (size_t)pos) {
        return -EIO;
    }

    return 0;
}

/**
 * LZ4_freeIndex - Free an index structure
 */
void LZ4_freeIndex(LZ4Chunked_Index* index) {
    if (index) {
        if (index->chunks) {
            free(index->chunks);
        }
        free(index);
    }
}

/**
 * LZ4_chunkedCompress - Compress a large file using chunked LZ4 compression
 */
int LZ4_chunkedCompress(const char* inputPath, const char* outputPath, const LZ4Chunked_Config* config) {
    if (!inputPath || !outputPath) return -EINVAL;

    // Use default config if not provided
    LZ4Chunked_Config localConfig;
    if (config) {
        memcpy(&localConfig, config, sizeof(LZ4Chunked_Config));
    } else {
        LZ4Chunked_Config* defaultConfig = LZ4_createDefaultConfig();
        if (!defaultConfig) return -ENOMEM;
        memcpy(&localConfig, defaultConfig, sizeof(LZ4Chunked_Config));
        LZ4_freeConfig(defaultConfig);
    }

    // Auto-detect thread count if not specified
    if (localConfig.threadCount <= 0) {
        localConfig.threadCount = autoDetectThreads();
    }

    // Auto-adjust chunk size based on file type
    int isText = isTextFile(inputPath);
    if (isText < 0) return isText;

    if (localConfig.chunkSize == LZ4CHUNKED_DEFAULT_CHUNK_SIZE) {
        localConfig.chunkSize = isText ? LZ4CHUNKED_TEXT_CHUNK_SIZE : LZ4CHUNKED_BINARY_CHUNK_SIZE;
    }

    // Get input file size
    struct stat st;
    if (stat(inputPath, &st) != 0) return -errno;
    long long fileSize = st.st_size;

    // Calculate total chunks
    int totalChunks = (fileSize + localConfig.chunkSize - 1) / localConfig.chunkSize;
    if (totalChunks == 0) return 0; // Empty file

    // Create index structure
    LZ4Chunked_Index* index = (LZ4Chunked_Index*)malloc(sizeof(LZ4Chunked_Index));
    if (!index) return -ENOMEM;

    memset(index, 0, sizeof(LZ4Chunked_Index));
    memcpy(index->magic, LZ4CHUNKED_MAGIC, sizeof(LZ4CHUNKED_MAGIC) - 1);
    index->version = LZ4CHUNKED_VERSION;
    index->totalChunks = totalChunks;
    index->fileSize = fileSize;
    index->createTime = time(NULL);
    memcpy(&index->config, &localConfig, sizeof(LZ4Chunked_Config));

    index->chunks = (LZ4Chunked_Chunk*)malloc(totalChunks * sizeof(LZ4Chunked_Chunk));
    if (!index->chunks) {
        LZ4_freeIndex(index);
        return -ENOMEM;
    }

    // Initialize chunk metadata
    for (int i = 0; i < totalChunks; i++) {
        index->chunks[i].chunkId = i + 1;
        index->chunks[i].state = LZ4CHUNKED_STATE_UNPROCESSED;
        index->chunks[i].offset = 0;
    }

    // Generate index file path
    char idxPath[1024];
    if (snprintf(idxPath, sizeof(idxPath), "%s.lz4idx", outputPath) < 0) {
        LZ4_freeIndex(index);
        return -errno;
    }

    // Save initial index file
    int err = LZ4_saveIndex(idxPath, index);
    if (err != 0) {
        LZ4_freeIndex(index);
        return err;
    }

    // Prepare thread data
    LZ4Chunked_ThreadData* threadData = (LZ4Chunked_ThreadData*)malloc(localConfig.threadCount * sizeof(LZ4Chunked_ThreadData));
    if (!threadData) {
        LZ4_freeIndex(index);
        return -ENOMEM;
    }

    pthread_t* threads = (pthread_t*)malloc(localConfig.threadCount * sizeof(pthread_t));
    if (!threads) {
        free(threadData);
        LZ4_freeIndex(index);
        return -ENOMEM;
    }

    int globalError = 0;
    int currentChunk = 0;
    long long currentOffset = 0;
    long long outputOffset = 0;

    // Compress chunks in parallel
    while (currentChunk < totalChunks && globalError == 0) {
        int threadsToUse = (totalChunks - currentChunk < localConfig.threadCount) ? 
                           (totalChunks - currentChunk) : localConfig.threadCount;

        // Initialize thread data
        for (int i = 0; i < threadsToUse; i++) {
            threadData[i].inputPath = inputPath;
            threadData[i].tempDir = localConfig.tempDir;
            threadData[i].chunkOffset = currentOffset + i * localConfig.chunkSize;
            threadData[i].chunkSize = (i == threadsToUse - 1 && currentOffset + i * localConfig.chunkSize + localConfig.chunkSize > fileSize) ? 
                                    (fileSize - currentOffset - i * localConfig.chunkSize) : localConfig.chunkSize;
            threadData[i].chunkId = currentChunk + i + 1;
            threadData[i].compressionLevel = localConfig.compressionLevel;
            threadData[i].enableChecksum = localConfig.enableChecksum;
            threadData[i].chunk = &index->chunks[currentChunk + i];
            threadData[i].error = &globalError;

            // Update chunk state to processing
            index->chunks[currentChunk + i].state = LZ4CHUNKED_STATE_PROCESSING;
        }

        // Start threads
        for (int i = 0; i < threadsToUse; i++) {
            if (pthread_create(&threads[i], NULL, compressChunk, &threadData[i]) != 0) {
                globalError = -errno;
                break;
            }
        }

        // Wait for threads to finish
        for (int i = 0; i < threadsToUse; i++) {
            pthread_join(threads[i], NULL);
        }

        // Update index and save
        if (globalError == 0) {
            // Calculate chunk offsets in output file
            for (int i = 0; i < threadsToUse; i++) {
                index->chunks[currentChunk + i].offset = outputOffset;
                outputOffset += index->chunks[currentChunk + i].compressedSize;
            }

            // Save updated index file
            err = LZ4_saveIndex(idxPath, index);
            if (err != 0) {
                globalError = err;
                break;
            }

            currentChunk += threadsToUse;
            currentOffset += threadsToUse * localConfig.chunkSize;
        }
    }

    // Cleanup
    free(threads);
    free(threadData);

    if (globalError != 0) {
        LZ4_freeIndex(index);
        return globalError;
    }

    // Merge temporary chunk files into final output file
    FILE* outputFile = fopen(outputPath, "wb");
    if (!outputFile) {
        LZ4_freeIndex(index);
        return -errno;
    }

    unsigned char buffer[1024 * 1024]; // 1MB buffer
    for (int i = 0; i < totalChunks; i++) {
        // Get chunk file name
        char chunkFileName[512];
        err = getChunkFileName(localConfig.tempDir, i + 1, chunkFileName, sizeof(chunkFileName));
        if (err != 0) {
            fclose(outputFile);
            LZ4_freeIndex(index);
            return err;
        }

        // Open chunk file
        FILE* chunkFile = fopen(chunkFileName, "rb");
        if (!chunkFile) {
            fclose(outputFile);
            LZ4_freeIndex(index);
            return -errno;
        }

        // Read and write chunk data
        size_t bytesRead;
        while ((bytesRead = fread(buffer, 1, sizeof(buffer), chunkFile)) > 0) {
            if (fwrite(buffer, 1, bytesRead, outputFile) != bytesRead) {
                fclose(chunkFile);
                fclose(outputFile);
                LZ4_freeIndex(index);
                return -EIO;
            }
        }

        fclose(chunkFile);

        // Delete temporary chunk file
        #ifdef _WIN32
        DeleteFile(chunkFileName);
        #else
        unlink(chunkFileName);
        #endif
    }

    fclose(outputFile);

    // Save final index file
    err = LZ4_saveIndex(idxPath, index);
    if (err != 0) {
        LZ4_freeIndex(index);
        return err;
    }

    LZ4_freeIndex(index);
    return 0;
}

/**
 * LZ4_chunkedDecompress - Decompress a chunked LZ4 file
 */
int LZ4_chunkedDecompress(const char* inputPath, const char* idxPath, const char* outputPath) {
    if (!inputPath || !idxPath || !outputPath) return -EINVAL;

    // Load index file
    LZ4Chunked_Index* index = LZ4_loadIndex(idxPath);
    if (!index) return -ENOENT;

    // Open input file
    FILE* inputFile = fopen(inputPath, "rb");
    if (!inputFile) {
        LZ4_freeIndex(index);
        return -errno;
    }

    // Open output file
    FILE* outputFile = fopen(outputPath, "wb");
    if (!outputFile) {
        fclose(inputFile);
        LZ4_freeIndex(index);
        return -errno;
    }

    // Decompress each chunk
    unsigned char* decompressedData = (unsigned char*)malloc(index->config.chunkSize);
    if (!decompressedData) {
        fclose(inputFile);
        fclose(outputFile);
        LZ4_freeIndex(index);
        return -ENOMEM;
    }

    int err = 0;
    for (int i = 0; i < index->totalChunks; i++) {
        const LZ4Chunked_Chunk* chunk = &index->chunks[i];

        // Seek to chunk offset
        if (fseeko(inputFile, chunk->offset, SEEK_SET) != 0) {
            err = -errno;
            break;
        }

        // Read compressed chunk data
        unsigned char* compressedData = (unsigned char*)malloc(chunk->compressedSize);
        if (!compressedData) {
            err = -ENOMEM;
            break;
        }

        size_t bytesRead = fread(compressedData, 1, chunk->compressedSize, inputFile);
        if (bytesRead != chunk->compressedSize) {
            err = -EIO;
            free(compressedData);
            break;
        }

        // Decompress the chunk
        size_t decompressedSize = LZ4F_decompressFrame(decompressedData, index->config.chunkSize, 
                                                     compressedData, chunk->compressedSize, NULL);
        if (LZ4F_isError(decompressedSize)) {
            err = (int)decompressedSize;
            free(compressedData);
            break;
        }

        // Verify checksum if enabled
        if (index->config.enableChecksum) {
            XXH32_hash_t checksum = XXH32(decompressedData, decompressedSize, 0);
            if (checksum != chunk->checksum) {
                err = -EINVAL; // Checksum mismatch
                free(compressedData);
                break;
            }
        }

        // Write decompressed data to output file
        if (fwrite(decompressedData, 1, decompressedSize, outputFile) != decompressedSize) {
            err = -EIO;
            free(compressedData);
            break;
        }

        free(compressedData);
    }

    // Cleanup
    free(decompressedData);
    fclose(inputFile);
    fclose(outputFile);
    LZ4_freeIndex(index);

    return err;
}

/**
 * LZ4_resumeCompress - Resume an interrupted chunked compression
 */
int LZ4_resumeCompress(const char* idxPath) {
    if (!idxPath) return -EINVAL;

    // Load index file
    LZ4Chunked_Index* index = LZ4_loadIndex(idxPath);
    if (!index) return -ENOENT;

    // Get input file path from index file name
    char inputPath[1024];
    strncpy(inputPath, idxPath, sizeof(inputPath) - 7); // Remove ".lz4idx"
    inputPath[sizeof(inputPath) - 1] = '\0';

    // Get output file path from index file name
    char outputPath[1024];
    strncpy(outputPath, idxPath, sizeof(outputPath) - 7); // Remove ".lz4idx"
    outputPath[sizeof(outputPath) - 1] = '\0';

    // Prepare thread data
    LZ4Chunked_ThreadData* threadData = (LZ4Chunked_ThreadData*)malloc(index->config.threadCount * sizeof(LZ4Chunked_ThreadData));
    if (!threadData) {
        LZ4_freeIndex(index);
        return -ENOMEM;
    }

    pthread_t* threads = (pthread_t*)malloc(index->config.threadCount * sizeof(pthread_t));
    if (!threads) {
        free(threadData);
        LZ4_freeIndex(index);
        return -ENOMEM;
    }

    int globalError = 0;
    int currentChunk = 0;
    long long currentOffset = 0;
    long long outputOffset = 0;

    // Find the first unprocessed chunk
    for (int i = 0; i < index->totalChunks; i++) {
        if (index->chunks[i].state != LZ4CHUNKED_STATE_COMPLETED) {
            currentChunk = i;
            break;
        }
        currentOffset += index->config.chunkSize;
        outputOffset += index->chunks[i].compressedSize;
    }

    // Compress remaining chunks in parallel
    while (currentChunk < index->totalChunks && globalError == 0) {
        int threadsToUse = (index->totalChunks - currentChunk < index->config.threadCount) ? 
                           (index->totalChunks - currentChunk) : index->config.threadCount;

        // Initialize thread data
        for (int i = 0; i < threadsToUse; i++) {
            threadData[i].inputPath = inputPath;
            threadData[i].tempDir = index->config.tempDir;
            threadData[i].chunkOffset = currentOffset + i * index->config.chunkSize;
            threadData[i].chunkSize = (i == threadsToUse - 1 && currentOffset + i * index->config.chunkSize + index->config.chunkSize > index->fileSize) ? 
                                    (index->fileSize - currentOffset - i * index->config.chunkSize) : index->config.chunkSize;
            threadData[i].chunkId = currentChunk + i + 1;
            threadData[i].compressionLevel = index->config.compressionLevel;
            threadData[i].enableChecksum = index->config.enableChecksum;
            threadData[i].chunk = &index->chunks[currentChunk + i];
            threadData[i].error = &globalError;

            // Update chunk state to processing
            index->chunks[currentChunk + i].state = LZ4CHUNKED_STATE_PROCESSING;
        }

        // Start threads
        for (int i = 0; i < threadsToUse; i++) {
            if (pthread_create(&threads[i], NULL, compressChunk, &threadData[i]) != 0) {
                globalError = -errno;
                break;
            }
        }

        // Wait for threads to finish
        for (int i = 0; i < threadsToUse; i++) {
            pthread_join(threads[i], NULL);
        }

        // Update index and save
        if (globalError == 0) {
            // Calculate chunk offsets in output file
            for (int i = 0; i < threadsToUse; i++) {
                index->chunks[currentChunk + i].offset = outputOffset;
                outputOffset += index->chunks[currentChunk + i].compressedSize;
            }

            // Save updated index file
            int err = LZ4_saveIndex(idxPath, index);
            if (err != 0) {
                globalError = err;
                break;
            }

            currentChunk += threadsToUse;
            currentOffset += threadsToUse * index->config.chunkSize;
        }
    }

    // Cleanup
    free(threads);
    free(threadData);

    if (globalError != 0) {
        LZ4_freeIndex(index);
        return globalError;
    }

    // Merge temporary chunk files into final output file
    FILE* outputFile = fopen(outputPath, "ab");
    if (!outputFile) {
        LZ4_freeIndex(index);
        return -errno;
    }

    unsigned char buffer[1024 * 1024]; // 1MB buffer
    for (int i = 0; i < index->totalChunks; i++) {
        // Skip completed chunks
        if (index->chunks[i].state != LZ4CHUNKED_STATE_COMPLETED) continue;

        // Get chunk file name
        char chunkFileName[512];
        int err = getChunkFileName(index->config.tempDir, i + 1, chunkFileName, sizeof(chunkFileName));
        if (err != 0) {
            fclose(outputFile);
            LZ4_freeIndex(index);
            return err;
        }

        // Open chunk file
        FILE* chunkFile = fopen(chunkFileName, "rb");
        if (!chunkFile) {
            fclose(outputFile);
            LZ4_freeIndex(index);
            return -errno;
        }

        // Read and write chunk data
        size_t bytesRead;
        while ((bytesRead = fread(buffer, 1, sizeof(buffer), chunkFile)) > 0) {
            if (fwrite(buffer, 1, bytesRead, outputFile) != bytesRead) {
                fclose(chunkFile);
                fclose(outputFile);
                LZ4_freeIndex(index);
                return -EIO;
            }
        }

        fclose(chunkFile);

        // Delete temporary chunk file
        #ifdef _WIN32
        DeleteFile(chunkFileName);
        #else
        unlink(chunkFileName);
        #endif
    }

    fclose(outputFile);

    // Save final index file
    int err = LZ4_saveIndex(idxPath, index);
    if (err != 0) {
        LZ4_freeIndex(index);
        return err;
    }

    LZ4_freeIndex(index);
    return 0;
}

/**
 * LZ4_verifyChunk - Verify the integrity of a specific chunk
 */
int LZ4_verifyChunk(const char* inputPath, const char* idxPath, int chunkId) {
    if (!inputPath || !idxPath || chunkId <= 0) return -EINVAL;

    // Load index file
    LZ4Chunked_Index* index = LZ4_loadIndex(idxPath);
    if (!index) return -ENOENT;

    // Check if chunkId is valid
    if (chunkId > index->totalChunks) {
        LZ4_freeIndex(index);
        return -EINVAL;
    }

    // Get chunk information
    LZ4Chunked_Chunk* chunk = &index->chunks[chunkId - 1];
    if (chunk->state != LZ4CHUNKED_STATE_COMPLETED) {
        LZ4_freeIndex(index);
        return -ENOTREADY;
    }

    // Open input file
    FILE* inputFile = fopen(inputPath, "rb");
    if (!inputFile) {
        LZ4_freeIndex(index);
        return -errno;
    }

    // Seek to chunk offset
    if (fseek(inputFile, chunk->offset, SEEK_SET) != 0) {
        fclose(inputFile);
        LZ4_freeIndex(index);
        return -errno;
    }

    // Read compressed chunk data
    unsigned char* compressedData = (unsigned char*)malloc(chunk->compressedSize);
    if (!compressedData) {
        fclose(inputFile);
        LZ4_freeIndex(index);
        return -ENOMEM;
    }

    size_t bytesRead = fread(compressedData, 1, chunk->compressedSize, inputFile);
    if (bytesRead != chunk->compressedSize) {
        free(compressedData);
        fclose(inputFile);
        LZ4_freeIndex(index);
        return -EIO;
    }

    fclose(inputFile);

    // Decompress the chunk
    unsigned char* decompressedData = (unsigned char*)malloc(chunk->uncompressedSize);
    if (!decompressedData) {
        free(compressedData);
        LZ4_freeIndex(index);
        return -ENOMEM;
    }

    size_t decompressedSize = LZ4F_decompress(decompressedData, chunk->uncompressedSize, compressedData, chunk->compressedSize, NULL);
    if (LZ4F_isError(decompressedSize)) {
        free(decompressedData);
        free(compressedData);
        LZ4_freeIndex(index);
        return -EBADMSG;
    }

    // Verify checksum if enabled
    if (index->config.enableChecksum) {
        XXH32_hash_t computedChecksum = XXH32(decompressedData, decompressedSize, 0);
        if (computedChecksum != chunk->checksum) {
            free(decompressedData);
            free(compressedData);
            LZ4_freeIndex(index);
            return -EINVAL;
        }
    }

    // Cleanup
    free(decompressedData);
    free(compressedData);
    LZ4_freeIndex(index);

    return 0;
}