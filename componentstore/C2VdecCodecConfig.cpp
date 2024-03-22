/*
 * Copyright (C) 2023 Amlogic, Inc. All rights reserved.
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
#define LOG_TAG "C2VdecCodecConfig"

#include <stdio.h>
#include <dlfcn.h>
#include <unistd.h>
#include <utils/Log.h>
#include <C2VdecCodecConfig.h>
#include <C2VendorDebug.h>
#include <C2VendorProperty.h>
#include <C2VendorVideoSupport.h>
#include <AmVideoDecBase.h>

#undef TEST
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#endif

#define GetFeatureDataType(t, o) \
    do {\
        for (int i = 0; i < ARRAY_SIZE(gCodecFeatures); i++) {\
            if (gCodecFeatures[i].index == t)\
                o = gCodecFeatures[i].featureType;\
        }\
    } while (0)

#define GetFeatureName(t, o) \
    do {\
        for (int i = 0; i < ARRAY_SIZE(gCodecFeatures); i++) {\
            if (gCodecFeatures[i].index == t)\
                o = gCodecFeatures[i].featureName;\
        }\
    } while (0)

#define GetDecName(t, o) \
    do {\
        for (int i = 0; i < ARRAY_SIZE(gVdecCodecConfig); i++) {\
            if (gVdecCodecConfig[i].type == t)\
                o = gVdecCodecConfig[i].codecDecName;\
        }\
    } while (0)

#define GetCompName(t, s, o) \
    do {\
        if (s) {\
            for (int i = 0; i < ARRAY_SIZE(gSecureVdecCodecConfig); i++) {\
                if (gSecureVdecCodecConfig[i].type == t)\
                    o = gSecureVdecCodecConfig[i].codecCompName.c_str();\
            }\
        } else {\
            for (int i = 0; i < ARRAY_SIZE(gVdecCodecConfig); i++) {\
                if (gVdecCodecConfig[i].type == t)\
                    o = gVdecCodecConfig[i].codecCompName.c_str();\
            }\
        }\
    } while (0)

//decoder module name
const char* kH264ModuleName = "ammvdec_h264_v4l";
const char* kH265ModuleName = "ammvdec_h265_v4l";
const char* kVP9ModuleName = "ammvdec_vp9_v4l";
const char* kAV1ModuleName = "ammvdec_av1_v4l";
const char* kDVHEModuleName = "ammvdec_h265_v4l";
const char* kDVAVModuleName = "ammvdec_h264_v4l";
const char* kDVAV1ModuleName = "ammvdec_av1_v4l";
const char* kMP2ModuleName = "ammvdec_mpeg12_v4l";
const char* kMP4ModuleName = "ammvdec_mpeg4_v4l";
const char* kMJPGModuleName = "ammvdec_mjpeg_v4l";
const char* kAVS3ModuleName = "ammvdec_avs3_v4l";
const char* kAVS2ModuleName = "ammvdec_avs2_v4l";
const char* kAVSModuleName = "ammvdec_avs_v4l";
const char* kVC1ModuleName = "ammvdec_vc1_v4l";

//decoder feature name
const char* kFeatureCCSubtitle = "CC subtitle";
const char* kFeatureDecoderInfoReport = "Decoder information report";
const char* kFeatureGameMode = "GameMode";
const char* kFeatureIOnly = "I only mode";
const char* kFeatureDVSupport = "DolbyVision";
const char* kFeatureMultiSupport = "multi_frame_dv";
const char* kFeatureHDRSupport = "HDR";
const char* kFeatureMaxResolution = "MaximumResolution";
const char* kFeatureClockFrequency = "ClockFrequency";
const char* kFeatureDecoderFccSupport = "Decoder FCC support";
const char* kFeatureReqUcodeVersion = "UcodeVersionRequest";
const char* kFeatureV4lDecNR = "V4ldec nr";
const char* kFeatureDmaBufHeap = "DMA buffer heap";
const char* kFeatureEsDmaMode = "Es dma mode";
const char* kFeatureDoubleWrite = "DoubleWrite";

namespace android {

//featureList
static struct {
    FeatureIndex index;
    const char* featureName;
    C2VdecCodecConfig::ValType featureType;
} gCodecFeatures [] = {
    {CC_SUBTITLE_SUPPORT, kFeatureCCSubtitle, C2VdecCodecConfig::TYPE_BOOL},
    {DECODER_INFO_REPORT, kFeatureDecoderInfoReport, C2VdecCodecConfig::TYPE_BOOL},
    {GAME_MODE_SUPPORT, kFeatureGameMode, C2VdecCodecConfig::TYPE_BOOL},
    {I_ONLY, kFeatureIOnly, C2VdecCodecConfig::TYPE_BOOL},
    {DV_SUPPORT, kFeatureDVSupport, C2VdecCodecConfig::TYPE_BOOL},
    {DV_MULTI_SUPPORT, kFeatureMultiSupport, C2VdecCodecConfig::TYPE_BOOL},
    {HDR_SUPPORT, kFeatureHDRSupport, C2VdecCodecConfig::TYPE_BOOL},
    {MAX_RESOLUTION, kFeatureMaxResolution, C2VdecCodecConfig::TYPE_STRING},
    {CLOCK_FREQUENCY, kFeatureClockFrequency, C2VdecCodecConfig::TYPE_STRING},
    {DECODER_FCC_SUPPORT, kFeatureDecoderFccSupport, C2VdecCodecConfig::TYPE_BOOL},
    {REQ_UCODE_VERSION, kFeatureReqUcodeVersion, C2VdecCodecConfig::TYPE_STRING},
    {V4L_DEC_NR, kFeatureV4lDecNR, C2VdecCodecConfig::TYPE_BOOL},
    {DMA_BUF_HEAP, kFeatureDmaBufHeap, C2VdecCodecConfig::TYPE_BOOL},
    {ES_DMA_MODE, kFeatureEsDmaMode, C2VdecCodecConfig::TYPE_BOOL},
    {DECODER_DOUBLE_WRITE, kFeatureDoubleWrite, C2VdecCodecConfig::TYPE_STRING_ARRAY},
};

//codec config
static struct {
    C2VendorCodec type;
    const C2String codecCompName;
    const char* codecDecName;
} gVdecCodecConfig[] = {
    {C2VendorCodec::VDEC_H264, kH264DecoderName, kH264ModuleName},
    {C2VendorCodec::VDEC_H265, kH265DecoderName, kH265ModuleName},
    {C2VendorCodec::VDEC_VP9, kVP9DecoderName, kVP9ModuleName},
    {C2VendorCodec::VDEC_AV1, kAV1DecoderName, kAV1ModuleName},
    {C2VendorCodec::VDEC_DVHE, kDVHEDecoderName, kH265ModuleName},
    {C2VendorCodec::VDEC_DVAV, kDVAVDecoderName, kH264ModuleName},
    {C2VendorCodec::VDEC_DVAV1, kDVAV1DecoderName, kAV1ModuleName},
    {C2VendorCodec::VDEC_MP2V, kMP2VDecoderName, kMP2ModuleName},
    {C2VendorCodec::VDEC_MP4V, kMP4VDecoderName, kMP4ModuleName},
    {C2VendorCodec::VDEC_MJPG, kMJPGDecoderName, kMJPGModuleName},
#ifdef  SUPPORT_VDEC_AVS3
    {C2VendorCodec::VDEC_AVS3, kAVS3DecoderName, kAVS3ModuleName},
#endif
#ifdef  SUPPORT_VDEC_AVS2
    {C2VendorCodec::VDEC_AVS2, kAVS2DecoderName, kAVS2ModuleName},
#endif
#ifdef  SUPPORT_VDEC_AVS
    {C2VendorCodec::VDEC_AVS, kAVSDecoderName, kAVSModuleName},
#endif
    {C2VendorCodec::VDEC_HW_VC1, kHWVC1DecoderName, kVC1ModuleName}
};

static struct {
    C2VendorCodec type;
    const C2String codecCompName;
    const char* codecDecName;
} gSecureVdecCodecConfig[] = {
    {C2VendorCodec::VDEC_H264, kH264SecureDecoderName, kH264ModuleName},
    {C2VendorCodec::VDEC_H265, kH265SecureDecoderName, kH265ModuleName},
    {C2VendorCodec::VDEC_VP9, kVP9SecureDecoderName, kVP9ModuleName},
    {C2VendorCodec::VDEC_AV1, kAV1SecureDecoderName, kAV1ModuleName},
    {C2VendorCodec::VDEC_DVHE, kDVHESecureDecoderName, kH265ModuleName},
    {C2VendorCodec::VDEC_DVAV, kDVAVSecureDecoderName, kH264ModuleName},
    {C2VendorCodec::VDEC_DVAV1, kDVAV1SecureDecoderName, kDVAV1ModuleName},
    {C2VendorCodec::VDEC_MP2V, kMP2VSecureDecoderName, kMP2ModuleName},
};

ANDROID_SINGLETON_STATIC_INSTANCE(C2VdecCodecConfig)

#ifdef TEST
#define FEATURES_LIST_SIZE 4096
bool getFeatureList(const char* file_path, char **outbuf) {
    FILE* fd = fopen(file_path, "r");
    if (!fd) {
        ALOGE("open file %s failed, err %s",file_path,  strerror(errno));
        return false;
    }
    fread(*outbuf, FEATURES_LIST_SIZE, 1, fd);
    fclose(fd);
    ALOGV("json:%s", *outbuf);

    return true;
}
#endif

bool StringToJsonValue(const std::string& inStr, Json::Value& out) {
    bool ret = false;
    JSONCPP_STRING err;
    Json::CharReaderBuilder builder;

    if (!inStr.empty()) {
        std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        ret = reader->parse(inStr.data(), inStr.data() + inStr.length(), &out, &err);
    }
    return ret;
}

C2VdecCodecConfig::C2VdecCodecConfig():
    mParser() {
    (void)mParser.parseXmlFilesInSearchDirs();
    (void)mParser.parseXmlPath(mParser.defaultProfilingResultsXmlPath);

    mDecoderFeatureInfo.data = NULL;
    propGetInt(CODEC2_VDEC_LOGDEBUG_PROPERTY, &gloglevel);
    Json::Value value;
    char *featureListData = NULL;
#ifdef TEST
    featureListData = (char*)malloc(FEATURES_LIST_SIZE);
    getFeatureList("/data/featureList.json", &featureListData);
#else
    featureListData = getCodecFeatures();
    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "json:%s", featureListData);
#endif
    if (featureListData)
        StringToJsonValue(std::string(featureListData), value);
    JsonValueToCodecsMap(value);
    if (mDecoderFeatureInfo.data) {
        free(mDecoderFeatureInfo.data);
        mDecoderFeatureInfo.data = NULL;
    }
#ifdef TEST
    free(featureListData);
#endif
}

C2VdecCodecConfig::~C2VdecCodecConfig() {
    mCodecsMap.clear();
    if (mDecoderFeatureInfo.data) {
        free(mDecoderFeatureInfo.data);
        mDecoderFeatureInfo.data = NULL;
    }
}


C2VendorCodec C2VdecCodecConfig::adaptorInputCodecToVendorCodec(InputCodec codec) {
    switch (codec) {
        case InputCodec::H264:
            return C2VendorCodec::VDEC_H264;
        case InputCodec::H265:
            return C2VendorCodec::VDEC_H265;
        case InputCodec::VP9:
            return C2VendorCodec::VDEC_VP9;
        case InputCodec::AV1:
            return C2VendorCodec::VDEC_AV1;
        case InputCodec::DVHE:
            return C2VendorCodec::VDEC_DVHE;
        case InputCodec::DVAV:
            return C2VendorCodec::VDEC_DVAV;
        case InputCodec::DVAV1:
            return C2VendorCodec::VDEC_DVAV1;
        case InputCodec::MP2V:
            return C2VendorCodec::VDEC_MP2V;
        case InputCodec::MP4V:
            return C2VendorCodec::VDEC_MP4V;
        case InputCodec::MJPG:
            return C2VendorCodec::VDEC_MJPG;
#ifdef SUPPORT_VDEC_AVS3
        case InputCodec::AVS3:
            return C2VendorCodec::VDEC_AVS3;
#endif
#ifdef SUPPORT_VDEC_AVS2
        case InputCodec::AVS2:
            return C2VendorCodec::VDEC_AVS2;
#endif
#ifdef SUPPORT_VDEC_AVS
        case InputCodec::AVS:
            return C2VendorCodec::VDEC_AVS;
#endif
        case InputCodec::VC1:
            return C2VendorCodec::VDEC_HW_VC1;
        case InputCodec::UNKNOWN:
            return C2VendorCodec::UNKNOWN;
        default:
            return C2VendorCodec::UNKNOWN;
    }
}

char* C2VdecCodecConfig::getCodecFeatures() {
    static void* video_dec = NULL;
    typedef uint32_t (*getVideoDecoderFeatureListFunc)(decoder_info_parameter type, void* arg);
    static getVideoDecoderFeatureListFunc getFeatureList = NULL;

    if (mDecoderFeatureInfo.data) {
        return (char*)mDecoderFeatureInfo.data;
    }

    if (video_dec == NULL) {
        video_dec = dlopen("libmediahal_videodec.so", RTLD_NOW);
        if (!video_dec) {
            ALOGE("Unable to dlopen libmediahal_videodec: %s", dlerror());
            return NULL;
        }
    }

    getFeatureList = (getVideoDecoderFeatureListFunc)dlsym(video_dec, "AmVideoDec_getVideoDecoderInfo");
    if (getFeatureList != NULL) {
        int featureListSize = 0;
        //get feature size
        (*getFeatureList)(GET_DECODER_FEATURE_LIST_SIZE, (void*)(&featureListSize));
        if (featureListSize > 0) {
            mDecoderFeatureInfo.data = (uint8_t *)malloc(featureListSize);
            memset(mDecoderFeatureInfo.data, 0, featureListSize);
            if (mDecoderFeatureInfo.data == nullptr) {
                ALOGE("%s:%d malloc %d fail", __func__, __LINE__, featureListSize);
                return NULL;
            }
            mDecoderFeatureInfo.data_len = featureListSize;
            //get featurelist
            (*getFeatureList)(GET_DECODER_FEATURE_LIST, (void*)&mDecoderFeatureInfo);
        } else {
            ALOGD("read none, ignore");
        }

        memset(&mDisplayInfo, 0, sizeof(mDisplayInfo));
        (*getFeatureList)(GET_DISPLAY_INFO_VIDEO_MAX_SIZE, (void*)(&(mDisplayInfo.maxSize)));
        if (mDisplayInfo.maxSize.w * mDisplayInfo.maxSize.h != 0) {
            ALOGI("display max size %dx%d", mDisplayInfo.maxSize.w, mDisplayInfo.maxSize.h);
            if ((mDisplayInfo.maxSize.w * mDisplayInfo.maxSize.h) > (4096 * 2304)) {
                    mDisplayInfo.isSupport8k = true;
            }
        }
        return (char*)mDecoderFeatureInfo.data;
    }

    return NULL;
}

bool C2VdecCodecConfig::JsonValueToCodecsMap(Json::Value& json) {
    for (int i = (int)C2VendorCodec::VDEC_H264; i <= (int)C2VendorCodec::VDEC_TYPE_MAX; i++) {
        C2VendorCodec codec_type = (C2VendorCodec)i;
        const char* codec_name = NULL;
        GetDecName(codec_type, codec_name);
        if (codec_name == NULL) {
            CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2,"codecname is null and return.");
            return false;
        }

        if (json.isMember(codec_name)) {
            CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2,"codecname:%s", codec_name);
            Json::Value& subVal = json[codec_name];
            std::vector<Feature> vec;
            for (int j = 0; j < (int)FEATURE_MAX ; j++) {
                FeatureIndex feature_index = (FeatureIndex)j;
                const char* feature_name = NULL;
                GetFeatureName(feature_index, feature_name);
                if (subVal.isMember(feature_name)) {
                    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2,"featureName:%s", feature_name);
                    ValType type = TYPE_INVALID;
                    GetFeatureDataType(feature_index, type);
                    if ((type == TYPE_INT) && subVal[feature_name].isInt()) {
                        int32_t val = (int32_t)subVal[feature_name].asInt();
                        vec.push_back(Feature(feature_index, feature_name, val));
                    } else if ((type == TYPE_BOOL) && subVal[feature_name].isBool()) {
                        bool val = subVal[feature_name].asBool();
                        vec.push_back(Feature(feature_index, feature_name, val));
                    } else if ((type == TYPE_STRING) && subVal[feature_name].isString()) {
                        std::string val(subVal[feature_name].asString());
                        vec.push_back(Feature(feature_index, feature_name, val));
                    } else if ((type == TYPE_INT_ARRAY) && subVal[feature_name].isArray()) {
                        std::vector<int> valVec;
                        int arrayVal = 0;
                        for (int k = 0; k < subVal[feature_name].size(); k++) {
                            if (subVal[feature_name][k].isInt()) {
                                arrayVal = subVal[feature_name][k].asInt();
                                valVec.push_back(arrayVal);
                            } else {
                                CODEC2_LOG(CODEC2_LOG_ERR,"%s:%d can not parse %s int type, please check", __func__, __LINE__, feature_name);
                                break;
                            }
                        }
                        vec.push_back(Feature(feature_index, feature_name, valVec));
                    } else if ((type == TYPE_STRING_ARRAY) && subVal[feature_name].isArray()) {
                        std::vector<std::string> valVec;
                        for (int k = 0; k < subVal[feature_name].size(); k++) {
                            if (subVal[feature_name][k].isString()) {
                                std::string val(subVal[feature_name][k].asString());
                                valVec.push_back(val);
                            } else {
                                CODEC2_LOG(CODEC2_LOG_ERR, "%s:%d can not parse %s string type, please check", __func__, __LINE__, feature_name);
                                break;
                            }
                        }
                        vec.push_back(Feature(feature_index, feature_name, valVec));
                    } else {
                        CODEC2_LOG(CODEC2_LOG_ERR, "%s:%d can not parse %s, please check",  __func__, __LINE__, feature_name);
                    }
                }
            }
            if (codec_name != NULL)
                mCodecsMap.insert(std::pair<const char*, std::vector<Feature>>(codec_name, vec));
        }
    }
    codecsMapToString();

    return true;
}

bool C2VdecCodecConfig::codecsMapToString() {
    auto iter1 = mCodecsMap.begin();
    while (iter1 != mCodecsMap.end()) {
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "codec type:%s", iter1->first);
        auto iter2 = iter1->second.begin();
        while (iter2 != iter1->second.end()) {
            if (iter2->type == TYPE_STRING) {
                CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "\t%s:%s", iter2->name.c_str(), iter2->sval.c_str());
            } else if (iter2->type == TYPE_INT) {
                CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "\t%s:%d", iter2->name.c_str(), iter2->ival);
            } else if (iter2->type == TYPE_BOOL) {
                CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "\t%s:%s", iter2->name.c_str(), ((iter2->bval) ? "true" : "false"));
            } else if (iter2->type == TYPE_STRING_ARRAY) {
                CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "\t%s", iter2->name.c_str());
                auto iter3 = iter2->svalStringArray.begin();
                while (iter3 != iter2->svalStringArray.end()) {
                    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "\t\t%s", iter3->c_str());;
                    iter3 ++;
                }
            } else if (iter2->type == TYPE_INT_ARRAY) {
                CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "\t%s", iter2->name.c_str());
                auto iter3 = iter2->svalIntArray.begin();
                while (iter3 != iter2->svalIntArray.end()) {
                    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "\t\t%d", *iter3);
                    iter3++;
                }
            }
            iter2++;
        }
        iter1++;
    }

    return true;
}

bool C2VdecCodecConfig::codecSupportFromFeatureList(C2VendorCodec type) {
    /* check from decoder featurelist */
    const char* decName = NULL;
    GetDecName(type, decName);
    auto iter = mCodecsMap.find(decName);
    if (mCodecsMap.size() > 0)
        return ((iter != mCodecsMap.end()) ? true: false);
    else
        return true;
}

