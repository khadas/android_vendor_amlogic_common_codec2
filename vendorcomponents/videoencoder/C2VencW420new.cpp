/*
 * Copyright 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_NDEBUG 0
#define LOG_TAG "C2VencW420New"
#include <utils/Log.h>
#include <utils/misc.h>

#include <algorithm>

#include <media/hardware/VideoAPI.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/foundation/AUtils.h>

#include <C2Debug.h>
#include <C2Config.h>
#include <Codec2Mapper.h>
#include <C2PlatformSupport.h>
#include <Codec2BufferUtils.h>
#include <SimpleC2Interface.h>
#include <util/C2InterfaceHelper.h>
#include <dlfcn.h>
#include "C2VendorSupport.h"
#include "C2VencW420new.h"

namespace android {

constexpr char COMPONENT_NAME[] = "c2.amlogic.hevc.encoder";

#define C2W420_LOG(level, fmt, str...) CODEC2_LOG(level, "[##%d##]"#fmt, mInstanceID, ##str)
#define true 1
uint32_t C2VencW420New::mInstanceID = 0;


#define MAX_INPUT_BUFFER_HEADERS 4
#define MAX_CONVERSION_BUFFERS   4
#define CODEC_MAX_CORES          4
#define LEN_STATUS_BUFFER        (10  * 1024)
#define MAX_VBV_BUFF_SIZE        (120 * 16384)
#define MAX_NUM_IO_BUFS           3

#define DEFAULT_MAX_REF_FRM         2
#define DEFAULT_MAX_REORDER_FRM     0
#define DEFAULT_QP_MIN              1
#define DEFAULT_QP_MAX              40
#define DEFAULT_MAX_BITRATE         240000000
#define DEFAULT_MAX_SRCH_RANGE_X    256
#define DEFAULT_MAX_SRCH_RANGE_Y    256
#define DEFAULT_MAX_FRAMERATE       120000
#define DEFAULT_NUM_CORES           1
#define DEFAULT_NUM_CORES_PRE_ENC   0
#define DEFAULT_FPS                 30
#define DEFAULT_ENC_SPEED           IVE_NORMAL

#define DEFAULT_MEM_REC_CNT         0
#define DEFAULT_RECON_ENABLE        0
#define DEFAULT_CHKSUM_ENABLE       0
#define DEFAULT_START_FRM           0
#define DEFAULT_NUM_FRMS            0xFFFFFFFF
#define DEFAULT_INP_COLOR_FORMAT       IV_YUV_420SP_VU
#define DEFAULT_RECON_COLOR_FORMAT     IV_YUV_420P
#define DEFAULT_LOOPBACK            0
#define DEFAULT_SRC_FRAME_RATE      30
#define DEFAULT_TGT_FRAME_RATE      30
#define DEFAULT_MAX_WD              1920
#define DEFAULT_MAX_HT              1920
#define DEFAULT_MAX_LEVEL           41
#define DEFAULT_STRIDE              0
#define DEFAULT_WD                  1280
#define DEFAULT_HT                  720
#define DEFAULT_PSNR_ENABLE         0
#define DEFAULT_ME_SPEED            100
#define DEFAULT_ENABLE_FAST_SAD     0
#define DEFAULT_ENABLE_ALT_REF      0
#define DEFAULT_RC_MODE             IVE_RC_STORAGE
#define DEFAULT_BITRATE             6000000
#define DEFAULT_I_QP                22
#define DEFAULT_I_QP_MAX            DEFAULT_QP_MAX
#define DEFAULT_I_QP_MIN            DEFAULT_QP_MIN
#define DEFAULT_P_QP                28
#define DEFAULT_P_QP_MAX            DEFAULT_QP_MAX
#define DEFAULT_P_QP_MIN            DEFAULT_QP_MIN
#define DEFAULT_B_QP                22
#define DEFAULT_B_QP_MAX            DEFAULT_QP_MAX
#define DEFAULT_B_QP_MIN            DEFAULT_QP_MIN
#define DEFAULT_AIR                 IVE_AIR_MODE_NONE
#define DEFAULT_AIR_REFRESH_PERIOD  30
#define DEFAULT_SRCH_RNG_X          64
#define DEFAULT_SRCH_RNG_Y          48
#define DEFAULT_I_INTERVAL          30
#define DEFAULT_IDR_INTERVAL        1000
#define DEFAULT_B_FRAMES            0
#define DEFAULT_DISABLE_DEBLK_LEVEL 0
#define DEFAULT_HPEL                1
#define DEFAULT_QPEL                1
#define DEFAULT_I4                  1
#define DEFAULT_EPROFILE            IV_PROFILE_BASE
#define DEFAULT_ENTROPY_MODE        0
#define DEFAULT_SLICE_MODE          IVE_SLICE_MODE_NONE
#define DEFAULT_SLICE_PARAM         256
#define DEFAULT_ARCH                ARCH_ARM_A9Q
#define DEFAULT_SOC                 SOC_GENERIC
#define DEFAULT_INTRA4x4            0

#define DEFAULT_CONSTRAINED_INTRA   0

#define CODEC_QP_MIN                4
#define CODEC_QP_MAX                51

#define WAVE420_MAX_B_FRAMES 1

int W420ColorFormatSupprt[] = {
    HAL_PIXEL_FORMAT_YCBCR_420_888,
    HAL_PIXEL_FORMAT_YCRCB_420_SP,
    HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED //color from surface!!!
};



void C2VencW420New::ParseGop(
        const C2StreamGopTuning::output &gop,
        uint32_t *syncInterval, uint32_t *iInterval, uint32_t *maxBframes) {
    uint32_t syncInt = 1;
    uint32_t iInt = 1;
    for (size_t i = 0; i < gop.flexCount(); ++i) {
        const C2GopLayerStruct &layer = gop.m.values[i];
        if (layer.count == UINT32_MAX) {
            syncInt = 0;
        } else if (syncInt <= UINT32_MAX / (layer.count + 1)) {
            syncInt *= (layer.count + 1);
        }
        if ((layer.type_ & I_FRAME) == 0) {
            if (layer.count == UINT32_MAX) {
                iInt = 0;
            } else if (iInt <= UINT32_MAX / (layer.count + 1)) {
                iInt *= (layer.count + 1);
            }
        }
        if (layer.type_ == C2Config::picture_type_t(P_FRAME | B_FRAME) && maxBframes) {
            *maxBframes = layer.count;
        }
    }
    if (syncInterval) {
        *syncInterval = syncInt;
    }
    if (iInterval) {
        *iInterval = iInt;
    }
}


class C2VencW420New::IntfImpl : /*public C2InterfaceHelper*/public SimpleInterface<void>::BaseParams{
public:
    explicit IntfImpl(const std::shared_ptr<C2ReflectorHelper> &helper)
        //: C2InterfaceHelper(helper) {
        : SimpleInterface<void>::BaseParams(
                helper,
                COMPONENT_NAME,
                C2Component::KIND_ENCODER,
                C2Component::DOMAIN_VIDEO,
                MEDIA_MIMETYPE_VIDEO_HEVC
                /*MEDIA_MIMETYPE_IMAGE_ANDROID_HEIC*/) {
    ALOGD("C2VencHCodec::IntfImpl constructor!");
    setDerivedInstance(this);
    /*addParameter(
            DefineParam(mInputMediaType, C2_PARAMKEY_INPUT_MEDIA_TYPE)
            .withConstValue(AllocSharedString<C2PortMediaTypeSetting::input>(MEDIA_MIMETYPE_VIDEO_RAW))
            .build());
    addParameter(
            DefineParam(mOutputMediaType, C2_PARAMKEY_OUTPUT_MEDIA_TYPE)
            .withConstValue(AllocSharedString<C2PortMediaTypeSetting::output>(MEDIA_MIMETYPE_VIDEO_AVC))
            .build());
    addParameter(
            DefineParam(mKind, C2_PARAMKEY_COMPONENT_KIND)
            .withConstValue(new C2ComponentKindSetting(C2Component::KIND_ENCODER))
            .build());

    addParameter(
            DefineParam(mDomain, C2_PARAMKEY_COMPONENT_DOMAIN)
            .withConstValue(new C2ComponentDomainSetting(C2Component::DOMAIN_VIDEO))
            .build());

    addParameter(
            DefineParam(mInputFormat, C2_PARAMKEY_INPUT_STREAM_BUFFER_TYPE)
            .withConstValue(new C2StreamBufferTypeSetting::input(0u, C2BufferData::GRAPHIC))
            .build());

    addParameter(
            DefineParam(mOutputFormat, C2_PARAMKEY_OUTPUT_STREAM_BUFFER_TYPE)
            .withConstValue(new C2StreamBufferTypeSetting::output(0u, C2BufferData::LINEAR))
            .build());*/
    addParameter(
                DefineParam(mUsage, C2_PARAMKEY_INPUT_STREAM_USAGE)
                .withConstValue(new C2StreamUsageTuning::input(
                        0u, (uint64_t)C2MemoryUsage::CPU_READ))
                .build());

    addParameter(
            DefineParam(mAttrib, C2_PARAMKEY_COMPONENT_ATTRIBUTES)
            .withConstValue(new C2ComponentAttributesSetting(
                C2Component::ATTRIB_IS_TEMPORAL))
            .build());

    addParameter(
            DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
            .withDefault(new C2StreamPictureSizeInfo::input(0u, 176, 144))
            .withFields({
                C2F(mSize, width).inRange(176, 2160, 8), /*wave420 height and width must align with 8*/
                C2F(mSize, height).inRange(144, 2160, 2),
            })
            .withSetter(SizeSetter)
            .build());

    addParameter(
            DefineParam(mGop, C2_PARAMKEY_GOP)
            .withDefault(C2StreamGopTuning::output::AllocShared(
                    0 /* flexCount */, 0u /* stream */))
            .withFields({C2F(mGop, m.values[0].type_).any(),
                         C2F(mGop, m.values[0].count).any()})
            .withSetter(GopSetter)
            .build());

    addParameter(
            DefineParam(mPictureQuantization, C2_PARAMKEY_PICTURE_QUANTIZATION)
            .withDefault(C2StreamPictureQuantizationTuning::output::AllocShared(
                    0 /* flexCount */, 0u /* stream */))
            .withFields({C2F(mPictureQuantization, m.values[0].type_).oneOf(
                            {C2Config::picture_type_t(I_FRAME),
                              C2Config::picture_type_t(P_FRAME),
                              C2Config::picture_type_t(B_FRAME)}),
                         C2F(mPictureQuantization, m.values[0].min).any(),
                         C2F(mPictureQuantization, m.values[0].max).any()})
            .withSetter(PictureQuantizationSetter)
            .build());

    addParameter(
            DefineParam(mFrameRate, C2_PARAMKEY_FRAME_RATE)
            .withDefault(new C2StreamFrameRateInfo::output(0u, 1.))
            // TODO: More restriction?
            .withFields({C2F(mFrameRate, value).greaterThan(0.)})
            .withSetter(Setter<decltype(*mFrameRate)>::StrictValueWithNoDeps)
            .build());

    addParameter(
            DefineParam(mBitrate, C2_PARAMKEY_BITRATE)
            .withDefault(new C2StreamBitrateInfo::output(0u, 64000))
            .withFields({C2F(mBitrate, value).inRange(4096, 12000000)})
            .withSetter(BitrateSetter)
            .build());

    addParameter(
            DefineParam(mIntraRefresh, C2_PARAMKEY_INTRA_REFRESH)
            .withDefault(new C2StreamIntraRefreshTuning::output(
                    0u, C2Config::INTRA_REFRESH_DISABLED, 0.))
            .withFields({
                C2F(mIntraRefresh, mode).oneOf({
                    C2Config::INTRA_REFRESH_DISABLED, C2Config::INTRA_REFRESH_ARBITRARY }),
                C2F(mIntraRefresh, period).any()
            })
            .withSetter(IntraRefreshSetter)
            .build());

    addParameter(
            DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
            .withDefault(new C2StreamProfileLevelInfo::output(
                    0u, PROFILE_HEVC_MAIN, LEVEL_HEVC_HIGH_5_1))
            .withFields({
                C2F(mProfileLevel, profile).oneOf({
                    PROFILE_HEVC_MAIN,
                }),
                C2F(mProfileLevel, level).oneOf({
                        LEVEL_HEVC_HIGH_4,  ///< HEVC (H.265) High Tier Level 4
                        LEVEL_HEVC_HIGH_4_1,                        ///< HEVC (H.265) High Tier Level 4.1
                        LEVEL_HEVC_HIGH_5,                          ///< HEVC (H.265) High Tier Level 5
                        LEVEL_HEVC_HIGH_5_1,                        ///< HEVC (H.265) High Tier Level 5.1
                        LEVEL_HEVC_HIGH_5_2,                        ///< HEVC (H.265) High Tier Level 5.2
                        LEVEL_HEVC_HIGH_6,                          ///< HEVC (H.265) High Tier Level 6
                        LEVEL_HEVC_HIGH_6_1,                        ///< HEVC (H.265) High Tier Level 6.1
                        LEVEL_HEVC_HIGH_6_2,                        ///< HEVC (H.265) High Tier Level 6.2
                }),
            })
            .withSetter(ProfileLevelSetter, mSize, mFrameRate, mBitrate)
            .build());

    addParameter(
            DefineParam(mRequestSync, C2_PARAMKEY_REQUEST_SYNC_FRAME)
            .withDefault(new C2StreamRequestSyncFrameTuning::output(0u, C2_FALSE))
            .withFields({C2F(mRequestSync, value).oneOf({ C2_FALSE, C2_TRUE }) })
            .withSetter(Setter<decltype(*mRequestSync)>::NonStrictValueWithNoDeps)
            .build());

    addParameter(
            DefineParam(mSyncFramePeriod, C2_PARAMKEY_SYNC_FRAME_INTERVAL)
            .withDefault(new C2StreamSyncFrameIntervalTuning::output(0u, 1000000))
            .withFields({C2F(mSyncFramePeriod, value).any()})
            .withSetter(Setter<decltype(*mSyncFramePeriod)>::StrictValueWithNoDeps)
            .build());

    addParameter(
            DefineParam(mColorAspects, C2_PARAMKEY_COLOR_ASPECTS)
            .withDefault(new C2StreamColorAspectsInfo::input(
                    0u, C2Color::RANGE_UNSPECIFIED, C2Color::PRIMARIES_UNSPECIFIED,
                    C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
            .withFields({
                C2F(mColorAspects, range).inRange(
                            C2Color::RANGE_UNSPECIFIED,     C2Color::RANGE_OTHER),
                C2F(mColorAspects, primaries).inRange(
                            C2Color::PRIMARIES_UNSPECIFIED, C2Color::PRIMARIES_OTHER),
                C2F(mColorAspects, transfer).inRange(
                            C2Color::TRANSFER_UNSPECIFIED,  C2Color::TRANSFER_OTHER),
                C2F(mColorAspects, matrix).inRange(
                            C2Color::MATRIX_UNSPECIFIED,    C2Color::MATRIX_OTHER)
            })
            .withSetter(ColorAspectsSetter)
            .build());

    addParameter(
            DefineParam(mCodedColorAspects, C2_PARAMKEY_VUI_COLOR_ASPECTS)
            .withDefault(new C2StreamColorAspectsInfo::output(
                    0u, C2Color::RANGE_LIMITED, C2Color::PRIMARIES_UNSPECIFIED,
                    C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
            .withFields({
                C2F(mCodedColorAspects, range).inRange(
                            C2Color::RANGE_UNSPECIFIED,     C2Color::RANGE_OTHER),
                C2F(mCodedColorAspects, primaries).inRange(
                            C2Color::PRIMARIES_UNSPECIFIED, C2Color::PRIMARIES_OTHER),
                C2F(mCodedColorAspects, transfer).inRange(
                            C2Color::TRANSFER_UNSPECIFIED,  C2Color::TRANSFER_OTHER),
                C2F(mCodedColorAspects, matrix).inRange(
                            C2Color::MATRIX_UNSPECIFIED,    C2Color::MATRIX_OTHER)
            })
            .withSetter(CodedColorAspectsSetter, mColorAspects)
            .build());

    // TODO: support more formats?
    std::vector<uint32_t> pixelFormats;
    for (int i = 0;i < sizeof(W420ColorFormatSupprt) / sizeof(int);i++)
    {
        pixelFormats.push_back(W420ColorFormatSupprt[i]);
    }

    addParameter(
            DefineParam(mPixelFormat, C2_PARAMKEY_PIXEL_FORMAT)
            .withDefault(new C2StreamPixelFormatInfo::input(
                              0u, HAL_PIXEL_FORMAT_YCRCB_420_SP))
            .withFields({C2F(mPixelFormat, value).oneOf(pixelFormats)})
            .withSetter((Setter<decltype(*mPixelFormat)>::StrictValueWithNoDeps))
            .build());

}
    static C2R SizeSetter(bool mayBlock, const C2P<C2StreamPictureSizeInfo::input> &oldMe,
        C2P<C2StreamPictureSizeInfo::input> &me) {
        (void)mayBlock;
        C2R res = C2R::Ok();
        ALOGD("SizeSetter oldwidth:%d,oldheight:%d,newwidth:%d,newheight:%d",oldMe.v.width,oldMe.v.height,me.v.width,me.v.height);
        if (!me.F(me.v.width).supportsAtAll(me.v.width)) {
            res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.width)));
            me.set().width = oldMe.v.width;
        }
        if (!me.F(me.v.height).supportsAtAll(me.v.height)) {
            res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.height)));
            me.set().height = oldMe.v.height;
        }
        return res;
    }
