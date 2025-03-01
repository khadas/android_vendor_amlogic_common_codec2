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

#ifndef _C2_VENDOR_CODEC_CONFIG_H
#define _C2_VENDOR_CODEC_CONFIG_H

#include <stdio.h>
#include <string.h>
#include <string>
#include <C2VendorSupport.h>
#include <AmVideoDecBase.h>
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

struct Size {
    int32_t w;
    int32_t h;
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
    bool isCodecSupportPictureSize(C2VendorCodec codec_type, bool secure, int32_t pictureSize);
    bool isMaxResolutionFromXml(C2VendorCodec codec_type, bool secure, int32_t width, int32_t height);
    bool getMinMaxResolutionFromXml(C2VendorCodec codec_type, bool secure, struct Size& min, struct Size& max);

    // Checks whether the specified codec supports 4k on the current platform
    bool isCodecSupport4k(C2VendorCodec codec_type, bool secure);
    // Checks whether the specified codec supports 8k on the current platform
    bool isCodecSupport8k(C2VendorCodec codec_type, bool secure);
    bool isDisplaySupport8k();
    c2_status_t isCodecSupportResolutionRatio(InputCodec codec, bool secure, int32_t bufferSize);
    enum ValType {
        TYPE_INVALID = 0,
        TYPE_INT,
        TYPE_BOOL,
        TYPE_STRING,
        TYPE_STRING_ARRAY,
        TYPE_INT_ARRAY,
    };

private:
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
        bool isSupport8k;
    };
    struct DisplayInfo {
        Size maxSize;
        bool isSupport8k;
    };

    char* getCodecFeatures();
    bool codecsMapToString();
    bool codecSupportFromMediaCodecXml(C2VendorCodec type, bool secure);
    bool codecSupportFromFeatureList(C2VendorCodec type);

    MediaCodecsXmlParser mParser;
    std::map<const char*, std::vector<Feature>> mCodecsMap;
    std::map<const char*, CodecAttributes> mCodecAttributes;
    decoder_feature_info mDecoderFeatureInfo;
    DisplayInfo mDisplayInfo;
};

}
#endif
