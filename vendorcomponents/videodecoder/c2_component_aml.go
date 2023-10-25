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
    "os/exec"
    "fmt"
    "strconv"
    "strings"
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
    var gitCommitIDShort string
    var gitCommitID string
    var gitChangeID string
    var gitCommitMsg string;
    var gitPD string
    var major_v string
    var minor_v string
    var baseCommitId string
    var commitCnt string
    var versionStr string

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

    currentdir := string(ctx.AConfig().Getenv("PWD")) + "/vendor/amlogic/common/codec2"

    execCmd := "cd " + currentdir + "&& git rev-parse --short HEAD"
    ret, err := exec.Command("/bin/bash", "-c", execCmd).CombinedOutput()
    if err != nil {
        //fmt.Printf("get commitidshort err: %s\n", err)
    }
    gitCommitIDShort = strings.Replace(string(ret), "\n", "", -1)
    //fmt.Printf("get commitid:%s\n", gitCommitIDShort)

    execCmd = "cd " + currentdir + "&& git rev-parse HEAD"
    ret, err = exec.Command("/bin/bash", "-c", execCmd).CombinedOutput()
    if err != nil {
        //fmt.Printf("get commitid err: %s\n", err)
    }
    gitCommitID = strings.Replace(string(ret), "\n", "", -1)
    //fmt.Printf("get commitid:%s\n", gitCommitID)GIT_COMMITID := "-DCOMMIT_CHANGEID=" + gitCommitID
    GIT_COMMITID := "-DGIT_COMMITID=" +  "\"" + gitCommitID +  "\""
    cppflags = append(cppflags, GIT_COMMITID)

    execCmd = "cd " + currentdir + "&& git log -1 | grep Change-Id | awk '{print $2}'"
    ret, err = exec.Command("/bin/bash", "-c", execCmd).CombinedOutput()
    if err != nil {
        //fmt.Printf("get changeid err: %s\n", err)
    }
    gitChangeID = strings.Replace(string(ret), "\n", "", -1)
    GIT_CHANGEID := "-DGIT_CHANGEID=" +  "\"" + gitChangeID +  "\""
    cppflags = append(cppflags, GIT_CHANGEID)

    //execCmd = "cd " + currentdir + "&& git log -1 | grep ] | sed 's/\"/\\\"/g'"
    execCmd = fmt.Sprintf("cd %s && git log -1 | grep ']' | sed 's/\"/\\\\\"/g'", string(currentdir))
    ret, err = exec.Command("/bin/bash", "-c", execCmd).CombinedOutput()
    if err != nil {
        //fmt.Printf("get commitmsg err: %s\n", err)
    }
    gitCommitMsg = strings.Replace(string(ret), "\n", "", -1)
    gitCommitMsg = strings.TrimSpace(string(gitCommitMsg))
    GIT_COMMITMSG := "-DGIT_COMMITMSG=" +  "\"" + gitCommitMsg +  "\""
    cppflags = append(cppflags, GIT_COMMITMSG)

    execCmd = "cd " + currentdir + "&& git log -1 | grep 'PD#'"
    ret, err = exec.Command("/bin/bash", "-c", execCmd).CombinedOutput()
    if err != nil {
        //fmt.Printf("get pd err: %s\n", err)
    }
    gitPD = strings.Replace(string(ret), "\n", "", -1)
    gitPD = strings.Replace(gitPD, " ", "", -1)
    GIT_PD := "-DGIT_PD=" +  "\"" + gitPD +  "\""
    cppflags = append(cppflags, GIT_PD)
    //fmt.Printf("get pd:%s\n", gitPD)

    execCmd = "cd " + currentdir + "&& cat VERSION | grep Major_V | awk -F'=' '{print $2}'"
    ret, err = exec.Command("/bin/bash", "-c", execCmd).CombinedOutput()
    if err != nil {
        //fmt.Printf("get major_v err: %s\n", err)
    }
    major_v = strings.Replace(string(ret), "\n", "", -1)
    //fmt.Printf("get major_v:%s\n", major_v)

    execCmd = "cd " + currentdir + "&& cat VERSION | grep Minor_V | awk -F'=' '{print $2}'"
    ret, err = exec.Command("/bin/bash", "-c", execCmd).CombinedOutput()
    if err != nil {
        //fmt.Printf("get minor_v err: %s\n", err)
    }
    minor_v = strings.Replace(string(ret), "\n", "", -1)
    //fmt.Printf("get major:%s\n", string(minor_v))

    execCmd = "cd " + currentdir + "&& cat VERSION | grep BaseCommitId | awk -F'=' '{print $2}'"
    ret, err = exec.Command("/bin/bash", "-c", execCmd).CombinedOutput()
    if err != nil {
        //fmt.Printf("get baseCommitid err: %s\n", err)
    }
    baseCommitId = strings.Replace(string(ret), "\n", "", -1)
    //fmt.Printf("get basecommited:%s\n", string(baseCommitId))

    execCmd = fmt.Sprintf("cd %s && git log | grep -E '^commit' | grep -n %s | awk -F':' '{print $1-1}'", string(currentdir), baseCommitId)
    //fmt.Printf("str:%s\n", string(execCmd))
    ret, err = exec.Command("/bin/bash", "-c", execCmd).CombinedOutput()
    if err != nil {
        //fmt.Printf("get commitCnt err: %s\n", err)
    }
    commitCnt = strings.Replace(string(ret), "\n", "", -1)
    //fmt.Printf("get commitCnt:%s\n", string(commitCnt))

    versionStr = fmt.Sprintf("V%s.%s.%s-g%s", major_v, minor_v, commitCnt, gitCommitIDShort);
    //fmt.Printf("codec2 version:%s\n", string(versionStr))

    execCmd = fmt.Sprintf("cd %s && cat featurelist.json | sed '1s/^.//' | tr -d '\n\r ' | sed 's/\"/\\\\\"/g'", string(currentdir))
    ret, err = exec.Command("/bin/bash", "-c", execCmd).CombinedOutput()
    if err != nil {
        //fmt.Printf("get featurelist err: %s\n", err)
    }
    featurelist := strings.Replace(string(ret), "\n", "", -1)
    //fmt.Printf("get featurelist:%s\n", string(featurelist))
    MEDIA_MODULE_FEATURES :="-DMEDIA_MODULE_FEATURES=" + "\"" + "\\n" + "{\\\"MM-module-name\\\":\\\"CODEC2\\\",\\\"Version\\\":\\\"" + string(versionStr)+ "\\\"," +string(featurelist) + "\\n"+"\""
    cppflags = append(cppflags, MEDIA_MODULE_FEATURES)

    CODEc2_VERSION := "-DVERSION=" + "\"" + versionStr +  "\""
    cppflags = append(cppflags, CODEc2_VERSION)

    HAVE_VERSOIN_INFO := "-DHAVE_VERSION_INFO"
    cppflags = append(cppflags, HAVE_VERSOIN_INFO)

    return cppflags
}
