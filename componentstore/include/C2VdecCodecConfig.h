/*
 * Copyright (c) 2021 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */
#ifndef _C2_VENDOR_CODEC_CONFIG_H
#define _C2_VENDOR_CODEC_CONFIG_H

#include <stdio.h>
#include <string.h>
#include <string>
#include <C2VendorSupport.h>
#include <utils/Singleton.h>
#include <json/json.h>
#include <media/stagefright/xmlparser/MediaCodecsXmlParser.h>

namespace android {

enum FeatureIndex {
    CC_SUBTITLE_SUPPORT = 0,
    DECODER_INFO_REPORT,
    GAME_MODE_SUPPORT,
    I_ONLY,
    DV_SUPPORT,
    DV_MULTI_SUPPORT,
    HDR_SUPPORT,
    MAX_RESOLUTION,
    CLOCK_FREQUENCY,
    DECODER_FCC_SUPPORT,
    REQ_UCODE_VERSION,
    V4L_DEC_NR,
    DMA_BUF_HEAP,
    ES_DMA_MODE,
    DECODER_DOUBLE_WRITE,
    FEATURE_MAX = DECODER_DOUBLE_WRITE + 1
};

class C2VdecCodecConfig : public Singleton<C2VdecCodecConfig> {
public:
    C2VdecCodecConfig();
    virtual ~C2VdecCodecConfig();
    bool JsonValueToCodecsMap(Json::Value& json);
    bool codecSupport(C2VendorCodec type, bool secure, bool fromFeatureList, bool fromMediaCodecXml);
    bool codecFeatureSupport(C2VendorCodec codec_type, FeatureIndex feature_type);

    C2VendorCodec adaptorInputCodecToVendorCodec(InputCodec codec);

    // check xml param.
    bool isCodecSupportFrameRate(C2VendorCodec codec_type, bool secure, int32_t width, int32_t height, float frameRate);

    enum ValType {
        TYPE_INVALID = 0,
        TYPE_INT,
        TYPE_BOOL,
        TYPE_STRING,
        TYPE_STRING_ARRAY,
        TYPE_INT_ARRAY,
    };

    struct Feature {
        Feature(int32_t index_, std::string name_, int32_t val_):
            index(index_), name(name_), type(TYPE_INT), ival(val_) {}
        Feature(int32_t index_, std::string name_, std::string val_):
            index(index_), name(name_), type(TYPE_STRING), sval(val_) {}
        Feature(int32_t index_, std::string name_, bool val_):
            index(index_), name(name_), type(TYPE_BOOL), bval(val_) {}
        Feature(int32_t index_, std::string name_, std::vector<std::string> val_):
            index(index_), name(name_), type(TYPE_STRING_ARRAY) {svalStringArray.assign(val_.begin(), val_.end());}
        Feature(int32_t index_, std::string name_, std::vector<int32_t> val_):
            index(index_), name(name_), type(TYPE_INT_ARRAY) {svalIntArray.assign(val_.begin(), val_.end());}
        ~Feature() = default;

        int32_t index = 0;
        std::string name;
        ValType type = ValType::TYPE_INVALID;
        int32_t ival = 0;
        std::string sval;
        bool bval = false;
        std::vector<std::string> svalStringArray;
        std::vector<int> svalIntArray;
    };

    struct Size {
        int32_t w;
        int32_t h;
    };
    struct Range {
        int32_t min;
        int32_t max;
    };
    struct CodecAttributes {
        std::string typeName;

        int adaptivePlayback;
        int tunnelPlayback;
        int lowLatency;
        int32_t concurrentInstance;

        Size blockSize;
        Size alignMent;
        Size minSize;
        Size maxSize;

        Range blockCount;
        Range blocksPerSecond;
        Range bitRate;
    };

private:
    char* getCodecFeatures();
    bool codecsMapToString();
    bool codecSupportFromMediaCodecXml(C2VendorCodec type, bool secure);
    bool codecSupportFromFeatureList(C2VendorCodec type);

    MediaCodecsXmlParser mParser;
    std::map<const char*, std::vector<Feature>> mCodecsMap;
    std::map<const char*, CodecAttributes> mCodecAttributes;
};

}
#endif
