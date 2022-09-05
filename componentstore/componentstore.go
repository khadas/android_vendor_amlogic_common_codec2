package componentstore

import (
        "android/soong/android"
        "android/soong/cc"
)

func init() {
    android.RegisterModuleType("componentstore_defaults", componentstoreDefaultsFactory)
}

func componentstoreDefaultsFactory() (android.Module) {
    module := cc.DefaultsFactory()
    android.AddLoadHook(module, componentstoreDefaults)
    return module
}

func componentstoreDefaults(ctx android.LoadHookContext) {
    var cppflags []string
    type props struct {
        Cflags []string
    }
    p := &props{}

    vconfig := ctx.Config().VendorConfig("amlogic_vendorconfig")
    if vconfig.Bool("enable_swcodec") == true {
        cppflags = append(cppflags, "-DSUPPORT_SOFT_VDEC=1")
    }
    p.Cflags = cppflags
    ctx.AppendProperties(p)
}