/*
    static C2R InputDelaySetter(
            bool mayBlock,
            C2P<C2PortActualDelayTuning::input> &me,
            const C2P<C2StreamGopTuning::output> &gop) {
        (void)mayBlock;
        uint32_t maxBframes = 0;
        ParseGop(gop.v, nullptr, nullptr, &maxBframes);
        me.set().value = maxBframes;
        return C2R::Ok();
    }*/

    static C2R BitrateSetter(bool mayBlock, C2P<C2StreamBitrateInfo::output> &me) {
        (void)mayBlock;
        C2R res = C2R::Ok();
        if (me.v.value <= 4096) {
            me.set().value = 4096;
        }
        return res;
    }

    static C2R ProfileLevelSetter(
            bool mayBlock,
            C2P<C2StreamProfileLevelInfo::output> &me,
            const C2P<C2StreamPictureSizeInfo::input> &size,
            const C2P<C2StreamFrameRateInfo::output> &frameRate,
            const C2P<C2StreamBitrateInfo::output> &bitrate) {
        (void)mayBlock;
        if (!me.F(me.v.profile).supportsAtAll(me.v.profile)) {
            me.set().profile = PROFILE_HEVC_MAIN;
        }

        struct LevelLimits {
            C2Config::level_t level;
            uint32_t samplesPerSec;
            uint32_t samples;
            uint32_t bitrate;
        };
        constexpr LevelLimits kLimits[] = {
            { LEVEL_HEVC_MAIN_1,       552960,    36864,    128000 },
            { LEVEL_HEVC_MAIN_2,      3686400,   122880,   1500000 },
            { LEVEL_HEVC_MAIN_2_1,    7372800,   245760,   3000000 },
            { LEVEL_HEVC_MAIN_3,     16588800,   552960,   6000000 },
            { LEVEL_HEVC_MAIN_3_1,   33177600,   983040,  10000000 },
            { LEVEL_HEVC_MAIN_4,     66846720,  2228224,  12000000 },
            { LEVEL_HEVC_MAIN_4_1,  133693440,  2228224,  20000000 },
            { LEVEL_HEVC_MAIN_5,    267386880,  8912896,  25000000 },
            { LEVEL_HEVC_MAIN_5_1,  534773760,  8912896,  40000000 },
            { LEVEL_HEVC_MAIN_5_2, 1069547520,  8912896,  60000000 },
            { LEVEL_HEVC_MAIN_6,   1069547520, 35651584,  60000000 },
            { LEVEL_HEVC_MAIN_6_1, 2139095040, 35651584, 120000000 },
            { LEVEL_HEVC_MAIN_6_2, 4278190080, 35651584, 240000000 },
        };

        uint32_t samples = size.v.width * size.v.height;
        uint32_t samplesPerSec = samples * frameRate.v.value;

        // Check if the supplied level meets the MB / bitrate requirements. If
        // not, update the level with the lowest level meeting the requirements.

        bool found = false;
        // By default needsUpdate = false in case the supplied level does meet
        // the requirements.
        bool needsUpdate = false;
        for (const LevelLimits &limit : kLimits) {
            if (samples <= limit.samples && samplesPerSec <= limit.samplesPerSec &&
                    bitrate.v.value <= limit.bitrate) {
                // This is the lowest level that meets the requirements, and if
                // we haven't seen the supplied level yet, that means we don't
                // need the update.
                if (needsUpdate) {
                    ALOGD("Given level %x does not cover current configuration: "
                          "adjusting to %x", me.v.level, limit.level);
                    me.set().level = limit.level;
                }
                found = true;
                break;
            }
            if (me.v.level == limit.level) {
                // We break out of the loop when the lowest feasible level is
                // found. The fact that we're here means that our level doesn't
                // meet the requirement and needs to be updated.
                needsUpdate = true;
            }
        }
        if (!found) {
            // We set to the highest supported level.
            me.set().level = LEVEL_HEVC_MAIN_5_2;
        }
        return C2R::Ok();
    }

    static C2R IntraRefreshSetter(bool mayBlock, C2P<C2StreamIntraRefreshTuning::output> &me) {
        (void)mayBlock;
        C2R res = C2R::Ok();
        if (me.v.period < 1) {
            me.set().mode = C2Config::INTRA_REFRESH_DISABLED;
            me.set().period = 0;
        } else {
            // only support arbitrary mode (cyclic in our case)
            me.set().mode = C2Config::INTRA_REFRESH_ARBITRARY;
        }
        return res;
    }

    static C2R GopSetter(bool mayBlock, C2P<C2StreamGopTuning::output> &me) {
        (void)mayBlock;
        ALOGI("GopSetter enter,count:%zu",me.v.flexCount());
        for (size_t i = 0; i < me.v.flexCount(); ++i) {
            const C2GopLayerStruct &layer = me.v.m.values[0];
            ALOGI("GopSetter,type:%d,count:%d",layer.type_,layer.count);
            if (layer.type_ == C2Config::picture_type_t(P_FRAME | B_FRAME)
                    && layer.count > WAVE420_MAX_B_FRAMES) {
                me.set().m.values[i].count = WAVE420_MAX_B_FRAMES;
            }
            if (layer.type_ == C2Config::picture_type_t(I_FRAME)) {
                ALOGI("GopSetter gop:%d",layer.count);
            }
        }
        return C2R::Ok();
    }

    static C2R PictureQuantizationSetter(bool mayBlock,
                                         C2P<C2StreamPictureQuantizationTuning::output> &me) {
        (void)mayBlock;
        (void)me;

        // TODO: refactor with same algorithm in the SetQp()
        int32_t iMin = DEFAULT_I_QP_MIN, pMin = DEFAULT_P_QP_MIN, bMin = DEFAULT_B_QP_MIN;
        int32_t iMax = DEFAULT_I_QP_MAX, pMax = DEFAULT_P_QP_MAX, bMax = DEFAULT_B_QP_MAX;

        for (size_t i = 0; i < me.v.flexCount(); ++i) {
            const C2PictureQuantizationStruct &layer = me.v.m.values[i];

            if (layer.type_ == C2Config::picture_type_t(I_FRAME)) {
                iMax = layer.max;
                iMin = layer.min;
                ALOGV("iMin %d iMax %d", iMin, iMax);
            } else if (layer.type_ == C2Config::picture_type_t(P_FRAME)) {
                pMax = layer.max;
                pMin = layer.min;
                ALOGV("pMin %d pMax %d", pMin, pMax);
            } else if (layer.type_ == C2Config::picture_type_t(B_FRAME)) {
                bMax = layer.max;
                bMin = layer.min;
                ALOGV("bMin %d bMax %d", bMin, bMax);
            }
        }

        ALOGV("PictureQuantizationSetter(entry): i %d-%d p %d-%d b %d-%d",
              iMin, iMax, pMin, pMax, bMin, bMax);

        // ensure we have legal values
        iMax = std::clamp(iMax, CODEC_QP_MIN, CODEC_QP_MAX);
        iMin = std::clamp(iMin, CODEC_QP_MIN, CODEC_QP_MAX);
        pMax = std::clamp(pMax, CODEC_QP_MIN, CODEC_QP_MAX);
        pMin = std::clamp(pMin, CODEC_QP_MIN, CODEC_QP_MAX);
        bMax = std::clamp(bMax, CODEC_QP_MIN, CODEC_QP_MAX);
        bMin = std::clamp(bMin, CODEC_QP_MIN, CODEC_QP_MAX);

        // put them back into the structure
        for (size_t i = 0; i < me.v.flexCount(); ++i) {
            const C2PictureQuantizationStruct &layer = me.v.m.values[i];

            if (layer.type_ == C2Config::picture_type_t(I_FRAME)) {
                me.set().m.values[i].max = iMax;
                me.set().m.values[i].min = iMin;
            }
            if (layer.type_ == C2Config::picture_type_t(P_FRAME)) {
                me.set().m.values[i].max = pMax;
                me.set().m.values[i].min = pMin;
            }
            if (layer.type_ == C2Config::picture_type_t(B_FRAME)) {
                me.set().m.values[i].max = bMax;
                me.set().m.values[i].min = bMin;
            }
        }

        ALOGV("PictureQuantizationSetter(exit): i %d-%d p %d-%d b %d-%d",
              iMin, iMax, pMin, pMax, bMin, bMax);

        return C2R::Ok();
    }

    static C2R ColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsInfo::input> &me) {
        (void)mayBlock;
        if (me.v.range > C2Color::RANGE_OTHER) {
                me.set().range = C2Color::RANGE_OTHER;
        }
        if (me.v.primaries > C2Color::PRIMARIES_OTHER) {
                me.set().primaries = C2Color::PRIMARIES_OTHER;
        }
        if (me.v.transfer > C2Color::TRANSFER_OTHER) {
                me.set().transfer = C2Color::TRANSFER_OTHER;
        }
        if (me.v.matrix > C2Color::MATRIX_OTHER) {
                me.set().matrix = C2Color::MATRIX_OTHER;
        }
        return C2R::Ok();
    }

    static C2R CodedColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsInfo::output> &me,
                                       const C2P<C2StreamColorAspectsInfo::input> &coded) {
        (void)mayBlock;
        me.set().range = coded.v.range;
        me.set().primaries = coded.v.primaries;
        me.set().transfer = coded.v.transfer;
        me.set().matrix = coded.v.matrix;
        return C2R::Ok();
    }

   static C2R StrictValueWithNoDeps(bool mayBlock, C2P<C2StreamPixelFormatInfo::input> &old,
                                          C2P<C2StreamPixelFormatInfo::input> &me) {
        (void)mayBlock;
        if (!me.F(me.v.value).supportsNow(me.v.value)) {
            me.set().value = old.v.value;
        }
        return me.F(me.v.value).validatePossible(me.v.value);
    }

    std::shared_ptr<C2StreamPictureSizeInfo::input> getSize() const { return mSize; }
    std::shared_ptr<C2StreamIntraRefreshTuning::output> getIntraRefresh() const { return mIntraRefresh; }
    std::shared_ptr<C2StreamFrameRateInfo::output> getFrameRate() const { return mFrameRate; }
    std::shared_ptr<C2StreamBitrateInfo::output> getBitrate() const { return mBitrate; }
    std::shared_ptr<C2StreamRequestSyncFrameTuning::output> getRequestSync() const { return mRequestSync; }
    std::shared_ptr<C2StreamGopTuning::output> getGop() const { return mGop; }
    std::shared_ptr<C2StreamPictureQuantizationTuning::output> getPictureQuantization() const { return mPictureQuantization; }
    std::shared_ptr<C2StreamColorAspectsInfo::output> getCodedColorAspects() const { return mCodedColorAspects; }
    std::shared_ptr<C2StreamPixelFormatInfo::input> getCodedPixelFormat() const { return mPixelFormat; }
    std::shared_ptr<C2StreamProfileLevelInfo::output> getProfileInfo() const { return mProfileLevel; }
    std::shared_ptr<C2StreamSyncFrameIntervalTuning::output> getIFrameInterval() const {return mSyncFramePeriod; }
