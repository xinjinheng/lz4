/*
 * LZ4 Chunked Compression with Resumable Support
 * Header File
 * Copyright (c) 2024. All rights reserved.
 *
 * This module provides chunked compression and decompression for large files,
 * with support for resuming interrupted operations.
 */

#if defined (__cplusplus)
extern "C" {
#endif

#ifndef LZ4CHUNKED_H_837465029837456
#define LZ4CHUNKED_H_837465029837456

#include <stddef.h>   /* size_t */
#include <time.h>     /* time_t */

/* --- Dependency --- */
#include "lz4frame.h"  /* LZ4 frame format */
#include "xxhash.h"    /* XXH32 checksum */


/* ************************************
 *  Constants
 ************************************/ 

#define LZ4CHUNKED_MAGIC "LZ4IDX"    /* Index file magic number */
#define LZ4CHUNKED_VERSION 1         /* Current version */

/* Default chunk sizes */
#define LZ4CHUNKED_DEFAULT_CHUNK_SIZE (100 * 1024 * 1024) /* 100MB */
#define LZ4CHUNKED_TEXT_CHUNK_SIZE (50 * 1024 * 1024)    /* 50MB */
#define LZ4CHUNKED_BINARY_CHUNK_SIZE (200 * 1024 * 1024) /* 200MB */

/* Default thread count (use number of CPU cores) */
#define LZ4CHUNKED_DEFAULT_THREADS 0


/* ************************************
 *  Enums
 ************************************/ 

/* Chunk state */
typedef enum {
    LZ4CHUNKED_STATE_UNPROCESSED = 0,
    LZ4CHUNKED_STATE_PROCESSING = 1,
    LZ4CHUNKED_STATE_COMPLETED = 2
} LZ4Chunked_State;


/* ************************************
 *  Data Structures
 ************************************/ 

/* Chunk configuration */
typedef struct {
    size_t chunkSize;          /* Chunk size in bytes */
    int compressionLevel;      /* LZ4 compression level (0-12) */
    int threadCount;           /* Number of threads to use (0 = auto-detect) */
    int enableChecksum;        /* Whether to enable XXH32 checksum (0 = disable, 1 = enable) */
    const char* tempDir;       /* Temporary directory for intermediate files (NULL = use system temp) */
} LZ4Chunked_Config;

/* Chunk metadata */
typedef struct {
    int chunkId;               /* Chunk ID (starting from 1) */
    size_t uncompressedSize;   /* Size of the chunk before compression */
    size_t compressedSize;     /* Size of the chunk after compression */
    long long offset;          /* Offset of the chunk in the compressed file */
    XXH32_hash_t checksum;     /* XXH32 checksum of the uncompressed chunk */
    LZ4Chunked_State state;    /* Current state of the chunk */
} LZ4Chunked_Chunk;

/* Index file structure */
typedef struct {
    char magic[6];             /* Magic number "LZ4IDX" */
    int version;               /* Version number */
    int totalChunks;           /* Total number of chunks */
    long long fileSize;        /* Total size of the original file */
    time_t createTime;         /* Creation time of the index file */
    LZ4Chunked_Config config;  /* Configuration used for compression */
    LZ4Chunked_Chunk* chunks;  /* Array of chunk metadata */
} LZ4Chunked_Index;


/* ************************************
 *  Public API
 ************************************/ 

/**
 * LZ4_chunkedCompress - Compress a large file using chunked LZ4 compression
 * 
 * @param inputPath  Path to the input file
 * @param outputPath Path to the output file (will be created)
 * @param config     Compression configuration (NULL = use default)
 * 
 * @return 0 on success, negative error code on failure
 */
int LZ4_chunkedCompress(const char* inputPath, const char* outputPath, const LZ4Chunked_Config* config);

/**
 * LZ4_chunkedDecompress - Decompress a chunked LZ4 file
 * 
 * @param inputPath Path to the compressed file
 * @param idxPath   Path to the index file (.lz4idx)
 * @param outputPath Path to the output file (will be created)
 * 
 * @return 0 on success, negative error code on failure
 */
int LZ4_chunkedDecompress(const char* inputPath, const char* idxPath, const char* outputPath);

/**
 * LZ4_resumeCompress - Resume an interrupted chunked compression
 * 
 * @param idxPath Path to the index file (.lz4idx)
 * 
 * @return 0 on success, negative error code on failure
 */
int LZ4_resumeCompress(const char* idxPath);

/**
 * LZ4_verifyChunk - Verify the integrity of a specific chunk
 * 
 * @param inputPath Path to the compressed file
 * @param idxPath   Path to the index file (.lz4idx)
 * @param chunkId   Chunk ID to verify (starting from 1)
 * 
 * @return 0 on success, negative error code on failure
 */
int LZ4_verifyChunk(const char* inputPath, const char* idxPath, int chunkId);

/**
 * LZ4_createDefaultConfig - Create a default configuration
 * 
 * @return Pointer to default configuration (must be freed by caller)
 */
LZ4Chunked_Config* LZ4_createDefaultConfig(void);

/**
 * LZ4_freeConfig - Free a configuration
 * 
 * @param config Pointer to configuration to free
 */
void LZ4_freeConfig(LZ4Chunked_Config* config);

/**
 * LZ4_loadIndex - Load an index file from disk
 * 
 * @param idxPath Path to the index file (.lz4idx)
 * 
 * @return Pointer to index structure (must be freed by caller), NULL on failure
 */
LZ4Chunked_Index* LZ4_loadIndex(const char* idxPath);

/**
 * LZ4_saveIndex - Save an index file to disk
 * 
 * @param idxPath Path to the index file (.lz4idx)
 * @param index   Pointer to index structure
 * 
 * @return 0 on success, negative error code on failure
 */
int LZ4_saveIndex(const char* idxPath, const LZ4Chunked_Index* index);

/**
 * LZ4_freeIndex - Free an index structure
 * 
 * @param index Pointer to index structure to free
 */
void LZ4_freeIndex(LZ4Chunked_Index* index);


#endif /* LZ4CHUNKED_H_837465029837456 */

#if defined (__cplusplus)
}
#endif