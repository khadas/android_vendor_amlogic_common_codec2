// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define LOG_NDEBUG 0
#define LOG_TAG "C2VendorComponentStore"
#include <C2Component.h>
#include <C2ComponentFactory.h>
#include <C2Config.h>
#include <C2VendorSupport.h>
#include <util/C2InterfaceHelper.h>

#include <cutils/properties.h>
#include <utils/Log.h>

#include <dlfcn.h>

#include <map>
#include <memory>
#include <mutex>

#define UNUSED(expr)  \
    do {              \
        (void)(expr); \
    } while (0)

#define ION_POOL_CMA_MASK                       (1 << 16)
#define ION_FLAG_EXTEND_MESON_SECURE_VDEC_HEAP  (1 << 28)
#define ION_FLAG_EXTEND_MESON_HEAP              (1 << 30)
#define ION_FLAG_EXTEND_PROTECTED               (1 << 31)

namespace android {

typedef ::C2ComponentFactory* (*CreateCodec2FactoryFunc2)(bool);
typedef void (*DestroyCodec2FactoryFunc2)(::C2ComponentFactory*);

const C2String kComponentLoadVideoDecoderLibray = "libcodec2_aml_video_decoder.so";
const C2String kComponentLoadVideoEncoderLibray = "libcodec2_aml_video_encoder.so";
const C2String kComponentLoadAudioDecoderLibray = "libcodec2_aml_audio_decoder.so";


class C2VendorComponentStore : public C2ComponentStore {
public:
    C2VendorComponentStore();
    ~C2VendorComponentStore() override = default;

    // The implementation of C2ComponentStore.
    C2String getName() const override;
    c2_status_t createComponent(C2String name,
                                std::shared_ptr<C2Component>* const component) override;
    c2_status_t createInterface(C2String name,
                                std::shared_ptr<C2ComponentInterface>* const interface) override;
    std::vector<std::shared_ptr<const C2Component::Traits>> listComponents() override;
    c2_status_t copyBuffer(std::shared_ptr<C2GraphicBuffer> src,
                           std::shared_ptr<C2GraphicBuffer> dst) override;
    c2_status_t query_sm(const std::vector<C2Param*>& stackParams,
                         const std::vector<C2Param::Index>& heapParamIndices,
                         std::vector<std::unique_ptr<C2Param>>* const heapParams) const override;
    c2_status_t config_sm(const std::vector<C2Param*>& params,
                          std::vector<std::unique_ptr<C2SettingResult>>* const failures) override;
    std::shared_ptr<C2ParamReflector> getParamReflector() const override;
    c2_status_t querySupportedParams_nb(
            std::vector<std::shared_ptr<C2ParamDescriptor>>* const params) const override;
    c2_status_t querySupportedValues_sm(
            std::vector<C2FieldSupportedValuesQuery>& fields) const override;

private:

    /**
     * An object encapsulating a loaded component module.
     *
     * \todo provide a way to add traits to known components here to avoid loading the .so-s
     * for listComponents
     */
    class ComponentModule : public C2ComponentFactory {
    public:
        ComponentModule()
              : mInit(C2_NO_INIT),
                mLibHandle(nullptr),
                mIsAudio(false),
                createFactory(nullptr),
                destroyFactory(nullptr),
                mComponentFactory(nullptr) {}

        ~ComponentModule() override;
        c2_status_t init(std::string libPath, C2VendorCodec codec, bool secure, bool isAudio);

        // Return the traits of the component in this module.
        std::shared_ptr<const C2Component::Traits> getTraits();

        // The implementation of C2ComponentFactory.
        c2_status_t createComponent(
                c2_node_id_t id, std::shared_ptr<C2Component>* component,
                ComponentDeleter deleter = std::default_delete<C2Component>()) override;
        c2_status_t createInterface(
                c2_node_id_t id, std::shared_ptr<C2ComponentInterface>* interface,
                InterfaceDeleter deleter = std::default_delete<C2ComponentInterface>()) override;

