package c2_service_aml

import (
    "android/soong/android"
    "android/soong/cc"
)

func init() {
    android.RegisterModuleType("c2_service_aml_go_defaults",codec_DefaultsFactory)
}

func codec_DefaultsFactory() (android.Module) {
    module := cc.DefaultsFactory()
    android.AddLoadHook(module, c2_service_aml_Defaults)
    return module
}

func c2_service_aml_Defaults(ctx android.LoadHookContext) {
     type propsE struct {
        Shared_libs  []string
    }
    p := &propsE{}
    PlatformVndkVersion := ctx.DeviceConfig().PlatformVndkVersion()
	//fmt.Println("PlatformVndkVersion:", PlatformVndkVersion)
	//fmt.Println("len(PlatformVndkVersion:", len(PlatformVndkVersion)
    // After Andriod T, PlatformVndkVersion return string like "Tiramisu", not string number like "32"
    // minijail is used to protect against unexpected system calls.
    if len(PlatformVndkVersion) > 2 {
        p.Shared_libs = append(p.Shared_libs, "libavservices_minijail")
		//fmt.Println("use libavservices_minijail")
    } else {
        p.Shared_libs = append(p.Shared_libs, "libavservices_minijail_vendor")
    }
    ctx.AppendProperties(p)
}