private:
    std::shared_ptr<C2StreamPictureSizeInfo::input> mSize;
    std::shared_ptr<C2StreamUsageTuning::input> mUsage;
    std::shared_ptr<C2StreamFrameRateInfo::output> mFrameRate;
    std::shared_ptr<C2StreamRequestSyncFrameTuning::output> mRequestSync;
    std::shared_ptr<C2StreamIntraRefreshTuning::output> mIntraRefresh;
    std::shared_ptr<C2StreamBitrateInfo::output> mBitrate;
    std::shared_ptr<C2StreamProfileLevelInfo::output> mProfileLevel;
    std::shared_ptr<C2StreamSyncFrameIntervalTuning::output> mSyncFramePeriod;
    std::shared_ptr<C2StreamGopTuning::output> mGop;
    std::shared_ptr<C2StreamPictureQuantizationTuning::output> mPictureQuantization;
    std::shared_ptr<C2StreamColorAspectsInfo::input> mColorAspects;
    std::shared_ptr<C2StreamColorAspectsInfo::output> mCodedColorAspects;
    std::shared_ptr<C2StreamPixelFormatInfo::input> mPixelFormat;

};

// static
std::atomic<int32_t> C2VencW420New::sConcurrentInstances = 0;

// static
std::shared_ptr<C2Component> C2VencW420New::create(
        char *name, c2_node_id_t id, const std::shared_ptr<C2VencW420New::IntfImpl>& helper) {
    static const int32_t kMaxConcurrentInstances = 4;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    if (kMaxConcurrentInstances >= 0 && sConcurrentInstances.load() >= kMaxConcurrentInstances) {
        ALOGE("Reject to Initialize() due to too many instances: %d", sConcurrentInstances.load());
        return nullptr;
    }
    return std::shared_ptr<C2Component>(new C2VencW420New(name, id, helper));
}


