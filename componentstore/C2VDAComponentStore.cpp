// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define LOG_NDEBUG 0
#define LOG_TAG "C2VDAComponentStore"
#include <C2Component.h>
#include <C2ComponentFactory.h>
#include <C2Config.h>
#include <C2VDASupport.h>
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

namespace android {
class C2VDAComponentStore : public C2ComponentStore {
public:
    C2VDAComponentStore();
    ~C2VDAComponentStore() override = default;

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
    enum class C2VDACodec {
        UNKNOWN,
        H264,
        H265,
        VP9,
    };

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
                createFactory(nullptr),
                destroyFactory(nullptr),
                mComponentFactory(nullptr) {}

        ~ComponentModule() override;
        c2_status_t init(std::string libPath, C2VDACodec codec);

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
        C2ComponentFactory::CreateCodec2FactoryFunc createFactory;    ///< loaded create function
        C2ComponentFactory::DestroyCodec2FactoryFunc destroyFactory;  ///< loaded destroy function
        C2ComponentFactory* mComponentFactory;  ///< loaded/created component factory
    };

    /**
     * An object encapsulating a loadable component module.
     *
     * \todo make this also work for enumerations
     */
    class ComponentLoader {
    public:
        ComponentLoader(std::string libPath, C2VDACodec codec) : mLibPath(libPath), mCodec(codec) {}

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
        c2_status_t fetchModule(std::shared_ptr<ComponentModule>* module);

    private:
        std::mutex mMutex;                       ///< mutex guarding the module
        std::weak_ptr<ComponentModule> mModule;  ///< weak reference to the loaded module
        std::string mLibPath;                    ///< library path (or name)
        C2VDACodec mCodec = C2VDACodec::UNKNOWN;
    };

    struct Interface : public C2InterfaceHelper {
        std::shared_ptr<C2StoreIonUsageInfo> mIonUsageInfo;

        Interface(std::shared_ptr<C2ReflectorHelper> reflector)
            : C2InterfaceHelper(reflector) {
            setDerivedInstance(this);
            struct Setter {
                static C2R setIonUsage(bool /* mayBlock */, C2P<C2StoreIonUsageInfo> &me) {
                    me.set().heapMask = ~(1<<5);
                    me.set().allocFlags = 0;
                    me.set().minAlignment = 0;
                    return C2R::Ok();
                }
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
        }
    };

    c2_status_t findComponent(C2String name, ComponentLoader** loader);

