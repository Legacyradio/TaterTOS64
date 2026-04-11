/*
 * ts_h264_wrap.h -- Clean C API for OpenH264 decoder
 *
 * Wraps the C++ ISVCDecoder interface into plain C functions
 * so tatersurf.c can decode H.264 without touching C++ types.
 */

#ifndef TS_H264_WRAP_H
#define TS_H264_WRAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *ts_h264_decoder;

/* Create and initialize an H.264 decoder.  Returns NULL on failure. */
ts_h264_decoder ts_h264_create(void);

/*
 * Feed a NAL unit to the decoder.
 * data/len: raw NAL unit (no start code prefix needed, but ok if present).
 * On success with a decoded frame: returns 1 and fills output params.
 * On success without a frame yet: returns 0.
 * On error: returns -1.
 *
 * The Y/U/V pointers are owned by the decoder and valid until the
 * next ts_h264_decode() call.
 */
int ts_h264_decode(ts_h264_decoder dec,
                   const uint8_t *data, int len,
                   uint8_t **y, uint8_t **u, uint8_t **v,
                   int *width, int *height,
                   int *stride_y, int *stride_uv);

/* Flush any remaining frames.  Same output semantics as ts_h264_decode. */
int ts_h264_flush(ts_h264_decoder dec,
                  uint8_t **y, uint8_t **u, uint8_t **v,
                  int *width, int *height,
                  int *stride_y, int *stride_uv);

/* Destroy the decoder and free all resources. */
void ts_h264_destroy(ts_h264_decoder dec);

#ifdef __cplusplus
}
#endif

#endif /* TS_H264_WRAP_H */