bool C2VdecCodecConfig::codecFeatureSupport(C2VendorCodec codec_type, FeatureIndex feature_type) {
    const char* featureName = NULL;
    GetDecName(codec_type, featureName);
    auto iter = mCodecsMap.find(featureName);
    if (iter == mCodecsMap.end()) {
        return false;
    }
    auto iter2 = std::find_if(iter->second.begin(), iter->second.end(),
            [feature_type] (const Feature& feature) {
            return feature.index == feature_type;});
    if (iter2 == iter->second.end()) {
        return false;
    }

    return true;
}

bool C2VdecCodecConfig::codecSupportFromMediaCodecXml(C2VendorCodec type, bool secure)  {
    const char* name = NULL;
    GetCompName(type, secure, name);

    if (name == NULL) {
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1, "%d not support from media codec xml", type);
        return false;
    }
    const auto& codec = mParser.getCodecMap().find(name);
    if (codec == mParser.getCodecMap().cend()) {
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1, "%s not support from media codec xml", name);
        return false;
    }

    const MediaCodecsXmlParser::TypeMap map = codec->second.typeMap;
    auto typemap = map.begin();
    while (typemap != map.end()) {
        auto attributemap = typemap->second.begin();
        struct CodecAttributes attributeItem;
        memset(&attributeItem, 0, sizeof(attributeItem));
        attributeItem.typeName = typemap->first.c_str();
        attributeItem.isSupport8k = false;

        while (attributemap != typemap->second.end()) {
            CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s] %s %s -- %s", __func__, name, attributemap->first.c_str(), attributemap->second.c_str());
            if (strstr(attributemap->first.c_str(), "alignment") != NULL) {
                sscanf(attributemap->second.c_str(), "%dx%d", &attributeItem.alignMent.w, &attributeItem.alignMent.h);
            } else if (strstr(attributemap->first.c_str(), "bitrate-range") != NULL) {
                sscanf(attributemap->second.c_str(), "%d-%d", &attributeItem.bitRate.min, &attributeItem.bitRate.max);
            } else if (strstr(attributemap->first.c_str(), "block-count-range") != NULL) {
                sscanf(attributemap->second.c_str(), "%d-%d", &attributeItem.blockCount.min, &attributeItem.blockCount.max);
            } else if (strstr(attributemap->first.c_str(), "block-size") != NULL) {
                sscanf(attributemap->second.c_str(), "%dx%d", &attributeItem.blockSize.w, &attributeItem.blockSize.h);
            } else if (strstr(attributemap->first.c_str(), "size") != NULL) {
                sscanf(attributemap->second.c_str(),"%dx%d-%dx%d",&attributeItem.minSize.w, &attributeItem.minSize.h, &attributeItem.maxSize.w, &attributeItem.maxSize.h);
                if ((attributeItem.maxSize.w*attributeItem.maxSize.h >= 7680*4320) && property_get_bool(PROPERTY_PLATFORM_SUPPORT_8K, true)) {
                    attributeItem.isSupport8k = true;
                }
            } else if (strstr(attributemap->first.c_str(), "blocks-per-second-range") != NULL) {
                sscanf(attributemap->second.c_str(), "%d-%d", &attributeItem.blocksPerSecond.min, &attributeItem.blocksPerSecond.max);
            } else if (strstr(attributemap->first.c_str(), "feature-adaptive-playback") != NULL) {
                sscanf(attributemap->second.c_str(), "%d", &attributeItem.adaptivePlayback);
            } else if (strstr(attributemap->first.c_str(), "feature-low-latency") != NULL) {
                sscanf(attributemap->second.c_str(), "%d", &attributeItem.lowLatency);
            } else if (strstr(attributemap->first.c_str(), "max-concurrent-instances") != NULL) {
                sscanf(attributemap->second.c_str(), "%d", &attributeItem.concurrentInstance);
            } else if (strstr(attributemap->first.c_str(), "feature-tunneled-playback") != NULL) {
                sscanf(attributemap->second.c_str(), "%d", &attributeItem.tunnelPlayback);
            }

            attributemap ++;
        }

        mCodecAttributes.insert(std::pair<const char*, CodecAttributes>(name, attributeItem));
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s] alignment(%d %d) blocksize-range(%d %d) minSize(%d %d) maxSize(%d %d) isSupport8k: %d",
                name,
                attributeItem.alignMent.w,attributeItem.alignMent.h,
                attributeItem.blockSize.w,attributeItem.blockSize.h,
                attributeItem.minSize.w,attributeItem.minSize.h,
                attributeItem.maxSize.w,attributeItem.maxSize.h,
                attributeItem.isSupport8k);
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "blockCount(%d %d) blocksPerSecond(%d %d) bitRate(%d %d) adaptivePlayback(%d) tunnelPlayback(%d) lowLatency(%d) concurrentInstance(%d)",
                attributeItem.blockCount.min, attributeItem.blockCount.max,
                attributeItem.blocksPerSecond.min, attributeItem.blocksPerSecond.max,
                attributeItem.bitRate.min,attributeItem.bitRate.max,
                attributeItem.adaptivePlayback,attributeItem.tunnelPlayback,
                attributeItem.lowLatency, attributeItem.concurrentInstance);
        typemap++;
    }

    return true;
}

