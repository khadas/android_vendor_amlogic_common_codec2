//
// Copyright (C) 2023 Amlogic, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

package c2_component_aml

import (
    "android/soong/android"
    "android/soong/cc"
    "fmt"
    "strconv"
)

func init() {
    android.RegisterModuleType("c2_component_aml_go_defaults",codec_DefaultsFactory)
}

func codec_DefaultsFactory() (android.Module) {
    module := cc.DefaultsFactory()
    android.AddLoadHook(module, c2_component_aml_Defaults)
    return module
}

func c2_component_aml_Defaults(ctx android.LoadHookContext) {
     type propsE struct {
        Shared_libs  []string
        Cflags []string
    }
    p := &propsE{}
    p.Cflags = getVersionInfo(ctx)
    ctx.AppendProperties(p)
}


func getVersionInfo(ctx android.LoadHookContext) ([]string) {
    var cppflags []string
    //var dir = string(ctx.AConfig().Getenv("PWD"))
    //sdkVersion,_ := strconv.Atoi(ctx.DeviceConfig().PlatformVndkVersion())
    sdkVersionstr := ctx.DeviceConfig().PlatformVndkVersion()

    PlatformVndkVersion,err := strconv.Atoi(sdkVersionstr);
    if err != nil {
        fmt.Printf("%v like UpsideDownCake may fail to convert", PlatformVndkVersion)
    }
    if (PlatformVndkVersion >= 34) {
        BUFFERUSAGE := "-DSUPPORT_GRALLOC_REPLACE_BUFFER_USAGE"
        cppflags = append(cppflags, BUFFERUSAGE)
        //fmt.Println("use BUFFER_USAGE")
    }
    SDKVERSION := "-DANDROID_PLATFORM_SDK_VERSION=" + sdkVersionstr
    cppflags = append(cppflags, SDKVERSION)
    fmt.Println("c2 PlatformVndkVersion:", SDKVERSION)

    return cppflags
}
