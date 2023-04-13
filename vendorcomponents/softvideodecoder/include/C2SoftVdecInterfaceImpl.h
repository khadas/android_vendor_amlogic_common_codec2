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

#ifndef _C2_SOFT_VDEC_INTERFACE_IMPL_H_
#define _C2_SOFT_VDEC_INTERFACE_IMPL_H_

#include <SimpleC2Interface.h>
#include <util/C2InterfaceHelper.h>

#include <C2SoftVdec.h>


namespace android {

class C2SoftVdec::IntfImpl : public C2InterfaceHelper {
public:
    IntfImpl(C2String name, const std::shared_ptr<C2ReflectorHelper>& helper);

    c2_status_t config(
            const std::vector<C2Param*> &params, c2_blocking_t mayBlock,
            std::vector<std::unique_ptr<C2SettingResult>>* const failures,
            bool updateParams = true,
            std::vector<std::shared_ptr<C2Param>> *changes = nullptr);

    std::shared_ptr<C2StreamColorAspectsInfo::output> getColorAspects_l() {
        return this->mColorAspects;
    }

    std::shared_ptr<C2StreamPictureSizeInfo::output> getSize_l() {
        return this->mSize;
    }

    const char* ConvertComponentNameToMimeType(const char* componentName);

private:
    // Configurable parameter setters.
    static C2R ProfileLevelSetter(bool mayBlock, C2P<C2StreamProfileLevelInfo::input>& info);

    static C2R SizeSetter(bool mayBlock, const C2P<C2StreamPictureSizeInfo::output> &oldMe,
                      C2P<C2StreamPictureSizeInfo::output> &me);

    static C2R MaxPictureSizeSetter(bool mayBlock, C2P<C2StreamMaxPictureSizeTuning::output> &me,
                                    const C2P<C2StreamPictureSizeInfo::output> &size);

    static C2R DefaultColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsTuning::output> &me);

    static C2R CodedColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsInfo::input> &me);

    static C2R ColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsInfo::output> &me,
                          const C2P<C2StreamColorAspectsTuning::output> &def,
                          const C2P<C2StreamColorAspectsInfo::input> &coded);
    std::shared_ptr<C2ApiFeaturesSetting> mApiFeatures;
    // The kind of the component; should be C2Component::KIND_ENCODER.
    std::shared_ptr<C2ComponentKindSetting> mKind;
    // The input format kind; should be C2FormatCompressed.
    std::shared_ptr<C2StreamBufferTypeSetting::input> mInputFormat;
    // The output format kind; should be C2FormatVideo.
    std::shared_ptr<C2StreamBufferTypeSetting::output> mOutputFormat;
    // The MIME type of input port.
    std::shared_ptr<C2PortMediaTypeSetting::input> mInputMediaType;
    // The MIME type of output port; should be MEDIA_MIMETYPE_VIDEO_RAW.
    std::shared_ptr<C2PortMediaTypeSetting::output> mOutputMediaType;
    // The input codec profile and level. For now configuring this parameter is useless since
    // the component always uses fixed codec profile to initialize accelerator. It is only used
    // for the client to query supported profile and level values.
    // TODO: use configured profile/level to initialize accelerator.
    std::shared_ptr<C2StreamProfileLevelInfo::input> mProfileLevel;
    // Decoded video size for output.
    std::shared_ptr<C2StreamPictureSizeInfo::output> mSize;
    // Decoded video max size for output.
    std::shared_ptr<C2StreamMaxPictureSizeTuning::output> mMaxSize;
    // Maximum size of one input buffer.
    std::shared_ptr<C2StreamMaxBufferSizeInfo::input> mMaxInputSize;
    // The suggested usage of input buffer allocator ID.
    std::shared_ptr<C2PortAllocatorsTuning::input> mInputAllocatorIds;
    // The suggested usage of output buffer allocator ID.
    std::shared_ptr<C2PortAllocatorsTuning::output> mOutputAllocatorIds;
    // Component uses this ID to fetch corresponding output block pool from platform.
    std::shared_ptr<C2PortBlockPoolsTuning::output> mOutputBlockPoolIds;
    std::shared_ptr<C2StreamColorInfo::output> mColorInfo;
    // The color aspects parsed from input bitstream. This parameter should be configured by
    // component while decoding.
    std::shared_ptr<C2StreamColorAspectsInfo::input> mCodedColorAspects;
    // The default color aspects specified by requested output format. This parameter should be
    // configured by client.
    std::shared_ptr<C2StreamColorAspectsTuning::output> mDefaultColorAspects;
    // The combined color aspects by |mCodedColorAspects| and |mDefaultColorAspects|, and the
    // former has higher priority. This parameter is used for component to provide color aspects
    // as C2Info in decoded output buffers.
    std::shared_ptr<C2StreamColorAspectsInfo::output> mColorAspects;
    std::shared_ptr<C2StreamPixelFormatInfo::output> mPixelFormat;
    std::shared_ptr<C2PortActualDelayTuning::input> mActualInputDelay;
    std::shared_ptr<C2PortActualDelayTuning::output> mActualOutputDelay;

    void onH263DeclareParam();

    void onVp8DeclareParam();

    void onBaseDeclareParam();

    void onMediaTypeDeclareParam(const char* mime);

    void onInputDelayDeclareParam();

    void onOutputDelayDeclareParam();

    void onBufferSizeDeclareParam();

    void onBufferPoolDeclareParam();

    void onColorAspectsDeclareParam(const std::shared_ptr<C2ReflectorHelper> &helper);
};

}// namespace android

#endif /* _C2_SOFT_VDEC_INTERFACE_IMPL_H_ */