bool C2VdecCodecConfig::codecSupport(C2VendorCodec type, bool secure, bool fromFeatureList, bool fromMediaCodecXml) {
    if (fromFeatureList && !fromMediaCodecXml)
        return codecSupportFromFeatureList(type);
    else if (fromMediaCodecXml && !fromFeatureList)
        return codecSupportFromMediaCodecXml(type, secure);
    else if (fromFeatureList && fromMediaCodecXml)
        return codecSupportFromFeatureList(type) & codecSupportFromMediaCodecXml(type, secure);
    else
        return true;
}

bool C2VdecCodecConfig::isCodecSupportFrameRate(C2VendorCodec codec_type, bool secure, int32_t width, int32_t height, float frameRate) {
    const char* name = NULL;
    GetCompName(codec_type, secure, name);
    int32_t size = width * height;
    bool support_4k = property_get_bool(PROPERTY_PLATFORM_SUPPORT_4K, true);
    int32_t support_4k_fps_max = property_get_int32(PROPERTY_PLATFORM_SUPPORT_4K_FPS_MAX, 0);

    if (name == NULL) {
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2,"codecname is null and return.");
        return false;
    }

    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "%s name:%s secure:%d size:%dx%d frameRate:%f", __func__, name, secure, width, height, frameRate);
    auto attribute = mCodecAttributes.find(name);
    if (attribute == mCodecAttributes.end()) {
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2,"don't found %s.", name);
        return false;
    }

    if ((3840 * 2160 <= size && size <= 4096 * 2304) && support_4k) {
        struct CodecAttributes codecAttributes = attribute->second;
        float supportFrameRate = (float)codecAttributes.blocksPerSecond.max / (float)codecAttributes.blockCount.max;
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2,"%s supported framerate:%f framerate:%f blocksPerSecond:%d blockCount:%d", __func__, supportFrameRate, frameRate,
            codecAttributes.blocksPerSecond.max, codecAttributes.blockCount.max);
        if (((codec_type == C2VendorCodec::VDEC_H265) || (codec_type == C2VendorCodec::VDEC_VP9) || (codec_type == C2VendorCodec::VDEC_AV1))
            && (frameRate <= support_4k_fps_max))
            return true;

        if (frameRate <= supportFrameRate) {
            return true;
        } else {
            return false;
        }
    }

    return true;
}

