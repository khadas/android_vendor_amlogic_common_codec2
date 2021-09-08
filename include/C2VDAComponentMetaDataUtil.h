#ifndef _C2_VDA_COMPONENT_METADATA_UTIL_H_
#define _C2_VDA_COMPONENT_METADATA_UTIL_H_

#include <mutex>

#include <C2Config.h>
#include <C2Enum.h>
#include <C2Param.h>
#include <C2ParamDef.h>
#include <SimpleC2Interface.h>
#include <util/C2InterfaceHelper.h>
#include <C2VDAComponent.h>

#include <VideoDecWraper.h>

namespace android {
class C2VDAComponent::MetaDataUtil {
public:
    MetaDataUtil(C2VDAComponent* comp, bool secure):
        mComp(comp),
        mUseSurfaceTexture(false),
        mNoSurface(false),
        mHDRStaticInfoChanged(false),
        mSecure(secure) {
        mIntfImpl = mComp->GetIntfImpl();
    }
    virtual ~MetaDataUtil() {}

    /* configure decoder */
    void codecConfig(aml_dec_params* params);
    void updateDecParmInfo(aml_dec_params* params);
    int getVideoType();
    void setUseSurfaceTexture(bool usersftexture) { mUseSurfaceTexture = usersftexture; }
    void setNoSurfaceTexture(bool isNoSurface);
    bool isHDRStaticInfoUpdated() {
        if (mHDRStaticInfoChanged) {
            std::lock_guard<std::mutex> lock(mMutex);
            mHDRStaticInfoChanged = false;
            return true;
        }

        return false;
    }
    //int check_color_aspects();
    uint64_t getPlatformUsage();

private:
    /* set hdr static to decoder */
    int setHDRStaticInfo();
    int checkHDRMetadataAndColorAspects(struct aml_vdec_hdr_infos* phdr);
    int checkHdrStaticInfoMetaChanged(struct aml_vdec_hdr_infos* phdr);
    int isHDRStaticInfoDifferent(struct aml_vdec_hdr_infos* phd_old, struct aml_vdec_hdr_infos* phdr_new);

    C2VDAComponent::IntfImpl* mIntfImpl;
    aml_dec_params* mConfigParam;
    C2VDAComponent* mComp;
    bool mUseSurfaceTexture;
    bool mNoSurface;
    bool mHDRStaticInfoChanged;
    bool mSecure;
    std::mutex mMutex;
};

}

#endif
