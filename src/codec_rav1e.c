// Copyright 2019 Joe Drago. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#include "avif/internal.h"

#include "rav1e.h"

#include <string.h>

struct avifCodecInternal
{
    RaContext * rav1eContext;
    RaChromaSampling chromaSampling;
    int yShift;
    uint32_t encodeWidth;
    uint32_t encodeHeight;
};

static void rav1eCodecDestroyInternal(avifCodec * codec)
{
    if (codec->internal->rav1eContext) {
        rav1e_context_unref(codec->internal->rav1eContext);
        codec->internal->rav1eContext = NULL;
    }
    avifFree(codec->internal);
}

// Official support wasn't added until v0.4.0
static avifBool rav1eSupports400(void)
{
    const char * rav1eVersionString = rav1e_version_short();

    // Check major version > 0
    int majorVersion = atoi(rav1eVersionString);
    if (majorVersion > 0) {
        return AVIF_TRUE;
    }

    // Check minor version >= 4
    const char * minorVersionString = strchr(rav1eVersionString, '.');
    if (!minorVersionString) {
        return AVIF_FALSE;
    }
    ++minorVersionString;
    if (!(*minorVersionString)) {
        return AVIF_FALSE;
    }
    int minorVersion = atoi(minorVersionString);
    return minorVersion >= 4;
}