bool C2VdecCodecConfig::isMaxResolutionFromXml(C2VendorCodec codec_type, bool secure, int32_t width, int32_t height) {
    const char* name = NULL;
    GetCompName(codec_type, secure, name);

    if (name == NULL) {
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2,"codecname is null and return.");
        return false;
    }

    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "%s name:%s secure:%d size:%dx%d", __func__, name, secure, width, height);
    auto attribute = mCodecAttributes.find(name);
    if (attribute == mCodecAttributes.end()) {
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2,"don't found %s.", name);
        return false;
    }

    struct CodecAttributes codecAttributes = attribute->second;

    int32_t maxSize = codecAttributes.blockCount.max * codecAttributes.blockSize.h * codecAttributes.blockSize.w;
    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "%s name:%s  max size:%d", __func__, name,  maxSize);
    /*Now only check max resolution defined from media_codec xml*/
    if ((width * height) == maxSize) {
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2,"%s use Max Resolution.", name);
        return true;
    }
    return false;
}

bool C2VdecCodecConfig::getMinMaxResolutionFromXml(C2VendorCodec codec_type, bool secure, struct Size& min, struct Size& max) {
    const char* name = NULL;
    GetCompName(codec_type, secure, name);

    if (name == NULL) {
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2,"codecname is null and return.");
        return false;
    }

    auto attribute = mCodecAttributes.find(name);
    if (attribute == mCodecAttributes.end()) {
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2,"don't found %s.", name);
        return false;
    }

    struct CodecAttributes codecAttributes = attribute->second;

    min = codecAttributes.minSize;
    max = codecAttributes.maxSize;
    return true;
}


