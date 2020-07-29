#define LOG_NDEBUG 0
#define LOG_TAG "C2VDAComponentMetaDataUtil"

#include <stdio.h>
#include <stdlib.h>
#include <utils/Log.h>
#include <cutils/properties.h>

#include <C2VDAComponentMetaDataUtil.h>
#include <VideoDecodeAcceleratorAdaptor.h>
#include <media/stagefright/foundation/ColorUtils.h>
#include <am_gralloc_ext.h>

#define min(a, b) (((a) > (b))? (b):(a))

namespace android {

void C2VDAComponent::MetaDataUtil::codecConfig(aml_dec_params* configParam) {
    uint32_t doubleWriteMode = 3;
    int default_margin = 9;
    uint32_t bufwidth = 4096;
    uint32_t bufheight = 2304;
    uint32_t margin = default_margin;
    char value[PROPERTY_VALUE_MAX];

    mConfigParam = configParam;
    memset(mConfigParam, 0, sizeof(aml_dec_params));

    std::vector<std::unique_ptr<C2Param>> params;
    C2StreamPictureSizeInfo::output output;
    c2_status_t err = mIntfImpl->query({&output}, {}, C2_MAY_BLOCK, nullptr);
    if (err != C2_OK) {
        ALOGE("query C2StreamPictureSizeInfo size error\n");
    }
    bufwidth = output.width;
    bufheight = output.height;
    ALOGI("configure width:%d height:%d", output.width, output.height);

    if (mUseSurfaceTexture) {
        doubleWriteMode = 1;
        ALOGI("surface texture use dw 1");
    }

    if (bufwidth * bufheight > 4096 * 2304) {
        doubleWriteMode = 0x04;
    }

    if (mIntfImpl->getInputCodec() == InputCodec::H264) {
        doubleWriteMode = 0x10;
    }

    if (bufwidth * bufheight > 1920 * 1088) {
        default_margin = 7;
    }

    if (property_get("vendor.media.doublewrite", value, NULL) > 0) {
        doubleWriteMode = atoi(value);
        ALOGI("set double:%d", doubleWriteMode);
    }
    memset(value, 0, sizeof(value));
    if (property_get("vendor.media.margin", value, NULL) > 0) {
        default_margin = atoi(value);
        ALOGI("set margin:%d", default_margin);
    }

    margin = default_margin;
    mConfigParam->cfg.canvas_mem_mode = 0;

#if 0
    if (mIntfImpl->getInputCodec() == InputCodec::VP9) {
        doubleWriteMode = 0x03;
        mConfigParam->cfg.canvas_mem_mode = 2;
    } else if (mIntfImpl->getInputCodec() == InputCodec::H265) {
        doubleWriteMode = 0x03;
        mConfigParam->cfg.canvas_mem_mode = 2;
    } else if (mIntfImpl->getInputCodec() == InputCodec::H264) {
        mConfigParam->cfg.canvas_mem_mode = 1;
    }
#endif

    ALOGI("doubleWriteMode %d, margin:%d \n", doubleWriteMode, margin);
    mConfigParam->cfg.uvm_hook_type = 2;
    if (/*!mSecureMode*/1) {
        if (mIntfImpl->getInputCodec() == InputCodec::H265) {
            mConfigParam->cfg.init_height = bufwidth;
            mConfigParam->cfg.init_width = bufheight;
            mConfigParam->cfg.ref_buf_margin = margin;
            mConfigParam->cfg.double_write_mode = doubleWriteMode;
            mConfigParam->cfg.canvas_mem_endian = 0;
            mConfigParam->cfg.low_latency_mode = 0;
            mConfigParam->parms_status |= V4L2_CONFIG_PARM_DECODE_CFGINFO;
    } else if (mIntfImpl->getInputCodec() == InputCodec::VP9) {
            mConfigParam->cfg.init_height = bufwidth;
            mConfigParam->cfg.init_width = bufheight;
            mConfigParam->cfg.ref_buf_margin = margin;
            mConfigParam->cfg.double_write_mode = doubleWriteMode;
            mConfigParam->cfg.canvas_mem_endian = 0;
            mConfigParam->cfg.low_latency_mode = 0;
            mConfigParam->parms_status |= V4L2_CONFIG_PARM_DECODE_CFGINFO;
            setHDRStaticInfo();
    } else if (mIntfImpl->getInputCodec() == InputCodec::H264) {
            mConfigParam->cfg.init_height = bufwidth;
            mConfigParam->cfg.init_width = bufheight;
            mConfigParam->cfg.ref_buf_margin = margin;
            mConfigParam->cfg.double_write_mode = doubleWriteMode;
            mConfigParam->cfg.canvas_mem_endian = 0;
            mConfigParam->cfg.low_latency_mode = 0;
            mConfigParam->parms_status |= V4L2_CONFIG_PARM_DECODE_CFGINFO;
        }
    }
}

int C2VDAComponent::MetaDataUtil::setHDRStaticInfo() {
        std::vector<std::unique_ptr<C2Param>> params;
    C2StreamHdrStaticInfo::output hdr;
    c2_status_t err = mIntfImpl->query({&hdr}, {}, C2_DONT_BLOCK, &params);
    if (err != C2_OK) {
         ALOGE("query hdr info error\n");
         return 0;
    }

    if (((int32_t)(hdr.mastering.red.x * 1000) == 0) &&
        ((int32_t)(hdr.mastering.red.y * 1000) == 0) &&
        ((int32_t)(hdr.mastering.green.x * 1000) == 0) &&
        ((int32_t)(hdr.mastering.green.y * 1000) == 0) &&
        ((int32_t)(hdr.mastering.blue.x * 1000) == 0) &&
        ((int32_t)(hdr.mastering.blue.y * 1000) == 0) &&
        ((int32_t)(hdr.mastering.white.x * 1000) == 0) &&
        ((int32_t)(hdr.mastering.white.y * 1000) == 0) &&
        ((int32_t)(hdr.mastering.maxLuminance * 1000) == 0) &&
        ((int32_t)(hdr.mastering.minLuminance * 1000) == 0) &&
        ((int32_t)(hdr.maxCll * 1000) == 0) &&
        ((int32_t)(hdr.maxFall * 1000) == 0)) { /* defalut val */
        ALOGE("no hdr static info set");
        return 0;
    }

    mConfigParam->hdr.color_parms.primaries[0][0] = hdr.mastering.green.x / 0.00002 + 0.5;//info.sType1.mG.x;
    mConfigParam->hdr.color_parms.primaries[0][1] = hdr.mastering.green.y / 0.00002 + 0.5;//info.sType1.mG.y;
    mConfigParam->hdr.color_parms.primaries[1][0] = hdr.mastering.blue.x / 0.00002 + 0.5;//info.sType1.mB.x;
    mConfigParam->hdr.color_parms.primaries[1][1] = hdr.mastering.blue.y / 0.00002 + 0.5;//info.sType1.mB.y;
    mConfigParam->hdr.color_parms.primaries[2][0] = hdr.mastering.red.x / 0.00002 + 0.5;//info.sType1.mR.x;
    mConfigParam->hdr.color_parms.primaries[2][1] = hdr.mastering.red.y / 0.00002 + 0.5;//info.sType1.mR.y;
    mConfigParam->hdr.color_parms.white_point[0] = hdr.mastering.white.x / 0.00002 + 0.5;//info.sType1.mW.x;
    mConfigParam->hdr.color_parms.white_point[1] = hdr.mastering.white.y / 0.00002 + 0.5;//info.sType1.mW.y;
    mConfigParam->hdr.color_parms.luminance[0] = ((int32_t)(hdr.mastering.maxLuminance + 0.5)) * 1000;//info.sType1.mMaxDisplayLuminance * 1000;
    mConfigParam->hdr.color_parms.luminance[1] = hdr.mastering.minLuminance / 0.0001 + 0.5;//info.sType1.mMinDisplayLuminance;
    mConfigParam->hdr.color_parms.content_light_level.max_content = hdr.maxCll + 0.5;//info.sType1.mMaxContentLightLevel;
    mConfigParam->hdr.color_parms.content_light_level.max_pic_average = hdr.maxFall + 0.5;//info.sType1.mMaxFrameAverageLightLevel;
    mConfigParam->parms_status |= V4L2_CONFIG_PARM_DECODE_HDRINFO;

    ALOGI("set hdrstaticinfo: gx:%d gy:%d bx:%d by:%d rx:%d,ry:%d wx:%d wy:%d maxlum:%d minlum:%d maxcontent:%d maxpicave:%d, %f %f %f %f %f %f %f %f %f %f %f %f",
            mConfigParam->hdr.color_parms.primaries[0][0],
            mConfigParam->hdr.color_parms.primaries[0][1],
            mConfigParam->hdr.color_parms.primaries[1][0],
            mConfigParam->hdr.color_parms.primaries[1][1],
            mConfigParam->hdr.color_parms.primaries[2][0],
            mConfigParam->hdr.color_parms.primaries[2][1],
            mConfigParam->hdr.color_parms.white_point[0],
            mConfigParam->hdr.color_parms.white_point[1],
            mConfigParam->hdr.color_parms.luminance[0],
            mConfigParam->hdr.color_parms.luminance[1],
            mConfigParam->hdr.color_parms.content_light_level.max_content,
            mConfigParam->hdr.color_parms.content_light_level.max_pic_average,
            hdr.mastering.green.x,
            hdr.mastering.green.y,
            hdr.mastering.blue.x,
            hdr.mastering.blue.y,
            hdr.mastering.red.x,
            hdr.mastering.red.y,
            hdr.mastering.white.x,
            hdr.mastering.white.y,
            hdr.mastering.maxLuminance,
            hdr.mastering.minLuminance,
            hdr.maxCll,
            hdr.maxFall);

    return 0;
}

void C2VDAComponent::MetaDataUtil::updateDecParmInfo(aml_dec_params* pinfo) {
    ALOGD("pinfo->dec_parms_status %x\n", pinfo->parms_status);
    if (pinfo->parms_status & V4L2_CONFIG_PARM_DECODE_HDRINFO) {
        checkHDRMetadataAndColorAspects(&pinfo->hdr);
    }
}

int C2VDAComponent::MetaDataUtil::checkHDRMetadataAndColorAspects(struct aml_vdec_hdr_infos* phdr) {
    bool isHdrChanged = false;
    bool isColorAspectsChanged = false;
    C2StreamHdrStaticInfo::output hdr;

    //setup hdr metadata, only present_flag is 1 there has a hdr metadata
    if (phdr->color_parms.present_flag == 1) {
        hdr.mastering.green.x	= 	phdr->color_parms.primaries[0][0] * 0.00002;
        hdr.mastering.green.y	= 	phdr->color_parms.primaries[0][1] * 0.00002;
        hdr.mastering.blue.x	=  	phdr->color_parms.primaries[1][0] * 0.00002;
        hdr.mastering.blue.y	= 	phdr->color_parms.primaries[1][1] * 0.00002;
        hdr.mastering.red.x     = 	phdr->color_parms.primaries[2][0] * 0.00002;
        hdr.mastering.red.y     = 	phdr->color_parms.primaries[2][1] * 0.00002;
        hdr.mastering.white.x	= 	phdr->color_parms.white_point[0] * 0.00002;
        hdr.mastering.white.y	= 	phdr->color_parms.white_point[1] * 0.00002;

        int32_t MaxDisplayLuminance = 0;
        if (mIntfImpl->getInputCodec() == InputCodec::H265) {
            MaxDisplayLuminance = min(50 * ((phdr->color_parms.luminance[0] + 250000) / 500000), 10000);
        } else if (mIntfImpl->getInputCodec() == InputCodec::VP9) {
            MaxDisplayLuminance = phdr->color_parms.luminance[0] / 1000;
        } else {
            MaxDisplayLuminance = phdr->color_parms.luminance[0];
        }
        hdr.mastering.maxLuminance = (float)MaxDisplayLuminance / 1000;
        hdr.mastering.minLuminance = phdr->color_parms.luminance[1] * 0.0001;

        hdr.maxCll =
            phdr->color_parms.content_light_level.max_content;
        hdr.maxFall =
            phdr->color_parms.content_light_level.max_pic_average;

        isHdrChanged = checkHdrStaticInfoMetaChanged(phdr);
	}

    //setup color aspects
    bool isPresent       = (phdr->signal_type >> 29) & 0x01;
    int32_t matrixCoeffs = (phdr->signal_type >> 0 ) & 0xff;
    int32_t transfer     = (phdr->signal_type >> 8 ) & 0xff;
    int32_t primaries    = (phdr->signal_type >> 16) & 0xff;
    bool    range        = (phdr->signal_type >> 25) & 0x01;

    if (isPresent) {
        ColorAspects aspects;
        ColorUtils::convertIsoColorAspectsToCodecAspects(
            primaries, transfer, matrixCoeffs, range, aspects);
        isColorAspectsChanged = 1;//((OmxVideoDecoder*)mOwner)->handleColorAspectsChange( true, aspects);
    }

    //notify OMX Client port settings changed if needed
    if (isHdrChanged || isColorAspectsChanged) {
        //mOwner->eventHandler(OMX_EventPortSettingsChanged, kOutputPortIndex,
        //OMX_IndexAndroidDescribeHDRStaticInfo, NULL)
        //config
        std::vector<std::unique_ptr<C2SettingResult>> failures;
        c2_status_t err = mIntfImpl->config({&hdr}, C2_MAY_BLOCK, &failures);
        if (err != C2_OK) {
            ALOGE("Failed to config hdr static info, error:%d", err);
        }
        std::lock_guard<std::mutex> lock(mMutex);
        mHDRStaticInfoChanged = true;
    }

    return 0;
}

int C2VDAComponent::MetaDataUtil::checkHdrStaticInfoMetaChanged(struct aml_vdec_hdr_infos* phdr) {
    if ((phdr->color_parms.primaries[0][0] == 0) &&
        (phdr->color_parms.primaries[0][1] == 0) &&
        (phdr->color_parms.primaries[1][0] == 0) &&
        (phdr->color_parms.primaries[1][1] == 0) &&
        (phdr->color_parms.primaries[2][0] == 0) &&
        (phdr->color_parms.primaries[2][1] == 0) &&
        (phdr->color_parms.white_point[0] == 0) &&
        (phdr->color_parms.white_point[1] == 0) &&
        (phdr->color_parms.luminance[0] == 0) &&
        (phdr->color_parms.luminance[1] == 0) &&
        (phdr->color_parms.content_light_level.max_content == 0) &&
        (phdr->color_parms.content_light_level.max_pic_average == 0)) {
        return false;
    }

    if (isHDRStaticInfoDifferent(&(mConfigParam->hdr), phdr)) {
        mConfigParam->hdr = *phdr;
        return true;
    }

    return false;
}

int C2VDAComponent::MetaDataUtil::isHDRStaticInfoDifferent(struct aml_vdec_hdr_infos* phdr_old, struct aml_vdec_hdr_infos* phdr_new) {
  if ((phdr_old->color_parms.primaries[0][0] != phdr_new->color_parms.primaries[0][0]) ||
      (phdr_old->color_parms.primaries[0][1] != phdr_new->color_parms.primaries[0][1]) ||
      (phdr_old->color_parms.primaries[1][0] != phdr_new->color_parms.primaries[1][0]) ||
      (phdr_old->color_parms.primaries[1][1] != phdr_new->color_parms.primaries[1][1]) ||
      (phdr_old->color_parms.primaries[2][0] |= phdr_new->color_parms.primaries[2][0]) ||
      (phdr_old->color_parms.primaries[2][1] |= phdr_new->color_parms.primaries[2][1]) ||
      (phdr_old->color_parms.white_point[0] != phdr_new->color_parms.white_point[0]) ||
      (phdr_old->color_parms.white_point[1] != phdr_new->color_parms.white_point[1]) ||
      (phdr_old->color_parms.luminance[0] != phdr_new->color_parms.luminance[0]) ||
      (phdr_old->color_parms.luminance[1] != phdr_new->color_parms.luminance[0]) ||
      (phdr_old->color_parms.content_light_level.max_content != phdr_new->color_parms.content_light_level.max_content) ||
      (phdr_old->color_parms.content_light_level.max_pic_average != phdr_new->color_parms.content_light_level.max_pic_average)) {
      return true;
  }

  return false;
}

int C2VDAComponent::MetaDataUtil::getVideoType() {
    int videotype = AM_VIDEO_4K;//AM_VIDEO_AFBC;

    if (mConfigParam->cfg.double_write_mode == 0 ||
        mConfigParam->cfg.double_write_mode == 3) {
        if (mIntfImpl->getInputCodec() == InputCodec::VP9 ||
            mIntfImpl->getInputCodec() == InputCodec::H265) {
            videotype |= AM_VIDEO_AFBC;
        }
    }

    return videotype;
}


}
