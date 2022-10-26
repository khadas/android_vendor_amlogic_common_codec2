/*
 * Copyright (c) 2021 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "C2VdecCodecConfig"

#include <dlfcn.h>
#include <unistd.h>
#include <utils/Log.h>
#include <C2VdecCodecConfig.h>
#include <c2logdebug.h>
#include <C2VendorProperty.h>

//#include <media/stagefright/xmlparser/MediaCodecsXmlParser.h>

#undef TEST
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#endif

#define GetFeatureDataType(t, o) \
    do {\
        for (int i = 0; i < ARRAY_SIZE(gCodecFeatures); i++) {\
            if (gCodecFeatures[i].index == t)\
                o = gCodecFeatures[i].featueType;\
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

#define GetCompName(t, o) \
    do {\
        for (int i = 0; i < ARRAY_SIZE(gVdecCodecConfig); i++) {\
            if (gVdecCodecConfig[i].type == t)\
                o = gVdecCodecConfig[i].codecCompName;\
        }\
    } while (0)

namespace android {

//featueList
static struct {
    FeatureIndex index;
    const char* featureName;
    C2VdecCodecConfig::ValType featueType;
} gCodecFeatures [] = {
    {CC_SUBTITLE_SUPPORT, "CC subtitle", C2VdecCodecConfig::TYPE_BOOL},
    {DECODER_INFO_REPORT, "Decoder information report", C2VdecCodecConfig::TYPE_BOOL},
    {GAME_MODE_SUPPORT, "GameMode", C2VdecCodecConfig::TYPE_BOOL},
    {I_ONLY, "I only mode", C2VdecCodecConfig::TYPE_BOOL},
    {DV_SUPPORT, "DolbyVision", C2VdecCodecConfig::TYPE_BOOL},
    {DV_MULTI_SUPPORT, "multi_frame_dv", C2VdecCodecConfig::TYPE_BOOL},
    {HDR_SUPPORT, "HDR", C2VdecCodecConfig::TYPE_BOOL},
    {MAX_RESOLUTION, "MaximumResolution", C2VdecCodecConfig::TYPE_STRING},
    {CLOCK_FREQUENCY, "ClockFrequency", C2VdecCodecConfig::TYPE_STRING},
    {DECODER_FCC_SUPPORT, "Decoder FCC support", C2VdecCodecConfig::TYPE_BOOL},
    {REQ_UCODE_VERSION, "UcodeVersionRequest", C2VdecCodecConfig::TYPE_STRING},
    {V4L_DEC_NR, "V4ldec nr", C2VdecCodecConfig::TYPE_BOOL},
    {DMA_BUF_HEAP, "DMA buffer heap", C2VdecCodecConfig::TYPE_BOOL},
    {ES_DMA_MODE, "Es dma mode", C2VdecCodecConfig::TYPE_BOOL},
    {DOUBLE_WRITE, "DoubleWrite", C2VdecCodecConfig::TYPE_STRING_ARRAY},
};

//codec config
static struct {
    C2VendorCodec type;
    const char* codecCompName;
    const char* codecDecName;
} gVdecCodecConfig[] = {
    {C2VendorCodec::VDEC_H264, "c2.amlogic.avc.decoder", "ammvdec_h264_v4l"},
    {C2VendorCodec::VDEC_H265, "c2.amlogic.hevc.decoder", "ammvdec_h265_v4l"},
    {C2VendorCodec::VDEC_VP9, "c2.amlogic.vp9.decoder", "ammvdec_vp9_v4l"},
    {C2VendorCodec::VDEC_AV1, "c2.amlogic.av1.decoder", "ammvdec_av1_v4l"},
    {C2VendorCodec::VDEC_DVHE, "c2.amlogic.dvhe.decoder", ""},
    {C2VendorCodec::VDEC_DVAV, "c2.amlogic.dvav.decoder", ""},
    {C2VendorCodec::VDEC_DVAV1, "c2.amlogic.dvav1.decoder", ""},
    {C2VendorCodec::VDEC_MP2V, "c2.amlogic.mpeg2.decoder", "ammvdec_mpeg2_v4l"},
    {C2VendorCodec::VDEC_MP4V, "c2.amlogic.mpeg4.decoder", "ammvdec_mpeg4_v4l"},
    {C2VendorCodec::VDEC_MJPG, "c2.amlogic.mjpeg.decoder", "ammvdec_mjpeg_v4l"},
#ifdef  SUPPORT_VDEC_AVS2
    {C2VendorCodec::VDEC_AVS2, "c2.amlogic.avs2.decoder", "ammvdec_avs2_v4l"},
#endif
#ifdef  SUPPORT_VDEC_AVS
    {C2VendorCodec::VDEC_AVS, "c2.amlogic.avs.decoder", "ammvdec_avs_v4l"},
#endif
};

#ifdef TEST
#define FEATRUES_LIST_SIZE 4096
bool getFeatureList(const char* file_path, char **outbuf) {
    FILE* fd = fopen(file_path, "r");
    if (!fd) {
        ALOGE("open file %s failed, err %s",file_path,  strerror(errno));
        return false;
    }
    fread(*outbuf, FEATRUES_LIST_SIZE, 1, fd);
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

C2VdecCodecConfig::C2VdecCodecConfig() {
    propGetInt(CODEC2_VDEC_LOGDEBUG_PROPERTY, &gloglevel);
    Json::Value value;
#ifdef TEST
    char *featureListData = (char*)malloc(FEATRUES_LIST_SIZE);
    getFeatureList("/data/featureList.json", &featureListData);
    if (featureListData)
        StringToJsonValue(std::string(featureListData), value);
#else
    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "json:%s", getCodecFeatures());
    if (getCodecFeatures())
        StringToJsonValue(std::string(getCodecFeatures()), value);
#endif
    JsonValueToCodecsMap(value);
#ifdef TEST
    free(featureListData);
#endif
}

C2VdecCodecConfig::~C2VdecCodecConfig() {
    mCodecsMap.clear();
}

char* C2VdecCodecConfig::getCodecFeatures() {
    static void* video_dec = NULL;
    typedef uint32_t (*getVideoDecodedrFeatureListFunc)(char** data);
    static getVideoDecodedrFeatureListFunc getFeatureList =  NULL;
    char * data = NULL;

    if (video_dec == NULL) {
        video_dec = dlopen("libmediahal_videodec.so", RTLD_NOW);
        if (!video_dec) {
            ALOGE("Unable to dlopen libmediahal_videodec: %s", dlerror());
            return NULL;
        }
        getFeatureList = (getVideoDecodedrFeatureListFunc)dlsym(video_dec, "AmVideoDec_getVideoDecodedrFeatureList");
    }

    if (getFeatureList != NULL) {
        (*getFeatureList)(&data);
        return data;
    }

    return NULL;
}

bool C2VdecCodecConfig::JsonValueToCodecsMap(Json::Value& json) {
    for (int i = (int)C2VendorCodec::VDEC_H264; i <= (int)C2VendorCodec::VDEC_TYPE_MAX; i++) {
        C2VendorCodec codec_type = (C2VendorCodec)i;
        const char* codec_name = NULL;
        GetDecName(codec_type, codec_name);
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
                        for (int z = 0; z < subVal[feature_name].size(); z++) {
                            if (subVal[feature_name][z].isInt()) {
                                arrayVal = subVal[feature_name][z].asInt();
                                valVec.push_back(arrayVal);
                            } else {
                                CODEC2_LOG(CODEC2_LOG_ERR,"%s:%d can not parse %s int type, please check", __func__, __LINE__, feature_name);
                                break;
                            }
                        }
                        vec.push_back(Feature(feature_index, feature_name, valVec));
                    } else if ((type == TYPE_STRING_ARRAY) && subVal[feature_name].isArray()) {
                        std::vector<std::string> valVec;
                        for (int z = 0; z < subVal[feature_name].size(); z++) {
                            if (subVal[feature_name][z].isString()) {
                                std::string val(subVal[feature_name][z].asString());
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
            if (codec_name)
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

bool C2VdecCodecConfig::codecSupport(C2VendorCodec type) {
    /* check from decoder featurelist */
    const char* decName = NULL;
    GetDecName(type, decName);
    auto iter = mCodecsMap.find(decName);
    return ((iter != mCodecsMap.end()) ? true: false);
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

}