bool C2VdecCodecConfig::isCodecSupport8k(C2VendorCodec codec_type, bool secure) {
    const char* name = nullptr;
    GetCompName(codec_type, secure, name);
    auto attribute = mCodecAttributes.find(name);
    if (attribute == mCodecAttributes.end()) {
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2,"don't found %s.", name);
        return false;
    }
    return attribute->second.isSupport8k;
}
bool C2VdecCodecConfig::isDisplaySupport8k() {
    return mDisplayInfo.isSupport8k ;
}

c2_status_t C2VdecCodecConfig::isCodecSupportResolutionRatio(InputCodec codec, bool secure, int32_t bufferSize) {
    bool support_4k = property_get_bool(PROPERTY_PLATFORM_SUPPORT_4K, true);
    bool support_8k = property_get_bool(PROPERTY_PLATFORM_SUPPORT_8K, true);

    c2_status_t ret = C2_OK;
    if ((bufferSize > (1920 * 1088)) && !support_4k) {
        CODEC2_LOG(CODEC2_LOG_ERR,"%s:%d not support 4K for non-4K platform, config failed, please check", __func__, __LINE__);
        ret = C2_BAD_VALUE;
    }
    C2VendorCodec vendorCodec = adaptorInputCodecToVendorCodec(codec);
    if ((bufferSize > (4096 * 2304)) &&
        (!isCodecSupport8k(vendorCodec, secure) && !support_8k)) {
        CODEC2_LOG(CODEC2_LOG_ERR,"%s:%d not support 8K for non-8K platform, config failed, please check", __func__, __LINE__);
        ret = C2_BAD_VALUE;
    }
    return ret;
}


}