static avifResult rav1eCodecEncodeImage(avifCodec * codec,
                                        avifEncoder * encoder,
                                        const avifImage * image,
                                        avifBool alpha,
                                        int tileRowsLog2,
                                        int tileColsLog2,
                                        int quantizer,
                                        avifEncoderChanges encoderChanges,
                                        avifBool disableLaggedOutput,
                                        uint32_t addImageFlags,
                                        avifCodecEncodeOutput * output)
{
    // rav1e does not support changing encoder settings.
    if (encoderChanges) {
        return AVIF_RESULT_NOT_IMPLEMENTED;
    }

    // rav1e does not support changing image dimensions.
    if (!codec->internal->rav1eContext) {
        codec->internal->encodeWidth = image->width;
        codec->internal->encodeHeight = image->height;
    } else if ((codec->internal->encodeWidth != image->width) || (codec->internal->encodeHeight != image->height)) {
        return AVIF_RESULT_NOT_IMPLEMENTED;
    }

    // rav1e does not support encoding layered image.
    if (encoder->extraLayerCount > 0) {
        return AVIF_RESULT_NOT_IMPLEMENTED;
    }

    // rav1e does not support disabling lagged output. See https://github.com/xiph/rav1e/issues/2267. Ignore this setting.
    (void)disableLaggedOutput;

    avifResult result = AVIF_RESULT_UNKNOWN_ERROR;

    RaConfig * rav1eConfig = NULL;
    RaFrame * rav1eFrame = NULL;

    if (!codec->internal->rav1eContext) {
        const avifBool supports400 = rav1eSupports400();
        RaPixelRange rav1eRange;
        if (alpha) {
            // AV1-AVIF specification, Section 4 "Auxiliary Image Items and Sequences":
            //   The color_range field in the Sequence Header OBU shall be set to 1.
            rav1eRange = RA_PIXEL_RANGE_FULL;

            // AV1-AVIF specification, Section 4 "Auxiliary Image Items and Sequences":
            //   The mono_chrome field in the Sequence Header OBU shall be set to 1.
            // Some encoders do not support 4:0:0 and encode alpha as 4:2:0 so it is not always respected.
            codec->internal->chromaSampling = supports400 ? RA_CHROMA_SAMPLING_CS400 : RA_CHROMA_SAMPLING_CS420;
            codec->internal->yShift = 1;

            // CICP (CP/TC/MC) does not apply to the alpha auxiliary image.
            // Use Unspecified (2) colour primaries, transfer characteristics, and matrix coefficients below.
        } else {
            // AV1-ISOBMFF specification, Section 2.3.4:
            //   The value of full_range_flag in the 'colr' box SHALL match the color_range
            //   flag in the Sequence Header OBU.
            rav1eRange = (image->yuvRange == AVIF_RANGE_FULL) ? RA_PIXEL_RANGE_FULL : RA_PIXEL_RANGE_LIMITED;

            // AV1-AVIF specification, Section 2.2.1. "AV1 Item Configuration Property":
            //   The values of the fields in the AV1CodecConfigurationBox shall match those
            //   of the Sequence Header OBU in the AV1 Image Item Data.
            codec->internal->yShift = 0;
            switch (image->yuvFormat) {
                case AVIF_PIXEL_FORMAT_YUV444:
                    codec->internal->chromaSampling = RA_CHROMA_SAMPLING_CS444;
                    break;
                case AVIF_PIXEL_FORMAT_YUV422:
                    codec->internal->chromaSampling = RA_CHROMA_SAMPLING_CS422;
                    break;
                case AVIF_PIXEL_FORMAT_YUV420:
                    codec->internal->chromaSampling = RA_CHROMA_SAMPLING_CS420;
                    codec->internal->yShift = 1;
                    break;
                case AVIF_PIXEL_FORMAT_YUV400:
                    codec->internal->chromaSampling = supports400 ? RA_CHROMA_SAMPLING_CS400 : RA_CHROMA_SAMPLING_CS420;
                    codec->internal->yShift = 1;
                    break;
                case AVIF_PIXEL_FORMAT_NONE:
                case AVIF_PIXEL_FORMAT_COUNT:
                default:
                    return AVIF_RESULT_UNKNOWN_ERROR;
            }
        }

        rav1eConfig = rav1e_config_default();
        if (rav1e_config_set_pixel_format(rav1eConfig,
                                          (uint8_t)image->depth,
                                          codec->internal->chromaSampling,
                                          (RaChromaSamplePosition)image->yuvChromaSamplePosition,
                                          rav1eRange) < 0) {
            goto cleanup;
        }

        if (addImageFlags & AVIF_ADD_IMAGE_FLAG_SINGLE) {
            if (rav1e_config_parse(rav1eConfig, "still_picture", "true") == -1) {
                goto cleanup;
            }
        }
        if (rav1e_config_parse_int(rav1eConfig, "width", image->width) == -1) {
            goto cleanup;
        }
        if (rav1e_config_parse_int(rav1eConfig, "height", image->height) == -1) {
            goto cleanup;
        }
        if (rav1e_config_parse_int(rav1eConfig, "threads", encoder->maxThreads) == -1) {
            goto cleanup;
        }

        int minQuantizer = AVIF_CLAMP(encoder->minQuantizer, 0, 63);
        if (alpha) {
            minQuantizer = AVIF_CLAMP(encoder->minQuantizerAlpha, 0, 63);
        }
        minQuantizer = (minQuantizer * 255) / 63; // Rescale quantizer values as rav1e's QP range is [0,255]
        quantizer = (quantizer * 255) / 63;
        if (rav1e_config_parse_int(rav1eConfig, "min_quantizer", minQuantizer) == -1) {
            goto cleanup;
        }
        if (rav1e_config_parse_int(rav1eConfig, "quantizer", quantizer) == -1) {
            goto cleanup;
        }
        if (tileRowsLog2 != 0) {
            if (rav1e_config_parse_int(rav1eConfig, "tile_rows", 1 << tileRowsLog2) == -1) {
                goto cleanup;
            }
        }
        if (tileColsLog2 != 0) {
            if (rav1e_config_parse_int(rav1eConfig, "tile_cols", 1 << tileColsLog2) == -1) {
                goto cleanup;
            }
        }
        if (encoder->speed != AVIF_SPEED_DEFAULT) {
            int speed = AVIF_CLAMP(encoder->speed, 0, 10);
            if (rav1e_config_parse_int(rav1eConfig, "speed", speed) == -1) {
                goto cleanup;
            }
        }
        if (encoder->keyframeInterval > 0) {
            // "key_frame_interval" is the maximum interval between two keyframes.
            if (rav1e_config_parse_int(rav1eConfig, "key_frame_interval", encoder->keyframeInterval) == -1) {
                goto cleanup;
            }
        }
        for (uint32_t i = 0; i < codec->csOptions->count; ++i) {
            avifCodecSpecificOption * entry = &codec->csOptions->entries[i];
            if (rav1e_config_parse(rav1eConfig, entry->key, entry->value) < 0) {
                avifDiagnosticsPrintf(codec->diag, "Invalid value for %s: %s.", entry->key, entry->value);
                result = AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
                goto cleanup;
            }
        }

        // Section 2.3.4 of AV1-ISOBMFF says 'colr' with 'nclx' should be present and shall match CICP
        // values in the Sequence Header OBU, unless the latter has 2/2/2 (Unspecified).
        // So set CICP values to 2/2/2 (Unspecified) in the Sequence Header OBU for simplicity.
        // It may also save 3 bytes since the AV1 encoder may set color_description_present_flag to 0
        // (see Section 5.5.2 "Color config syntax" of the AV1 specification).
        rav1e_config_set_color_description(rav1eConfig,
                                           RA_MATRIX_COEFFICIENTS_UNSPECIFIED,
                                           RA_COLOR_PRIMARIES_UNSPECIFIED,
                                           RA_TRANSFER_CHARACTERISTICS_UNSPECIFIED);

        codec->internal->rav1eContext = rav1e_context_new(rav1eConfig);
        if (!codec->internal->rav1eContext) {
            goto cleanup;
        }
    }

    rav1eFrame = rav1e_frame_new(codec->internal->rav1eContext);

    int byteWidth = (image->depth > 8) ? 2 : 1;
    if (alpha) {
        rav1e_frame_fill_plane(rav1eFrame, 0, image->alphaPlane, (size_t)image->alphaRowBytes * image->height, image->alphaRowBytes, byteWidth);
    } else {
        rav1e_frame_fill_plane(rav1eFrame, 0, image->yuvPlanes[0], (size_t)image->yuvRowBytes[0] * image->height, image->yuvRowBytes[0], byteWidth);
        if (image->yuvFormat != AVIF_PIXEL_FORMAT_YUV400) {
            uint32_t uvHeight = (image->height + codec->internal->yShift) >> codec->internal->yShift;
            rav1e_frame_fill_plane(rav1eFrame, 1, image->yuvPlanes[1], (size_t)image->yuvRowBytes[1] * uvHeight, image->yuvRowBytes[1], byteWidth);
            rav1e_frame_fill_plane(rav1eFrame, 2, image->yuvPlanes[2], (size_t)image->yuvRowBytes[2] * uvHeight, image->yuvRowBytes[2], byteWidth);
        }
    }

    RaFrameTypeOverride frameType = RA_FRAME_TYPE_OVERRIDE_NO;
    if (addImageFlags & AVIF_ADD_IMAGE_FLAG_FORCE_KEYFRAME) {
        frameType = RA_FRAME_TYPE_OVERRIDE_KEY;
    }
    rav1e_frame_set_type(rav1eFrame, frameType);

    RaEncoderStatus encoderStatus = rav1e_send_frame(codec->internal->rav1eContext, rav1eFrame);
    if (encoderStatus != RA_ENCODER_STATUS_SUCCESS) {
        goto cleanup;
    }

    RaPacket * pkt = NULL;
    for (;;) {
        encoderStatus = rav1e_receive_packet(codec->internal->rav1eContext, &pkt);
        if (encoderStatus == RA_ENCODER_STATUS_ENCODED) {
            continue;
        }
        if ((encoderStatus != RA_ENCODER_STATUS_SUCCESS) && (encoderStatus != RA_ENCODER_STATUS_NEED_MORE_DATA)) {
            goto cleanup;
        } else if (pkt) {
            if (pkt->data && (pkt->len > 0)) {
                result = avifCodecEncodeOutputAddSample(output, pkt->data, pkt->len, (pkt->frame_type == RA_FRAME_TYPE_KEY));
                if (result != AVIF_RESULT_OK) {
                    goto cleanup;
                }
            }
            rav1e_packet_unref(pkt);
            pkt = NULL;
        } else {
            break;
        }
    }
    result = AVIF_RESULT_OK;
cleanup:
    if (rav1eFrame) {
        rav1e_frame_unref(rav1eFrame);
        rav1eFrame = NULL;
    }
    if (rav1eConfig) {
        rav1e_config_unref(rav1eConfig);
        rav1eConfig = NULL;
    }
    return result;
}

