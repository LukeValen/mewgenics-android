/*
 * hle_libs.c - host implementations of hot library code the game links statically.
 *
 * tinfl_decompress (miniz, 0x140B8FF30): inflates every zlib-compressed SWF bitmap at start-up
 * (1815 of them); lifted it is the single hottest function. The guest's mz_inflate keeps the
 * tinfl_decompressor inside its own state block and touches only m_state (tinfl_init) and
 * m_check_adler32, which sit at the same offsets in every miniz version, so the host decoder
 * can own the rest of the structure.
 */
#define MINIZ_NO_STDIO
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_TIME
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "../../third_party/miniz/miniz.c"
#include "w64.h"
#include <stddef.h>
#define STB_DXT_IMPLEMENTATION
#include "../../third_party/stb/stb_dxt.h"

void w64_add_override(uint64_t va, w64_fn fn, const char *name);

_Static_assert(sizeof(tinfl_decompressor) <= 0x2af8, "host tinfl_decompressor must fit the guest's slot");
_Static_assert(offsetof(tinfl_decompressor, m_check_adler32) == 0x1c, "m_check_adler32 offset");

/* tinfl_status tinfl_decompress(tinfl_decompressor *r, const mz_uint8 *pIn_buf_next, size_t *pIn_buf_size,
 *     mz_uint8 *pOut_buf_start, mz_uint8 *pOut_buf_next, size_t *pOut_buf_size, const mz_uint32 decomp_flags) */
static void hle_tinfl_decompress(CPU *c)
{
    tinfl_status st = tinfl_decompress((tinfl_decompressor *)AP(0), (const mz_uint8 *)AP(1), (size_t *)AP(2),
                                       (mz_uint8 *)AP(3), (mz_uint8 *)AP(4), (size_t *)AP(5), AU(6));
    RET((uint32_t)(int32_t)st);
}

/* 0x140A24760: DXT5-compress a w x h RGBA image (stb_dxt inlined by the game's atlas code).
 * rcx = dest, rdx = src (w*4 bytes per row), r8d = w, r9d = h; blocks row-major, 16 bytes each,
 * a block row in dest is w*4 bytes. The game calls the colour block with mode left in r8 from the
 * previous call; the normal mode is used here. */
static void hle_dxt5_image(CPU *c)
{
    uint8_t *dst = AP(0);
    const uint8_t *src = AP(1);
    int w = AI(2), h = AI(3);
    if (h > 0) {
        int bh = (h - 1) / 4 + 1, bw = w > 0 ? (w - 1) / 4 + 1 : 0;
        size_t stride = (size_t)w * 4;
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                uint8_t blk[64];
                const uint8_t *s = src + (size_t)by * 4 * stride + (size_t)bx * 16;
                for (int r = 0; r < 4; r++) memcpy(blk + 16 * r, s + (size_t)r * stride, 16);
                stb_compress_dxt_block(dst + (size_t)by * stride + (size_t)bx * 16, blk, 1, STB_DXT_NORMAL);
            }
    }
    RETV();
}

__attribute__((constructor)) static void hle_libs_init(void)
{
    w64_add_override(0x140A24760ull, hle_dxt5_image, "DXT5 image compressor (stb_dxt)");
    w64_add_override(0x140B8FF30ull, hle_tinfl_decompress, "tinfl_decompress (miniz)");
}