    protected:
        std::recursive_mutex mLock;                    ///< lock protecting mTraits
        std::shared_ptr<C2Component::Traits> mTraits;  ///< cached component traits

        c2_status_t mInit;  ///< initialization result

        void* mLibHandle;                                             ///< loaded library handle
        bool mIsAudio;
        CreateCodec2FactoryFunc2 createFactory;    ///< loaded create function
        DestroyCodec2FactoryFunc2 destroyFactory;  ///< loaded destroy function
        C2ComponentFactory* mComponentFactory;  ///< loaded/created component factory
    };

    /**
     * An object encapsulating a loadable component module.
     *
     * \todo make this also work for enumerations
     */
    class ComponentLoader {
    public:
        ComponentLoader(std::string libPath, C2VendorCodec codec, bool isAudio = false) : mLibPath(libPath), mCodec(codec),  mIsAudio(isAudio){}

        /**
         * Load the component module.
         *
         * This method simply returns the component module if it is already currently loaded, or
         * attempts to load it if it is not.
         *
         * \param module[out] pointer to the shared pointer where the loaded module shall be stored.
         *                    This will be nullptr on error.
         *
         * \retval C2_OK        the component module has been successfully loaded
         * \retval C2_NO_MEMORY not enough memory to loading the component module
         * \retval C2_NOT_FOUND could not locate the component module
         * \retval C2_CORRUPTED the component module could not be loaded
         * \retval C2_REFUSED   permission denied to load the component module
         */
        c2_status_t fetchModule(std::shared_ptr<ComponentModule>* module, bool secure);

    private:
        std::mutex mMutex;                       ///< mutex guarding the module
        std::weak_ptr<ComponentModule> mModule;  ///< weak reference to the loaded module
        std::string mLibPath;                    ///< library path (or name)
        C2VendorCodec mCodec = C2VendorCodec::UNKNOWN;
        bool mIsAudio;
    };

    struct Interface : public C2InterfaceHelper {
        std::shared_ptr<C2StoreIonUsageInfo> mIonUsageInfo;
        std::shared_ptr<C2StoreDmaBufUsageInfo> mDmaBufUsageInfo;

        Interface(std::shared_ptr<C2ReflectorHelper> reflector)
            : C2InterfaceHelper(reflector) {
            setDerivedInstance(this);
            struct Setter {
                static C2R setIonUsage(bool /* mayBlock */, C2P<C2StoreIonUsageInfo> &me) {
                    ALOGI("setIonUsage %lld %d", me.get().usage, me.get().capacity);
                    if (me.get().usage & C2MemoryUsage::READ_PROTECTED) {
                        me.set().heapMask = ION_POOL_CMA_MASK;
                        me.set().allocFlags = ION_FLAG_EXTEND_MESON_HEAP | ION_FLAG_EXTEND_MESON_SECURE_VDEC_HEAP;
                    } else {
                        me.set().heapMask = ~(1<<5);
                        me.set().allocFlags = 0;
                    }

                    me.set().minAlignment = 0;
                    return C2R::Ok();
                }

                static C2R setDmaBufUsage(bool /* mayBlock */, C2P<C2StoreDmaBufUsageInfo> &me) {
                    long long usage = (long long)me.get().m.usage;
                    ALOGI("setDmaBufUsage %lld", usage);
                    if (usage & C2MemoryUsage::READ_PROTECTED)
                        strncpy(me.set().m.heapName, "system-secure-uncached", me.v.flexCount());
                    else
                        strncpy(me.set().m.heapName, "system-uncached", me.v.flexCount());
                    me.set().m.allocFlags = 0;
                    return C2R::Ok();
                };
            };

            addParameter(
                DefineParam(mIonUsageInfo, "ion-usage")
                .withDefault(new C2StoreIonUsageInfo())
                .withFields({
                    C2F(mIonUsageInfo, usage).flags({C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE}),
                    C2F(mIonUsageInfo, capacity).inRange(0, UINT32_MAX, 1024),
                    C2F(mIonUsageInfo, heapMask).any(),
                    C2F(mIonUsageInfo, allocFlags).flags({}),
                    C2F(mIonUsageInfo, minAlignment).equalTo(0)
                })
                .withSetter(Setter::setIonUsage)
                .build());

            addParameter(
                DefineParam(mDmaBufUsageInfo, "dmabuf-usage")
                .withDefault(C2StoreDmaBufUsageInfo::AllocShared(0))
                .withFields({
                    C2F(mDmaBufUsageInfo, m.usage).flags({C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE}),
                    C2F(mDmaBufUsageInfo, m.capacity).inRange(0, UINT32_MAX, 1024),
                    C2F(mDmaBufUsageInfo, m.allocFlags).flags({}),
                    C2F(mDmaBufUsageInfo, m.heapName).any(),
                })
                .withSetter(Setter::setDmaBufUsage)
                .build());
        }
    };