static avifBool rav1eCodecEncodeFinish(avifCodec * codec, avifCodecEncodeOutput * output)
{
    for (;;) {
        RaEncoderStatus encoderStatus = rav1e_send_frame(codec->internal->rav1eContext, NULL); // flush
        if (encoderStatus != RA_ENCODER_STATUS_SUCCESS) {
            return AVIF_FALSE;
        }

        avifBool gotPacket = AVIF_FALSE;
        RaPacket * pkt = NULL;
        for (;;) {
            encoderStatus = rav1e_receive_packet(codec->internal->rav1eContext, &pkt);
            if (encoderStatus == RA_ENCODER_STATUS_ENCODED) {
                continue;
            }
            if ((encoderStatus != RA_ENCODER_STATUS_SUCCESS) && (encoderStatus != RA_ENCODER_STATUS_LIMIT_REACHED)) {
                return AVIF_FALSE;
            }
            if (pkt) {
                gotPacket = AVIF_TRUE;
                if (pkt->data && (pkt->len > 0)) {
                    if (avifCodecEncodeOutputAddSample(output, pkt->data, pkt->len, (pkt->frame_type == RA_FRAME_TYPE_KEY)) !=
                        AVIF_RESULT_OK) {
                        return AVIF_FALSE;
                    }
                }
                rav1e_packet_unref(pkt);
                pkt = NULL;
            } else {
                break;
            }
        }

        if (!gotPacket) {
            break;
        }
    }
    return AVIF_TRUE;
}

const char * avifCodecVersionRav1e(void)
{
    return rav1e_version_full();
}

avifCodec * avifCodecCreateRav1e(void)
{
    avifCodec * codec = (avifCodec *)avifAlloc(sizeof(avifCodec));
    if (codec == NULL) {
        return NULL;
    }
    memset(codec, 0, sizeof(struct avifCodec));
    codec->encodeImage = rav1eCodecEncodeImage;
    codec->encodeFinish = rav1eCodecEncodeFinish;
    codec->destroyInternal = rav1eCodecDestroyInternal;

    codec->internal = (struct avifCodecInternal *)avifAlloc(sizeof(struct avifCodecInternal));
    if (codec->internal == NULL) {
        avifFree(codec);
        return NULL;
    }
    memset(codec->internal, 0, sizeof(struct avifCodecInternal));
    return codec;
}
