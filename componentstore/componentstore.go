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
        cppflags = append(cppflags, "-DSUPPORT_SOFT_VDEC=1 -DSUPPORT_SOFT_AFFMPEG=1")
    }

    if vconfig.Bool("enable_hwcodec") == true {
        cppflags = append(cppflags, "-DSUPPORT_VDEC_AVS=1 -DSUPPORT_VDEC_AVS2=1 -DSUPPORT_VDEC_AVS3=1")
    }

    p.Cflags = cppflags
    ctx.AppendProperties(p)
}

