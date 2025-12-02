/*
 *  LZ4 - Fast LZ compression algorithm
 *  Copyright (C) 2011-2023, Yann Collet.
 *  BSD 2-Clause License (https://www.opensource.org/licenses/bsd-license.php).
 *  See LICENSE.txt for details.
 */

#define LZ4_HEAPMODE 0
#define LZ4_ACCELERATION_DEFAULT 1

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// 定义错误码
typedef enum {
    LZ4_STREAM_OK = 0,
    LZ4_STREAM_ERR_MEMORY,
    LZ4_STREAM_ERR_INVALID_INPUT,
    LZ4_STREAM_ERR_DECOMPRESSION_FAILED,
    LZ4_STREAM_ERR_MAX
} LZ4_stream_error_t;

// 定义错误消息大小
#define LZ4_STREAM_ERROR_MSG_SIZE 128

// 定义流状态
typedef enum {
    LZ4_STREAM_STATE_INITIALIZED,
    LZ4_STREAM_STATE_READY,
    LZ4_STREAM_STATE_PROCESSING,
    LZ4_STREAM_STATE_ERROR,
    LZ4_STREAM_STATE_FINISHED
} LZ4_stream_state_t;

// 定义内部流结构
typedef struct {
    LZ4_stream_state_t streamState;
    LZ4_stream_error_t lastErrorCode;
    char lastErrorMsg[LZ4_STREAM_ERROR_MSG_SIZE];
    size_t prefixSize;
    const uint8_t* prefixEnd;
    const uint8_t* externalDict;
    size_t extDictSize;
} LZ4_streamDecode_t_internal;

// 定义外部流结构
typedef struct {
    LZ4_streamDecode_t_internal internal_donotuse;
} LZ4_streamDecode_t;

// 定义压缩流结构
typedef struct {
    LZ4_streamDecode_t_internal internal_donotuse;
} LZ4_stream_t;

// 全局错误变量
LZ4_stream_error_t g_lastErrorCode = LZ4_STREAM_OK;
char g_lastErrorMsg[LZ4_STREAM_ERROR_MSG_SIZE] = {0};

// 辅助函数定义
#define LZ4_STATIC_ASSERT(c) typedef char LZ4_static_assert[(c) ? 1 : -1]
#define ALLOC_AND_ZERO(size) calloc(1, size)
#define FREEMEM(ptr) free(ptr)
#define DEBUGLOG(level, fmt, ...) ((void)0)

// 错误设置辅助函数
void LZ4_setError(LZ4_stream_error_t code, const char* msg) {
    g_lastErrorCode = code;
    if (msg != NULL) {
        strncpy(g_lastErrorMsg, msg, sizeof(g_lastErrorMsg));
    } else {
        memset(g_lastErrorMsg, 0, sizeof(g_lastErrorMsg));
    }
}

// 内存分配函数
void* LZ4_malloc(size_t size) {
    return malloc(size);
}

// 内存释放函数
void LZ4_free(void* ptr) {
    free(ptr);
}

// 原始LZ4_decompress_fast函数（简化版）
int LZ4_decompress_fast(const char* source, char* dest, int originalSize) {
    const uint8_t* src = (const uint8_t*)source;
    uint8_t* dst = (uint8_t*)dest;
    const uint8_t* end = dst + originalSize;

    while (dst < end) {
        int token = *src++;
        int literalLength = token >> 4;
        int matchLength = token & 0xF;

        // 复制字面量
        if (literalLength == 15) {
            int extra = 0;
            while (*src == 255) {
                extra += 255;
                src++;
            }
            extra += *src++;
            literalLength += extra;
        }
        memcpy(dst, src, literalLength);
        dst += literalLength;
        src += literalLength;

        // 处理匹配
        if (dst >= end) break;
        int offset = (*src++) | (*src++ << 8);
        const uint8_t* match = dst - offset;
        if (matchLength == 15) {
            int extra = 0;
            while (*src == 255) {
                extra += 255;
                src++;
            }
            extra += *src++;
            matchLength += extra;
        }
        matchLength += 4;

        // 复制匹配数据
        while (matchLength-- > 0 && dst < end) {
            *dst++ = *match++;
        }
    }

    return src - (const uint8_t*)source;
}

// LZ4_decompress_unsafe_generic函数（简化版）
int LZ4_decompress_unsafe_generic(const uint8_t* src, uint8_t* dst, int originalSize,
                                     size_t prefixSize, const uint8_t* externalDict, size_t extDictSize) {
    // 简化实现，直接调用LZ4_decompress_fast
    return LZ4_decompress_fast((const char*)src, (char*)dst, originalSize);
}

// LZ4_decompress_fast_extDict函数（简化版）
int LZ4_decompress_fast_extDict(const char* source, char* dest, int originalSize,
                                   const char* dictionary, int dictSize) {
    // 简化实现，直接调用LZ4_decompress_fast
    return LZ4_decompress_fast(source, dest, originalSize);
}