C2VencW420New::C2VencW420New(const char *name, c2_node_id_t id, const std::shared_ptr<IntfImpl> &intfImpl)
            : C2VencComponent(std::make_shared<SimpleInterface<IntfImpl>>(name, id, intfImpl)),
              mIntfImpl(intfImpl),
              mEncHeaderFunc(NULL),
              mEncFrameFunc(NULL),
              mEncBitrateChangeFunc(NULL),
              mEncFrameRateChangeFunc(NULL),
              mDestroyFunc(NULL),
              mCodecHandle(0),
              mIDRInterval(0),
              mBitrateBak(0),
              //mtimeStampBak(0),
              //curFrameRateBak(0),
              //mElapsedTime(0),
              mtimeStampBak(0),
              mFrameRateValue(0) {
    ALOGD("C2VencW420New constructor!");
    propGetInt(CODEC2_VENC_LOGDEBUG_PROPERTY, &gloglevel);
    ALOGD("gloglevel:%x",gloglevel);
    sConcurrentInstances.fetch_add(1, std::memory_order_relaxed);
    mInstanceID++;
}


C2VencW420New::~C2VencW420New() {
    /*
     * This is the logic, no need to modify, ignore coverity weak cryptor report.
    */
    /*coverity[exn_spec_violation:SUPPRESS]*/

    ALOGD("C2VencW420New destructor!");
    sConcurrentInstances.fetch_sub(1, std::memory_order_relaxed);
    mInstanceID--;
}


