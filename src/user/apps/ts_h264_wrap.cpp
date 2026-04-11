/*
 * ts_h264_wrap.cpp -- C-callable wrapper around OpenH264 decoder
 *
 * Compiled with x86_64-elf-g++.  Exposes a plain C API via extern "C".
 * All OpenH264 C++ interaction is confined to this file.
 */

#include "ts_h264_wrap.h"
#include <codec_api.h>
#include <codec_def.h>
#include <codec_app_def.h>
#include <string.h>

extern "C" {

ts_h264_decoder ts_h264_create(void) {
    ISVCDecoder *dec = nullptr;
    if (WelsCreateDecoder(&dec) != 0 || !dec)
        return nullptr;

    SDecodingParam param;
    memset(&param, 0, sizeof(param));
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    param.eEcActiveIdc = ERROR_CON_SLICE_COPY;
    param.bParseOnly = false;

    if (dec->Initialize(&param) != 0) {
        WelsDestroyDecoder(dec);
        return nullptr;
    }

    return (ts_h264_decoder)dec;
}

int ts_h264_decode(ts_h264_decoder handle,
                   const uint8_t *data, int len,
                   uint8_t **y, uint8_t **u, uint8_t **v,
                   int *width, int *height,
                   int *stride_y, int *stride_uv) {
    if (!handle) return -1;
    ISVCDecoder *dec = (ISVCDecoder *)handle;

    unsigned char *dst[3] = {0, 0, 0};
    SBufferInfo info;
    memset(&info, 0, sizeof(info));

    DECODING_STATE state = dec->DecodeFrameNoDelay(data, (int)len, dst, &info);
    if (state != dsErrorFree && state != dsFramePending)
        return -1;

    if (info.iBufferStatus == 1) {
        *y = info.pDst[0] ? info.pDst[0] : dst[0];
        *u = info.pDst[1] ? info.pDst[1] : dst[1];
        *v = info.pDst[2] ? info.pDst[2] : dst[2];
        *width    = info.UsrData.sSystemBuffer.iWidth;
        *height   = info.UsrData.sSystemBuffer.iHeight;
        *stride_y = info.UsrData.sSystemBuffer.iStride[0];
        *stride_uv = info.UsrData.sSystemBuffer.iStride[1];
        return 1;
    }

    return 0;
}

int ts_h264_flush(ts_h264_decoder handle,
                  uint8_t **y, uint8_t **u, uint8_t **v,
                  int *width, int *height,
                  int *stride_y, int *stride_uv) {
    if (!handle) return -1;
    ISVCDecoder *dec = (ISVCDecoder *)handle;

    unsigned char *dst[3] = {0, 0, 0};
    SBufferInfo info;
    memset(&info, 0, sizeof(info));

    DECODING_STATE state = dec->FlushFrame(dst, &info);
    if (state != dsErrorFree)
        return -1;

    if (info.iBufferStatus == 1) {
        *y = info.pDst[0] ? info.pDst[0] : dst[0];
        *u = info.pDst[1] ? info.pDst[1] : dst[1];
        *v = info.pDst[2] ? info.pDst[2] : dst[2];
        *width    = info.UsrData.sSystemBuffer.iWidth;
        *height   = info.UsrData.sSystemBuffer.iHeight;
        *stride_y = info.UsrData.sSystemBuffer.iStride[0];
        *stride_uv = info.UsrData.sSystemBuffer.iStride[1];
        return 1;
    }

    return 0;
}

void ts_h264_destroy(ts_h264_decoder handle) {
    if (!handle) return;
    ISVCDecoder *dec = (ISVCDecoder *)handle;
    dec->Uninitialize();
    WelsDestroyDecoder(dec);
}

} /* extern "C" */