// 增强LZ4_createStream函数
LZ4_stream_t* LZ4_createStream(void) {
    LZ4_stream_t* lz4s = (LZ4_stream_t*)ALLOC_AND_ZERO(sizeof(LZ4_stream_t));
    LZ4_STATIC_ASSERT(sizeof(LZ4_stream_t) >= sizeof(LZ4_stream_t_internal));

    if (lz4s == NULL) {
        LZ4_setError(LZ4_STREAM_ERR_MEMORY, "Failed to allocate memory for LZ4_stream_t");
        return NULL;
    }

    // 初始化新的状态和错误字段
    lz4s->internal_donotuse.streamState = LZ4_STREAM_STATE_INITIALIZED;
    lz4s->internal_donotuse.lastErrorCode = LZ4_STREAM_OK;
    memset(lz4s->internal_donotuse.lastErrorMsg, 0, sizeof(lz4s->internal_donotuse.lastErrorMsg));

    return lz4s;
}

// 增强LZ4_createStreamDecode函数
LZ4_streamDecode_t* LZ4_createStreamDecode(void) {
    LZ4_streamDecode_t* lz4sd = (LZ4_streamDecode_t*)ALLOC_AND_ZERO(sizeof(LZ4_streamDecode_t));
    LZ4_STATIC_ASSERT(sizeof(LZ4_streamDecode_t) >= sizeof(LZ4_streamDecode_t_internal));

    if (lz4sd == NULL) {
        LZ4_setError(LZ4_STREAM_ERR_MEMORY, "Failed to allocate memory for LZ4_streamDecode_t");
        return NULL;
    }

    // 初始化新的状态和错误字段
    lz4sd->internal_donotuse.streamState = LZ4_STREAM_STATE_INITIALIZED;
    lz4sd->internal_donotuse.lastErrorCode = LZ4_STREAM_OK;
    memset(lz4sd->internal_donotuse.lastErrorMsg, 0, sizeof(lz4sd->internal_donotuse.lastErrorMsg));

    return lz4sd;
}

// 增强LZ4_setStreamDecode函数
int LZ4_setStreamDecode(LZ4_streamDecode_t* LZ4_streamDecode, const char* dictionary, int dictSize) {
    if (LZ4_streamDecode == NULL) {
        LZ4_setError(LZ4_STREAM_ERR_INVALID_INPUT, "Invalid input parameters (NULL stream)");
        return 0;
    }

    LZ4_streamDecode_t_internal* lz4sd = &LZ4_streamDecode->internal_donotuse;

    // 边界检查增强
    if (dictSize < 0) {
        LZ4_setError(LZ4_STREAM_ERR_INVALID_INPUT, "Invalid dictionary size (must be non-negative)");
        lz4sd->streamState = LZ4_STREAM_STATE_ERROR;
        lz4sd->lastErrorCode = LZ4_STREAM_ERR_INVALID_INPUT;
        strncpy(lz4sd->lastErrorMsg, "Invalid dictionary size (must be non-negative)", sizeof(lz4sd->lastErrorMsg));
        return 0;
    }

    if (dictSize > 0 && dictionary == NULL) {
        LZ4_setError(LZ4_STREAM_ERR_INVALID_INPUT, "Invalid dictionary pointer (NULL with non-zero size)");
        lz4sd->streamState = LZ4_STREAM_STATE_ERROR;
        lz4sd->lastErrorCode = LZ4_STREAM_ERR_INVALID_INPUT;
        strncpy(lz4sd->lastErrorMsg, "Invalid dictionary pointer (NULL with non-zero size)", sizeof(lz4sd->lastErrorMsg));
        return 0;
    }

    lz4sd->prefixSize = (size_t)dictSize;
    if (dictSize) {
        lz4sd->prefixEnd = (const uint8_t*)dictionary + dictSize;
    } else {
        lz4sd->prefixEnd = (const uint8_t*)dictionary;
    }
    lz4sd->externalDict = NULL;
    lz4sd->extDictSize = 0;

    // 状态更新
    lz4sd->streamState = LZ4_STREAM_STATE_READY;
    lz4sd->lastErrorCode = LZ4_STREAM_OK;
    memset(lz4sd->lastErrorMsg, 0, sizeof(lz4sd->lastErrorMsg));

    return 1;
}