int C2VencW420New::getFrameRate(int32_t frameIndex,int64_t timestamp) {
#if 0
    int64_t mElapsedTime = 0;
    int32_t curFrameRate = 0;
    int frame_rate = mFrameRate->value;

    if (frameIndex > 0 && timestamp != 0) {
        mElapsedTime = timestamp - mtimeStampBak;
        curFrameRate = 1000000.0 / mElapsedTime;
        ALOGD("InputFrameInfo.frameIndex:%d,timestamp now:%lld,timestamp last time:%lld,mElapsedTime:%lld",frameIndex,timestamp,mtimeStampBak,mElapsedTime);
        ALOGD("now calculate current FrameRate:%d",curFrameRate);
        if (curFrameRate > 0 && ((curFrameRate - curFrameRateBak) >= 4 || (curFrameRateBak - curFrameRate) >= 4)) {
            frame_rate = curFrameRate;
            ALOGD("frame_rate 1111");
        }
        else {
            frame_rate = curFrameRateBak;
            ALOGD("frame_rate 2222");
        }
    }
    ALOGD("frame_rate 333");
    curFrameRateBak = frame_rate;
    ALOGD("now use FrameRate:%d",frame_rate);
    mtimeStampBak = timestamp;
    return frame_rate;
    #endif
    return 0;
}

bool C2VencW420New::isSupportDMA() {
    C2W420_LOG(CODEC2_VENC_LOG_INFO,"w420 support dma mode:%d!",0);
    return false;
}

bool C2VencW420New::isSupportCanvas() {
    C2W420_LOG(CODEC2_VENC_LOG_INFO,"w420 not support canvas mode!");
    return false;
}



bool C2VencW420New::LoadModule() {
    C2W420_LOG(CODEC2_VENC_LOG_INFO,"C2VencW420New initModule!");
    void *handle = dlopen("libvp_hevc_codec_new.so", RTLD_NOW);
    if (handle) {
        mInitFunc = NULL;
        mInitFunc = (fn_hevc_video_encoder_init)dlsym(handle, "vl_video_encoder_init_hevc");
        if (mInitFunc == NULL) {
            C2W420_LOG(CODEC2_VENC_LOG_ERR,"dlsym for vl_video_encoder_init_hevc failed");
            dlclose(handle);
            return false;
        }

        mEncHeaderFunc = NULL;
        mEncHeaderFunc = (fn_hevc_video_encode_header)dlsym(handle, "vl_video_encoder_generate_header");
        if (mEncHeaderFunc == NULL) {
            C2W420_LOG(CODEC2_VENC_LOG_ERR,"dlsym for vl_video_encoder_generate_header failed");
            dlclose(handle);
            return false;
        }

        mEncFrameFunc = NULL;
        mEncFrameFunc = (fn_hevc_video_encoder_encode)dlsym(handle, "vl_video_encoder_encode_hevc");
        if (mEncFrameFunc == NULL) {
            C2W420_LOG(CODEC2_VENC_LOG_ERR,"dlsym for vl_video_encoder_encode_hevc failed");
            dlclose(handle);
            return false;
        }

        mEncBitrateChangeFunc = NULL;
        mEncBitrateChangeFunc = (fn_vl_change_bitrate)dlsym(handle, "vl_video_encoder_change_bitrate_hevc");
        if (mEncBitrateChangeFunc == NULL) {
            ALOGE("dlsym for vl_video_encoder_change_bitrate_hevc failed");
            dlclose(handle);
            return false;
        }

        mEncFrameRateChangeFunc = NULL;
        mEncFrameRateChangeFunc = (fn_vl_change_framerate_hevc)dlsym(handle, "vl_video_encoder_change_framerate_hevc");
        if (mEncFrameRateChangeFunc == NULL) {
            ALOGE("dlsym for vl_video_encoder_change_framerate_hevc failed");
            dlclose(handle);
            return false;
        }

        mDestroyFunc = NULL;
        mDestroyFunc = (fn_hevc_video_encoder_destroy)dlsym(handle,"vl_video_encoder_destroy_hevc");
        if (mDestroyFunc == NULL) {
            C2W420_LOG(CODEC2_VENC_LOG_ERR,"dlsym for vl_video_encoder_destroy_hevc failed");
            dlclose(handle);
            return false;
        }
    } else {
        C2W420_LOG(CODEC2_VENC_LOG_ERR,"dlopen for libvp_hevc_codec.so failed");
        dlclose(handle);
        return false;
    }
    /*
    * This is the logic, no need to modify, ignore coverity weak cryptor report.
    */
    /*coverity[leaked_storage:SUPPRESS]*/

    return true;
}