    std::map<C2String, ComponentLoader> mComponents;  ///< list of components
    std::shared_ptr<C2ReflectorHelper> mReflector;
    Interface mInterface;
};

C2VDAComponentStore::ComponentModule::~ComponentModule() {
    ALOGV("in %s", __func__);
    if (destroyFactory && mComponentFactory) {
        destroyFactory(mComponentFactory);
    }
    if (mLibHandle) {
        ALOGV("unloading dll");
        dlclose(mLibHandle);
    }
}

c2_status_t C2VDAComponentStore::ComponentModule::init(std::string libPath, C2VDACodec codec) {
    ALOGV("loading dll");
    mLibHandle = dlopen(libPath.c_str(), RTLD_NOW | RTLD_NODELETE);
    if (mLibHandle == nullptr) {
        ALOGD("could not dlopen %s: %s", libPath.c_str(), dlerror());
        mInit = C2_CORRUPTED;
    } else {
        std::string createFactoryName;
        std::string destroyFactoryName;
        switch (codec) {
        case C2VDACodec::H264:
            createFactoryName = "CreateC2VDAH264Factory";
            destroyFactoryName = "DestroyC2VDAH264Factory";
            break;
		case C2VDACodec::H265:
			createFactoryName = "CreateC2VDAH265Factory";
            destroyFactoryName = "DestroyC2VDAH265Factory";
			break;
        case C2VDACodec::VP9:
            createFactoryName = "CreateC2VDAVP9Factory";
            destroyFactoryName = "DestroyC2VDAVP9Factory";
            break;
        default:
            ALOGE("Unknown ");
            return C2_CORRUPTED;
        }
        createFactory = (C2ComponentFactory::CreateCodec2FactoryFunc)dlsym(
                mLibHandle, createFactoryName.c_str());
        destroyFactory = (C2ComponentFactory::DestroyCodec2FactoryFunc)dlsym(
                mLibHandle, destroyFactoryName.c_str());
        mComponentFactory = createFactory();
        if (mComponentFactory == nullptr) {
            ALOGD("could not create factory in %s", libPath.c_str());
            mInit = C2_NO_MEMORY;
        } else {
            mInit = C2_OK;
        }
    }
    return mInit;
}

std::shared_ptr<const C2Component::Traits> C2VDAComponentStore::ComponentModule::getTraits() {
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
            traits->domain = C2Component::DOMAIN_VIDEO;
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

c2_status_t C2VDAComponentStore::ComponentModule::createComponent(
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

c2_status_t C2VDAComponentStore::ComponentModule::createInterface(
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

c2_status_t C2VDAComponentStore::ComponentLoader::fetchModule(
        std::shared_ptr<ComponentModule>* module) {
    c2_status_t res = C2_OK;
    std::lock_guard<std::mutex> lock(mMutex);
    std::shared_ptr<ComponentModule> localModule = mModule.lock();
    if (localModule == nullptr) {
        localModule = std::make_shared<ComponentModule>();
        ALOGI("localModule libpath %s %d\n", mLibPath.c_str(), mCodec);
        res = localModule->init(mLibPath, mCodec);
        if (res == C2_OK) {
            mModule = localModule;
        }
    }
    *module = localModule;
    return res;
}

C2VDAComponentStore::C2VDAComponentStore()
      :
      mReflector(std::make_shared<C2ReflectorHelper>()),
      mInterface(mReflector) {
    // TODO: move this also into a .so so it can be updated
    bool supportc2 = property_get_bool("vendor.media.codec2.support", false);
    if (supportc2) {
        mComponents.emplace(std::piecewise_construct, std::forward_as_tuple("c2.vda.avc.decoder"),
                            std::forward_as_tuple("libcodec2_aml.so", C2VDACodec::H264));
        mComponents.emplace(std::piecewise_construct, std::forward_as_tuple("c2.vda.hevc.decoder"),
                            std::forward_as_tuple("libcodec2_aml.so", C2VDACodec::H265));
        mComponents.emplace(std::piecewise_construct, std::forward_as_tuple("c2.vda.vp9.decoder"),
                            std::forward_as_tuple("libcodec2_aml.so", C2VDACodec::VP9));

    }
    ALOGI("C2VDAComponentStore::C2VDAComponentStore\n");
}

C2String C2VDAComponentStore::getName() const {
    return "android.componentStore.vda";
}

std::vector<std::shared_ptr<const C2Component::Traits>> C2VDAComponentStore::listComponents() {
    // This method SHALL return within 500ms.
    std::vector<std::shared_ptr<const C2Component::Traits>> list;
    ALOGV("C2VDAComponentStore::listComponents\n");
    if (!property_get_bool("debug.vdastore.enable-c2", true)) {
        // Temporarily disable all VDA components.
        return list;
    }
    for (auto& it : mComponents) {
        ComponentLoader& loader = it.second;
        std::shared_ptr<ComponentModule> module;
        c2_status_t res = loader.fetchModule(&module);
        if (res == C2_OK) {
            std::shared_ptr<const C2Component::Traits> traits = module->getTraits();
            if (traits) {
                ALOGI("C2VDAComponentStore::listComponents traits push name %s\n", traits->name.c_str());
                list.push_back(traits);
            }
        }
    }
    return list;
}

c2_status_t C2VDAComponentStore::findComponent(C2String name, ComponentLoader** loader) {
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

c2_status_t C2VDAComponentStore::createComponent(C2String name,
                                                 std::shared_ptr<C2Component>* const component) {
    // This method SHALL return within 100ms.
    ALOGI("C2VDAComponentStore::createComponent name %s\n", name.c_str());
    component->reset();
    ComponentLoader* loader;
    c2_status_t res = findComponent(name, &loader);
    if (res == C2_OK) {
        std::shared_ptr<ComponentModule> module;
    ALOGI("C2VDAComponentStore::createComponent fetchModule\n");
        res = loader->fetchModule(&module);
        if (res == C2_OK) {
            // TODO: get a unique node ID
            res = module->createComponent(0, component);
        }
    }
    return res;
}

c2_status_t C2VDAComponentStore::createInterface(
        C2String name, std::shared_ptr<C2ComponentInterface>* const interface) {
    // This method SHALL return within 100ms.
    interface->reset();
    ComponentLoader* loader;
    c2_status_t res = findComponent(name, &loader);
    if (res == C2_OK) {
        std::shared_ptr<ComponentModule> module;
        res = loader->fetchModule(&module);
        if (res == C2_OK) {
            // TODO: get a unique node ID
            res = module->createInterface(0, interface);
        }
    }
    return res;
}

c2_status_t C2VDAComponentStore::copyBuffer(std::shared_ptr<C2GraphicBuffer> src,
                                            std::shared_ptr<C2GraphicBuffer> dst) {
    UNUSED(src);
    UNUSED(dst);
    return C2_OMITTED;
}

c2_status_t C2VDAComponentStore::query_sm(
        const std::vector<C2Param*>& stackParams,
        const std::vector<C2Param::Index>& heapParamIndices,
        std::vector<std::unique_ptr<C2Param>>* const heapParams) const {
    // there are no supported configs
    return mInterface.query(stackParams, heapParamIndices, C2_MAY_BLOCK, heapParams);
}

c2_status_t C2VDAComponentStore::config_sm(
        const std::vector<C2Param*>& params,
        std::vector<std::unique_ptr<C2SettingResult>>* const failures) {
    // there are no supported configs
    return mInterface.config(params, C2_MAY_BLOCK, failures);
}

std::shared_ptr<C2ParamReflector> C2VDAComponentStore::getParamReflector() const {
    return mReflector;
}

c2_status_t C2VDAComponentStore::querySupportedParams_nb(
        std::vector<std::shared_ptr<C2ParamDescriptor>>* const params) const {
    // there are no supported config params
    return mInterface.querySupportedParams(params);
}

c2_status_t C2VDAComponentStore::querySupportedValues_sm(
        std::vector<C2FieldSupportedValuesQuery>& fields) const {
    // there are no supported config params
    return mInterface.querySupportedValues(fields, C2_MAY_BLOCK);
}

std::shared_ptr<C2ComponentStore> GetCodec2VDAComponentStore() {
    static std::mutex mutex;
    static std::weak_ptr<C2ComponentStore> platformStore;
    std::lock_guard<std::mutex> lock(mutex);
    ALOGI("GetCodec2VDAComponentStore\n");
    std::shared_ptr<C2ComponentStore> store = platformStore.lock();
    if (store == nullptr) {
        store = std::make_shared<C2VDAComponentStore>();
        platformStore = store;
    }
    return store;
}

}  // namespace android