// 增强LZ4_decompress_fast_continue函数
int LZ4_decompress_fast_continue(LZ4_streamDecode_t* LZ4_streamDecode, const char* source, char* dest, int originalSize) {
    if (LZ4_streamDecode == NULL || source == NULL || dest == NULL) {
        LZ4_setError(LZ4_STREAM_ERR_INVALID_INPUT, "Invalid input parameters (NULL pointer)");
        if (LZ4_streamDecode != NULL) {
            LZ4_streamDecode_t_internal* lz4sd = &LZ4_streamDecode->internal_donotuse;
            lz4sd->streamState = LZ4_STREAM_STATE_ERROR;
            lz4sd->lastErrorCode = LZ4_STREAM_ERR_INVALID_INPUT;
            strncpy(lz4sd->lastErrorMsg, "Invalid input parameters (NULL pointer)", sizeof(lz4sd->lastErrorMsg));
        }
        return -1;
    }

    if (originalSize <= 0) {
        LZ4_setError(LZ4_STREAM_ERR_INVALID_INPUT, "Invalid input size (must be positive)");
        LZ4_streamDecode_t_internal* lz4sd = &LZ4_streamDecode->internal_donotuse;
        lz4sd->streamState = LZ4_STREAM_STATE_ERROR;
        lz4sd->lastErrorCode = LZ4_STREAM_ERR_INVALID_INPUT;
        strncpy(lz4sd->lastErrorMsg, "Invalid input size (must be positive)", sizeof(lz4sd->lastErrorMsg));
        return -1;
    }

    LZ4_streamDecode_t_internal* lz4sd = &LZ4_streamDecode->internal_donotuse;
    lz4sd->streamState = LZ4_STREAM_STATE_PROCESSING;

    int result;
    if (lz4sd->prefixSize == 0) {
        result = LZ4_decompress_fast(source, dest, originalSize);
        if (result <= 0) {
            LZ4_setError(LZ4_STREAM_ERR_DECOMPRESSION_FAILED, "Decompression failed (invalid or corrupted data)");
            lz4sd->streamState = LZ4_STREAM_STATE_ERROR;
            lz4sd->lastErrorCode = LZ4_STREAM_ERR_DECOMPRESSION_FAILED;
            strncpy(lz4sd->lastErrorMsg, "Decompression failed (invalid or corrupted data)", sizeof(lz4sd->lastErrorMsg));
            return result;
        }
        lz4sd->prefixSize = (size_t)originalSize;
        lz4sd->prefixEnd = (uint8_t*)dest + originalSize;
    } else if (lz4sd->prefixEnd == (uint8_t*)dest) {
        result = LZ4_decompress_unsafe_generic(
                        (const uint8_t*)source, (uint8_t*)dest, originalSize,
                        lz4sd->prefixSize,
                        lz4sd->externalDict, lz4sd->extDictSize);
        if (result <= 0) {
            LZ4_setError(LZ4_STREAM_ERR_DECOMPRESSION_FAILED, "Decompression failed (invalid or corrupted data)");
            lz4sd->streamState = LZ4_STREAM_STATE_ERROR;
            lz4sd->lastErrorCode = LZ4_STREAM_ERR_DECOMPRESSION_FAILED;
            strncpy(lz4sd->lastErrorMsg, "Decompression failed (invalid or corrupted data)", sizeof(lz4sd->lastErrorMsg));
            return result;
        }
        lz4sd->prefixSize += (size_t)originalSize;
        lz4sd->prefixEnd += originalSize;
    } else {
        lz4sd->extDictSize = lz4sd->prefixSize;
        lz4sd->externalDict = lz4sd->prefixEnd - lz4sd->extDictSize;
        result = LZ4_decompress_fast_extDict(source, dest, originalSize,
                                             (const char*)lz4sd->externalDict, (int)lz4sd->extDictSize);
        if (result <= 0) {
            LZ4_setError(LZ4_STREAM_ERR_DECOMPRESSION_FAILED, "Decompression failed (invalid or corrupted data)");
            lz4sd->streamState = LZ4_STREAM_STATE_ERROR;
            lz4sd->lastErrorCode = LZ4_STREAM_ERR_DECOMPRESSION_FAILED;
            strncpy(lz4sd->lastErrorMsg, "Decompression failed (invalid or corrupted data)", sizeof(lz4sd->lastErrorMsg));
            return result;
        }
        lz4sd->prefixSize = (size_t)originalSize;
        lz4sd->prefixEnd = (uint8_t*)dest + originalSize;
    }

    lz4sd->streamState = LZ4_STREAM_STATE_READY;
    lz4sd->lastErrorCode = LZ4_STREAM_OK;
    memset(lz4sd->lastErrorMsg, 0, sizeof(lz4sd->lastErrorMsg));

    return result;
}

// LZ4_freeStream函数
void LZ4_freeStream(LZ4_stream_t* LZ4_stream) {
    if (LZ4_stream != NULL) {
        LZ4_free(LZ4_stream);
    }
}

// LZ4_freeStreamDecode函数
void LZ4_freeStreamDecode(LZ4_streamDecode_t* LZ4_streamDecode) {
    if (LZ4_streamDecode != NULL) {
        LZ4_free(LZ4_streamDecode);
    }
}

#endif   /* LZ4_COMMONDEFS_ONLY */