bool C2VencW420New::codecTypeTrans(uint32_t inputCodec,vl_img_format_hevc_t *pOutputCodec) {
    bool ret = true;
    if (!pOutputCodec) {
        C2W420_LOG(CODEC2_VENC_LOG_ERR,"param check failed!!");
        return false;
    }
    switch (inputCodec) {
        case HAL_PIXEL_FORMAT_YCBCR_420_888: {
            *pOutputCodec = IMG_FMT_NV21;
            ret = true;
            break;
        }
        case HAL_PIXEL_FORMAT_YCRCB_420_SP: {
            *pOutputCodec = IMG_FMT_YUV420P/*IMG_FMT_NV12*/;
            ret = true;
            break;
        }
        default: {
            *pOutputCodec = IMG_FMT_NV21;
            C2W420_LOG(CODEC2_VENC_LOG_ERR,"cannot support colorformat:%x",inputCodec);
            ret = false;
            break;
        }
    }
    return ret;
}


bool C2VencW420New::codec2TypeTrans(ColorFmt inputFmt,vl_img_format_hevc_t *pOutputCodec) {
    bool ret = true;
    if (!pOutputCodec) {
        C2W420_LOG(CODEC2_VENC_LOG_ERR,"param check failed!!");
        return false;
    }
    switch (inputFmt) {
        case C2_ENC_FMT_NV12: {
            *pOutputCodec = IMG_FMT_NV12;
            ret = true;
            break;
        }
        case C2_ENC_FMT_NV21: {
            *pOutputCodec = IMG_FMT_NV21;
            ret = true;
            break;
        }
        case C2_ENC_FMT_I420: {
            *pOutputCodec = IMG_FMT_YV12;
            ret = true;
            break;
        }
        case C2_ENC_FMT_RGBA8888: {
            *pOutputCodec = IMG_FMT_RGBA8888;
            ret = true;
            break;
        }
        default: {
            C2W420_LOG(CODEC2_VENC_LOG_ERR,"cannot support colorformat:%x",inputFmt);
            ret = false;
            break;
        }
    }
    return ret;
}

c2_status_t C2VencW420New::Init() {
    C2W420_LOG(CODEC2_VENC_LOG_INFO,"C2VencW420New Init!");
    vl_img_format_hevc_t colorformat = IMG_FMT_NONE;
    vl_encode_info_hevc_t initParam;
    qp_param_hevc_t qp_tbl;

    memset(&initParam,0,sizeof(initParam));
    memset(&qp_tbl,0,sizeof(qp_tbl));

    mSize = mIntfImpl->getSize();
    mFrameRate = mIntfImpl->getFrameRate();
    mGop = mIntfImpl->getGop();
    mBitrate = mIntfImpl->getBitrate();
    mPixelFormat = mIntfImpl->getCodedPixelFormat();
    mCodedColorAspects = mIntfImpl->getCodedColorAspects();
    mProfileLevel = mIntfImpl->getProfileInfo();
    mSyncFramePeriod = mIntfImpl->getIFrameInterval();
    mIDRInterval = (mSyncFramePeriod->value / 1000000) * mFrameRate->value; //max_int:just one i frame,0:all i frame

    //getQp(&mEncParams.i_qp_max,&initParam.i_qp_min,&initParam.p_qp_max,&initParam.p_qp_min);
    //codecTypeTrans(mPixelFormat->value,&mEncParams.colorformat);
    codecTypeTrans(mPixelFormat->value,&colorformat);
    C2W420_LOG(CODEC2_VENC_LOG_INFO,"upper set pixelformat:0x%x,colorAspect:%d",mPixelFormat->value,mCodedColorAspects->matrix);

/*    if (MATRIX_BT709 == mCodedColorAspects->matrix) {
        initParam.csc = ENC_CSC_BT709;
        C2W420_LOG(CODEC2_VENC_LOG_INFO,"color space BT709");
    }
    else {
        initParam.csc = ENC_CSC_BT601;
        C2W420_LOG(CODEC2_VENC_LOG_INFO,"color space BT601");
    }*/
    initParam.width = mSize->width;
    if (mSize->width < 256) {
        initParam.width = 256; //cause wave420 not support for width < 256
        C2W420_LOG(CODEC2_VENC_LOG_INFO,"set ctsmode=true");
        C2StreamPictureSizeInfo::output Size(0u, 256, mSize->height);
        std::vector<std::unique_ptr<C2SettingResult>> failures;
        mIntfImpl->config({ &Size }, C2_MAY_BLOCK, &failures);
    }
    initParam.height = mSize->height;//(mSize->height + 7) & ~7; //temprory modify
    initParam.frame_rate = mFrameRate->value;
    initParam.bit_rate = mBitrate->value;
    if (mIDRInterval < 0) {
        initParam.gop = mIDRInterval;
    }
    else {
        initParam.gop = mIDRInterval + 1;
    }

    initParam.prepend_spspps_to_idr_frames = true;
    initParam.img_format = colorformat;
    mBitrateBak = mBitrate->value;
    mFrameRateValue = mFrameRate->value;

    initParam.qp_mode = 1;
    qp_tbl.qp_min = DEFAULT_QP_MIN;
    qp_tbl.qp_max = DEFAULT_QP_MAX;

    initParam.enc_feature_opts |= ENABLE_PARA_UPDATE; //enable dynamic settings

    if (C2_OK == genVuiParam(&initParam.primaries,&initParam.transfer,&initParam.matrix,(bool *)&initParam.range)) {
        initParam.vui_info_present = true;
        initParam.video_signal_type = true;
        initParam.color_description = true;

        C2W420_LOG(CODEC2_VENC_LOG_INFO,"enable vui info");
    }


    C2W420_LOG(CODEC2_VENC_LOG_INFO,"width:%d,height:%d,framerate:%f,bitrate:%d,IFrameInterval:%d,QP_MIN:%d,QP_MAX:%d",
                                          mSize->width,
                                          mSize->height,
                                          mFrameRate->value,
                                          mBitrate->value,
                                          mIDRInterval,
                                          qp_tbl.qp_min,
                                          qp_tbl.qp_max);
    mCodecHandle = mInitFunc(CODEC_ID_H265,initParam,&qp_tbl);
    if (!mCodecHandle) {
        C2W420_LOG(CODEC2_VENC_LOG_ERR,"init failed!,codechandle:%lx",mCodecHandle);
        return C2_CORRUPTED;
    }
    C2W420_LOG(CODEC2_VENC_LOG_INFO,"init encoder success,codechandle:%lx",mCodecHandle);
    return C2_OK;
}


