// sha256.h — minimal self-contained SHA-256 for disc image verification.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} m2_sha256_ctx;

void m2_sha256_init(m2_sha256_ctx* ctx);
void m2_sha256_update(m2_sha256_ctx* ctx, const void* data, size_t len);
void m2_sha256_final(m2_sha256_ctx* ctx, uint8_t digest[32]);
int m2_sha256_file(const char* path, uint8_t digest[32], uint64_t* size_out);

#ifdef __cplusplus
}
#endif