    c2_status_t findComponent(C2String name, ComponentLoader** loader);

    std::map<C2String, ComponentLoader> mComponents;  ///< list of components
    std::shared_ptr<C2ReflectorHelper> mReflector;
    Interface mInterface;
};

C2VendorComponentStore::ComponentModule::~ComponentModule() {
    ALOGV("In %s", __func__);
    if (destroyFactory && mComponentFactory) {
        destroyFactory(mComponentFactory);
    }
    if (mLibHandle) {
        ALOGV("Unloading dll");
        dlclose(mLibHandle);
    }
}

c2_status_t C2VendorComponentStore::ComponentModule::init(std::string libPath, C2VendorCodec codec, bool secure,  bool isAudio) {
    ALOGV("Loading dll");
    mIsAudio = isAudio;
    mLibHandle = dlopen(libPath.c_str(), RTLD_NOW | RTLD_NODELETE);
    if (mLibHandle == nullptr) {
        ALOGD("Could not dlopen %s: %s", libPath.c_str(), dlerror());
        mInit = C2_CORRUPTED;
    } else {
        std::string createFactoryName;
        std::string destroyFactoryName;
        switch (codec) {
          case C2VendorCodec::VDEC_H264:
              createFactoryName = "CreateC2VdecH264Factory";
              destroyFactoryName = "DestroyC2VdecH264Factory";
              break;
          case C2VendorCodec::VDEC_H265:
              createFactoryName = "CreateC2VdecH265Factory";
              destroyFactoryName = "DestroyC2VdecH265Factory";
              break;
          case C2VendorCodec::VDEC_VP9:
              createFactoryName = "CreateC2VdecVP9Factory";
              destroyFactoryName = "DestroyC2VdecVP9Factory";
              break;
          case C2VendorCodec::VDEC_AV1:
              createFactoryName = "CreateC2VdecAV1Factory";
              destroyFactoryName = "DestroyC2VdecAV1Factory";
              break;
          case C2VendorCodec::VDEC_DVHE:
              createFactoryName = "CreateC2VdecDVHEFactory";
              destroyFactoryName = "DestroyC2VdecDVHEFactory";
              break;
          case C2VendorCodec::VDEC_DVAV:
              createFactoryName = "CreateC2VdecDVAVFactory";
              destroyFactoryName = "DestroyC2VdecDVAVFactory";
              break;
          case C2VendorCodec::VDEC_DVAV1:
              createFactoryName = "CreateC2VdecDVAV1Factory";
              destroyFactoryName = "DestroyC2VdecDVAV1Factory";
              break;
          case C2VendorCodec::VDEC_MP2V:
              createFactoryName = "CreateC2VdecMP2VFactory";
              destroyFactoryName = "DestroyC2VdecMP2VFactory";
              break;
          case C2VendorCodec::VDEC_MP4V:
              createFactoryName = "CreateC2VdecMP4VFactory";
              destroyFactoryName = "DestroyC2VdecMP4VFactory";
              break;
          case C2VendorCodec::VDEC_MJPG:
              createFactoryName = "CreateC2VdecMJPGFactory";
              destroyFactoryName = "DestroyC2VdecMJPGFactory";
              break;
          case C2VendorCodec::VENC_H264:
              createFactoryName = "CreateC2VencHCodecFactory";
              destroyFactoryName = "DestroyC2VencHCodecFactory";
              break;
          case C2VendorCodec::VENC_H265:
              createFactoryName = "CreateC2VencW420Factory";
              destroyFactoryName = "DestroyC2VencW420Factory";
              break;
          default:
              ALOGE("Unknown codec:%d", codec);
              return C2_CORRUPTED;
        }
        createFactory = (CreateCodec2FactoryFunc2)dlsym(
                mLibHandle, createFactoryName.c_str());
        destroyFactory = (DestroyCodec2FactoryFunc2)dlsym(
                mLibHandle, destroyFactoryName.c_str());
        mComponentFactory = createFactory(secure);
        if (mComponentFactory == nullptr) {
            ALOGD("could not create factory in %s", libPath.c_str());
            mInit = C2_NO_MEMORY;
        } else {
            mInit = C2_OK;
        }
    }
    return mInit;
}

std::shared_ptr<const C2Component::Traits> C2VendorComponentStore::ComponentModule::getTraits() {
    std::unique_lock<std::recursive_mutex> lock(mLock);
    if (!mTraits) {
        std::shared_ptr<C2ComponentInterface> intf;
        auto res = createInterface(0, &intf);
        if (res != C2_OK) {
            ALOGE("failed to create interface: %d", res);
            return nullptr;
        }

        std::shared_ptr<C2Component::Traits> traits(new (std::nothrow) C2Component::Traits);
        if (traits) {
            traits->name = intf->getName();
            // TODO: get this from interface properly.
            bool encoder = (traits->name.find("encoder") != std::string::npos);
            uint32_t mediaTypeIndex = encoder ? C2PortMediaTypeSetting::output::PARAM_TYPE
                                              : C2PortMediaTypeSetting::input::PARAM_TYPE;
            std::vector<std::unique_ptr<C2Param>> params;
            res = intf->query_vb({}, {mediaTypeIndex}, C2_MAY_BLOCK, &params);
            if (res != C2_OK) {
                ALOGE("failed to query interface: %d", res);
                return nullptr;
            }
            if (params.size() != 1u) {
                ALOGE("failed to query interface: unexpected vector size: %zu", params.size());
                return nullptr;
            }
            C2PortMediaTypeSetting* mediaTypeConfig = (C2PortMediaTypeSetting*)(params[0].get());
            if (mediaTypeConfig == nullptr) {
                ALOGE("failed to query media type");
                return nullptr;
            }
            traits->domain = mIsAudio ? C2Component::DOMAIN_AUDIO : C2Component::DOMAIN_VIDEO;
            traits->kind = encoder ? C2Component::KIND_ENCODER : C2Component::KIND_DECODER;
            traits->mediaType = mediaTypeConfig->m.value;
            // TODO: get this properly.
            // Set the rank prior to c2.google.* (=0x200) and after OMX.google.* (=0x100) by now.
            // In the future this should be prior to OMX.google.* as well so that ARC HW codec
            // would be the first priority.
            traits->rank = 0x10;
        }
        mTraits = traits;
    }
    return mTraits;
}

c2_status_t C2VendorComponentStore::ComponentModule::createComponent(
        c2_node_id_t id, std::shared_ptr<C2Component>* component,
        std::function<void(::C2Component*)> deleter) {
    UNUSED(deleter);
    component->reset();
    if (mInit != C2_OK) {
        return mInit;
    }
    return mComponentFactory->createComponent(id, component,
                                              C2ComponentFactory::ComponentDeleter());
}

c2_status_t C2VendorComponentStore::ComponentModule::createInterface(
        c2_node_id_t id, std::shared_ptr<C2ComponentInterface>* interface,
        std::function<void(::C2ComponentInterface*)> deleter) {
    UNUSED(deleter);
    interface->reset();
    if (mInit != C2_OK) {
        return mInit;
    }
    return mComponentFactory->createInterface(id, interface,
                                              C2ComponentFactory::InterfaceDeleter());
}

c2_status_t C2VendorComponentStore::ComponentLoader::fetchModule(
        std::shared_ptr<ComponentModule>* module, bool secure) {
    c2_status_t res = C2_OK;
    std::lock_guard<std::mutex> lock(mMutex);
    std::shared_ptr<ComponentModule> localModule = mModule.lock();
    if (localModule == nullptr) {
        localModule = std::make_shared<ComponentModule>();

        ALOGI("localModule libpath %s %d\n", mLibPath.c_str(), mCodec);
        res = localModule->init(mLibPath, mCodec, secure, mIsAudio);
        if (res == C2_OK) {
            mModule = localModule;
        }
    }
    *module = localModule;
    return res;
}

C2VendorComponentStore::C2VendorComponentStore()
      :
      mReflector(std::make_shared<C2ReflectorHelper>()),
      mInterface(mReflector) {
    // TODO: move this also into a .so so it can be updated
    bool supportC2Vdec = property_get_bool("vendor.media.c2.vdec.support", true);
    bool disableC2SecureVdec = property_get_bool("vendor.media.c2.vdec.secure.disable", false);
    bool supportC2VEnc = property_get_bool("vendor.media.c2.vdec.support", true);
    bool supportC2Adec = property_get_bool("vendor.media.c2.adec.support", false);
    if (supportC2Vdec) {
        for (int i = 0; i < sizeof(gC2VideoDecoderComponents) / sizeof(C2VendorComponent); i++) {
            if (disableC2SecureVdec && strstr(gC2VideoDecoderComponents[i].compname.c_str(), (const char *)".secure"))
                continue;
            mComponents.emplace(std::piecewise_construct, std::forward_as_tuple(gC2VideoDecoderComponents[i].compname),
                    std::forward_as_tuple(kComponentLoadVideoDecoderLibray, gC2VideoDecoderComponents[i].codec));
        }
    }
    if (supportC2VEnc) {
        for (int i = 0; i < sizeof(gC2VideoEncoderComponents) / sizeof(C2VendorComponent); i++) {
            mComponents.emplace(std::piecewise_construct, std::forward_as_tuple(gC2VideoEncoderComponents[i].compname),
                    std::forward_as_tuple(kComponentLoadVideoEncoderLibray, gC2VideoEncoderComponents[i].codec));
        }
    }
    if (supportC2Adec) {
        for (int i = 0; i < sizeof(gC2AudioDecoderComponents) / sizeof(C2VendorComponent); i++) {
            mComponents.emplace(std::piecewise_construct, std::forward_as_tuple(gC2AudioDecoderComponents[i].compname),
                    std::forward_as_tuple(kComponentLoadAudioDecoderLibray, gC2AudioDecoderComponents[i].codec, true));
        }
    }
    ALOGI("C2VendorComponentStore::C2VendorComponentStore\n");
}

C2String C2VendorComponentStore::getName() const {
    return "android.componentStore.vendor";
}

std::vector<std::shared_ptr<const C2Component::Traits>> C2VendorComponentStore::listComponents() {
    // This method SHALL return within 500ms.
    std::vector<std::shared_ptr<const C2Component::Traits>> list;
    ALOGV("C2VendorComponentStore::listComponents\n");
    if (!property_get_bool("debug.vdecstore.enable-c2", true)) {
        // Temporarily disable all vdec components.
        return list;
    }
    for (auto& it : mComponents) {
        ComponentLoader& loader = it.second;
        std::shared_ptr<ComponentModule> module;
        bool secure = it.first.find(".secure") != std::string::npos;
        c2_status_t res = loader.fetchModule(&module, secure);
        if (res == C2_OK) {
            std::shared_ptr<const C2Component::Traits> traits = module->getTraits();
            if (traits) {
                ALOGI("C2VendorComponentStore::listComponents traits push name %s\n", traits->name.c_str());
                list.push_back(traits);
            }
        }
    }
    return list;
}

c2_status_t C2VendorComponentStore::findComponent(C2String name, ComponentLoader** loader) {
    *loader = nullptr;
    ALOGI("findComponent\n");
    auto pos = mComponents.find(name);
    // TODO: check aliases
    if (pos == mComponents.end()) {
        return C2_NOT_FOUND;
    }
    *loader = &pos->second;
    return C2_OK;
}

c2_status_t C2VendorComponentStore::createComponent(C2String name,
                                                 std::shared_ptr<C2Component>* const component) {
    // This method SHALL return within 100ms.
    ALOGI("C2VendorComponentStore::createComponent name %s\n", name.c_str());
    component->reset();
    ComponentLoader* loader;
    c2_status_t res = findComponent(name, &loader);
    bool secure = name.find(".secure") != std::string::npos;
    if (res == C2_OK) {
        std::shared_ptr<ComponentModule> module;
        ALOGI("C2VendorComponentStore::createComponent fetchModule\n");
        res = loader->fetchModule(&module, secure);
        if (res == C2_OK) {
            // TODO: get a unique node ID
            res = module->createComponent(0, component);
        }
    }
    return res;
}

c2_status_t C2VendorComponentStore::createInterface(
        C2String name, std::shared_ptr<C2ComponentInterface>* const interface) {
    // This method SHALL return within 100ms.
    interface->reset();
    ComponentLoader* loader;
    c2_status_t res = findComponent(name, &loader);
    bool secure = name.find(".secure") != std::string::npos;
    if (res == C2_OK) {
        std::shared_ptr<ComponentModule> module;
        res = loader->fetchModule(&module, secure);
        if (res == C2_OK) {
            // TODO: get a unique node ID
            res = module->createInterface(0, interface);
        }
    }
    return res;
}

c2_status_t C2VendorComponentStore::copyBuffer(std::shared_ptr<C2GraphicBuffer> src,
                                            std::shared_ptr<C2GraphicBuffer> dst) {
    UNUSED(src);
    UNUSED(dst);
    return C2_OMITTED;
}

c2_status_t C2VendorComponentStore::query_sm(
        const std::vector<C2Param*>& stackParams,
        const std::vector<C2Param::Index>& heapParamIndices,
        std::vector<std::unique_ptr<C2Param>>* const heapParams) const {
    // there are no supported configs
    return mInterface.query(stackParams, heapParamIndices, C2_MAY_BLOCK, heapParams);
}

c2_status_t C2VendorComponentStore::config_sm(
        const std::vector<C2Param*>& params,
        std::vector<std::unique_ptr<C2SettingResult>>* const failures) {
    // there are no supported configs
    return mInterface.config(params, C2_MAY_BLOCK, failures);
}

std::shared_ptr<C2ParamReflector> C2VendorComponentStore::getParamReflector() const {
    return mReflector;
}

c2_status_t C2VendorComponentStore::querySupportedParams_nb(
        std::vector<std::shared_ptr<C2ParamDescriptor>>* const params) const {
    // there are no supported config params
    return mInterface.querySupportedParams(params);
}

c2_status_t C2VendorComponentStore::querySupportedValues_sm(
        std::vector<C2FieldSupportedValuesQuery>& fields) const {
    // there are no supported config params
    return mInterface.querySupportedValues(fields, C2_MAY_BLOCK);
}

std::shared_ptr<C2ComponentStore> GetCodec2VendorComponentStore() {
    static std::mutex mutex;
    static std::weak_ptr<C2ComponentStore> platformStore;
    std::lock_guard<std::mutex> lock(mutex);
    ALOGI("GetCodec2VendorComponentStore\n");
    std::shared_ptr<C2ComponentStore> store = platformStore.lock();
    if (store == nullptr) {
        store = std::make_shared<C2VendorComponentStore>();
        platformStore = store;
    }
    return store;
}

}  // namespace android