c2_status_t C2VencW420New::GenerateHeader(uint8_t *pHeaderData,uint32_t *pSize)
{
    C2W420_LOG(CODEC2_VENC_LOG_INFO,"C2VencW420New GenerateHeader!");
    unsigned int outSize = 0;
    encoding_metadata_hevc_t ret;

    memset(&ret,0,sizeof(ret));
    if (!pHeaderData || !pSize) {
        C2W420_LOG(CODEC2_VENC_LOG_ERR,"GenerateHeader parameter bad value,pls check!");
        return C2_BAD_VALUE;
    }
    ret = mEncHeaderFunc(mCodecHandle,pHeaderData,&outSize);
    if (outSize <= 0 || !ret.is_valid) {
        C2W420_LOG(CODEC2_VENC_LOG_ERR,"generate header failed!!!");
        return C2_BAD_VALUE;
    }
    *pSize = outSize;
    return C2_OK;
}


c2_status_t C2VencW420New::genVuiParam(int32_t *primaries,int32_t *transfer,int32_t *matrixCoeffs,bool *range)
{
    ColorAspects sfAspects;
    if (!C2Mapper::map(mCodedColorAspects->primaries, &sfAspects.mPrimaries)) {
        sfAspects.mPrimaries = android::ColorAspects::PrimariesUnspecified;
    }
    if (!C2Mapper::map(mCodedColorAspects->range, &sfAspects.mRange)) {
        sfAspects.mRange = android::ColorAspects::RangeUnspecified;
    }
    if (!C2Mapper::map(mCodedColorAspects->matrix, &sfAspects.mMatrixCoeffs)) {
        sfAspects.mMatrixCoeffs = android::ColorAspects::MatrixUnspecified;
    }
    if (!C2Mapper::map(mCodedColorAspects->transfer, &sfAspects.mTransfer)) {
        sfAspects.mTransfer = android::ColorAspects::TransferUnspecified;
    }
    ColorUtils::convertCodecColorAspectsToIsoAspects(sfAspects,primaries,transfer,matrixCoeffs,range);
    C2W420_LOG(CODEC2_VENC_LOG_INFO,"vui info:primaries:%d,transfer:%d,matrixCoeffs:%d,range:%d",*primaries,*transfer,*matrixCoeffs,(int)(*range));
    return C2_OK;
}

void C2VencW420New::getResolution(int *pWidth,int *pHeight)
{
    *pWidth = mSize->width;
    *pHeight = mSize->height;
}

void C2VencW420New::getCodecDumpFileName(std::string &strName,DumpFileType_e type) {
    char pName[128];
    memset(pName,0,sizeof(pName));
    sprintf(pName, "/data/venc_dump_%lx.%s", mCodecHandle,(C2_DUMP_RAW == type) ? "yuv" : "h265");
    strName = pName;
    C2W420_LOG(CODEC2_VENC_LOG_INFO,"Enable Dump raw file, name: %s", strName.c_str());
}



c2_status_t C2VencW420New::getQp(int32_t *i_qp_max,int32_t *i_qp_min,int32_t *p_qp_max,int32_t *p_qp_min) {
    ALOGV("in getQp()");

    // TODO: refactor with same algorithm in the PictureQuantizationSetter()
    int32_t iMin = DEFAULT_I_QP_MIN, pMin = DEFAULT_P_QP_MIN, bMin = DEFAULT_B_QP_MIN;
    int32_t iMax = DEFAULT_I_QP_MAX, pMax = DEFAULT_P_QP_MAX, bMax = DEFAULT_B_QP_MAX;

    IntfImpl::Lock lock = mIntfImpl->lock();

    std::shared_ptr<C2StreamPictureQuantizationTuning::output> qp =
                    mIntfImpl->getPictureQuantization();
    for (size_t i = 0; i < qp->flexCount(); ++i) {
        const C2PictureQuantizationStruct &layer = qp->m.values[i];

        if (layer.type_ == C2Config::picture_type_t(I_FRAME)) {
            iMax = layer.max;
            iMin = layer.min;
            ALOGV("i_qp_Min %d i_qp_Max %d", iMin, iMax);
        } else if (layer.type_ == C2Config::picture_type_t(P_FRAME)) {
            pMax = layer.max;
            pMin = layer.min;
            ALOGV("p_qp_Min %d p_qp_Max %d", pMin, pMax);
        } else if (layer.type_ == C2Config::picture_type_t(B_FRAME)) {
            bMax = layer.max;
            bMin = layer.min;
            ALOGV("b_qp_Min %d b_qp_Max %d", bMin, bMax);
        }
    }

    *i_qp_max = iMax;
    *i_qp_min = iMin;
    *p_qp_max = pMax;
    *p_qp_min = pMin;
    return C2_OK;
}


c2_status_t C2VencW420New::ProcessOneFrame(InputFrameInfo_t InputFrameInfo,OutputFrameInfo_t *pOutFrameInfo) {
    ALOGD("C2VencMulti ProcessOneFrame! yPlane:%p,uPlane:%p,vPlane:%p",InputFrameInfo.yPlane,InputFrameInfo.uPlane,InputFrameInfo.vPlane);
    encoding_metadata_hevc_t ret;
    vl_buffer_info_hevc_t inputInfo;
    uint32_t curFrameRate = 0;
    vl_frame_type_hevc_t frameType = FRAME_TYPE_NONE;
    int32_t bitRateNeedChangeTo = 0;

    if (!InputFrameInfo.yPlane || !InputFrameInfo.uPlane || !InputFrameInfo.vPlane || !pOutFrameInfo) {
        ALOGD("ProcessOneFrame parameter bad value,pls check!");
        return C2_BAD_VALUE;
    }
    ALOGE("y1:%d,y2:%d,y3:%d",InputFrameInfo.yPlane[0],InputFrameInfo.yPlane[1],InputFrameInfo.yPlane[2]);
    memset(&inputInfo,0,sizeof(inputInfo));
    IntfImpl::Lock lock = mIntfImpl->lock();
    //std::shared_ptr<C2StreamIntraRefreshTuning::output> intraRefresh = mIntfImpl->getIntraRefresh();
    std::shared_ptr<C2StreamBitrateInfo::output> bitrate = mIntfImpl->getBitrate();
    std::shared_ptr<C2StreamRequestSyncFrameTuning::output> requestSync = mIntfImpl->getRequestSync();
    mFrameRate = mIntfImpl->getFrameRate();
    lock.unlock();

    frameType = FRAME_TYPE_AUTO;
    if (requestSync != mRequestSync) {
        // we can handle IDR immediately
        if (requestSync->value) {
            // unset request
            C2StreamRequestSyncFrameTuning::output clearSync(0u, C2_FALSE);
            std::vector<std::unique_ptr<C2SettingResult>> failures;
            mIntfImpl->config({ &clearSync }, C2_MAY_BLOCK, &failures);
            frameType = FRAME_TYPE_IDR;
            ALOGV("Got dynamic IDR request");
        }
        mRequestSync = requestSync;
    }
    inputInfo.buf_type = VMALLOC_TYPE;
    ALOGI("mPixelFormat->value:0x%x,InputFrameInfo.colorFmt:%x",mPixelFormat->value,InputFrameInfo.colorFmt);
    codec2TypeTrans(InputFrameInfo.colorFmt,&inputInfo.buf_fmt);


    if (IMG_FMT_RGBA8888 == inputInfo.buf_fmt) {
        inputInfo.buf_info.in_ptr[0] = (unsigned long)InputFrameInfo.yPlane;
        inputInfo.buf_info.in_ptr[1] = (unsigned long)InputFrameInfo.uPlane;
    /*
     * This is the logic, no need to modify, ignore coverity weak cryptor report.
    */
    /*coverity[copy_paste_error:SUPPRESS]*/
        inputInfo.buf_info.in_ptr[2] = (unsigned long)InputFrameInfo.vPlane;
    }
    else {
        inputInfo.buf_info.in_ptr[0] = (unsigned long)InputFrameInfo.yPlane;
        inputInfo.buf_info.in_ptr[1] = (unsigned long)InputFrameInfo.vPlane;//inputInfo.YCbCr[0] + mSize->width * mSize->height;//(unsigned long)uPlane;
        inputInfo.buf_info.in_ptr[2] = (unsigned long)InputFrameInfo.vPlane;//(unsigned long)vPlane;
    }
    if (IMG_FMT_RGBA8888 != inputInfo.buf_fmt) {
        inputInfo.buf_stride = InputFrameInfo.yStride;//mSize->width;//pitch,need modify,fix me ????
    }
    else {
        inputInfo.buf_stride = InputFrameInfo.yStride / 4;
    }

    int frame_rate = 0;
    int64_t mElapsedTime = 0;
    if (InputFrameInfo.frameIndex > 0 && InputFrameInfo.timeStamp != 0) {
        mElapsedTime = InputFrameInfo.timeStamp - mtimeStampBak;
        curFrameRate = 1000000.0 / mElapsedTime;
        ALOGE("InputFrameInfo.frameIndex:%" PRId64",timestamp now:%" PRId64",timestamp last time:%" PRId64",mElapsedTime:%" PRId64"",InputFrameInfo.frameIndex,InputFrameInfo.timeStamp,mtimeStampBak,mElapsedTime);
        ALOGE("now calculate current FrameRate:%d,mFrameRateValue:%d",curFrameRate,mFrameRateValue);
        if (curFrameRate > 0 /*&& abs(curFrameRate - mFrameRateValue) >= 5*/) {
            int absvalue = (curFrameRate > mFrameRateValue )?(curFrameRate - mFrameRateValue):(mFrameRateValue - curFrameRate);
            if (absvalue >= 2) {
                frame_rate = curFrameRate;
                ALOGE("frame_rate change to :%d",frame_rate);
                mEncFrameRateChangeFunc(mCodecHandle,frame_rate,bitrate->value);
                #if 0
                    bitRateNeedChangeTo  = mBitrate->value / frame_rate * mFrameRate->value;
                    ALOGE("frame_rate change to :%d,bit_rate:%d",frame_rate,bitRateNeedChangeTo);
                    mEncBitrateChangeFunc(mCodecHandle,bitRateNeedChangeTo);
                #else
                    UNUSED(bitRateNeedChangeTo);
                #endif
                mFrameRateValue = curFrameRate;
            }
        }
        frame_rate = mFrameRateValue;
    }
    mtimeStampBak = InputFrameInfo.timeStamp;

    if (mBitrateBak != bitrate->value) {
        ALOGD("bitrate change to %d",bitrate->value);
        mEncBitrateChangeFunc(mCodecHandle,bitrate->value);
        mBitrateBak = bitrate->value;
    }

    ALOGD("Debug input info:yAddr:0x%lx,uAddr:0x%lx,vAddr:0x%lx,frame_type:%d,fmt:%d,pitch:%d,bitrate:%d",inputInfo.buf_info.in_ptr[0],
         inputInfo.buf_info.in_ptr[1],inputInfo.buf_info.in_ptr[2],inputInfo.buf_type,inputInfo.buf_fmt,inputInfo.buf_stride,bitrate->value);
    ret = mEncFrameFunc(mCodecHandle, frameType, (unsigned char*)pOutFrameInfo->Data, &inputInfo);
    pOutFrameInfo->Length = ret.encoded_data_length_in_bytes;
    ALOGD("encode finish,output len:%d",ret.encoded_data_length_in_bytes);
    if (!ret.is_valid) {
        ALOGE("multi encode frame failed,errcode:%d",ret.err_cod);
        return C2_CORRUPTED;
    }

    pOutFrameInfo->FrameType = FRAMETYPE_P;
    if (ret.is_key_frame) {
        ALOGD("is_key_frame:%d",ret.is_key_frame);
        pOutFrameInfo->FrameType = FRAMETYPE_IDR;
    }
    return C2_OK;
}


void C2VencW420New::Close()  {
    if (!mCodecHandle) {
        return;
    }
    mDestroyFunc(mCodecHandle);
    mCodecHandle = 0;
    return;
}


class C2VencW420NewFactory : public C2ComponentFactory {
public:
    C2VencW420NewFactory() : mHelper(std::static_pointer_cast<C2ReflectorHelper>(
        GetCodec2VendorComponentStore()->getParamReflector())) {
    }

    virtual c2_status_t createComponent(
            c2_node_id_t id,
            std::shared_ptr<C2Component>* const component,
            std::function<void(C2Component*)> deleter) override {
        UNUSED(deleter);
        ALOGV("in %s", __func__);
        *component = C2VencW420New::create((char *)COMPONENT_NAME, id, std::make_shared<C2VencW420New::IntfImpl>(mHelper));
        return *component ? C2_OK : C2_NO_MEMORY;
        /*
        *component = std::shared_ptr<C2Component>(
                new C2VencW420(COMPONENT_NAME,
                                 id,
                                 std::make_shared<C2VencW420::IntfImpl>(mHelper)));
        return C2_OK;*/
    }

    virtual c2_status_t createInterface(
            c2_node_id_t id,
            std::shared_ptr<C2ComponentInterface>* const interface,
            std::function<void(C2ComponentInterface*)> deleter) override {
        ALOGV("in %s", __func__);
        UNUSED(deleter);
        *interface = std::shared_ptr<C2ComponentInterface>(
                new SimpleInterface<C2VencW420New::IntfImpl>(
                        COMPONENT_NAME, id, std::make_shared<C2VencW420New::IntfImpl>(mHelper)));
        return C2_OK;
    }

    virtual ~C2VencW420NewFactory() override = default;

private:
    std::shared_ptr<C2ReflectorHelper> mHelper;
};

}

__attribute__((cfi_canonical_jump_table))
extern "C" ::C2ComponentFactory* CreateC2VencW420NewFactory() {
    ALOGV("in %s", __func__);
    return new ::android::C2VencW420NewFactory();
}

__attribute__((cfi_canonical_jump_table))
extern "C" void DestroyC2VencW420NewFactory(::C2ComponentFactory* factory) {
    ALOGV("in %s", __func__);
    delete factory;
}


