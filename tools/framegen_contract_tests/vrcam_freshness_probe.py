"""Compile the actual freshness getter with controlled clocks/submission metadata.

This is a CPU contract regression, not a GPU or headset acceptance test.
The production function is extracted verbatim; no replacement gate is tested.
"""
from pathlib import Path
import argparse
import os
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
text = (root / "src/Stereo/Capture.cpp").read_text(encoding="utf-8")
start = text.index('extern "C" __declspec(dllexport) ID3D12Resource* CyberpunkVR_GetVrcamEyeTextureFresh()')
end = text.index("\n}\n", start) + 3
body = text[start:end]
stub = r'''
#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <string>
struct ID3D12Resource {};
ID3D12Resource oldImage, newImage;
uint64_t clockMs=2000;
uint64_t GetTickCount64(){return clockMs;}
double StableNowMs(){return double(clockMs);}
std::atomic<uint64_t> g_stable_tick{2000},g_stable_tick_us{2000000};
std::atomic<uint64_t> g_eye_node_hits{},g_eye_no_rtv{},g_eye_no_list{},g_eye_copy_calls{};
std::atomic<unsigned> g_mirror_rtv_candidate_count{};
std::array<int,512> g_mirror_rtv_candidates{};
uint32_t CyberpunkVR_DebugVrcamEyeAgeMs{},CyberpunkVR_DebugVrcamEyeAgeUs{};
uint32_t CyberpunkVR_StereoEyeMaxAgeMs=100,CyberpunkVR_VrcamOwnTarget=0;
uint64_t CyberpunkVR_DebugStableCopies{},CyberpunkVR_DebugStableSkips{};
uint64_t CyberpunkVR_DebugMirrorRtvRegs{},CyberpunkVR_DebugMirrorRtvEvicts{};
int CyberpunkVR_DebugRtvPickLog{};
template<class... T> void log(const char*,T...){}
namespace cvr {
bool RuntimeDiagnosticsEnabled(){return false;}
namespace camera {
struct ImagePoseIdentity {
    uintptr_t resource{}; uint64_t generation{},stampMs{};
    explicit operator bool() const {return generation!=0;}
};
ImagePoseIdentity published{reinterpret_cast<uintptr_t>(&oldImage),1,1000};
ImagePoseIdentity LatestSubmittedVrcamImageIdentity(){return published;}
}}
ID3D12Resource* CyberpunkVR_GetVrcamEyeTexture(){return &oldImage;}
'''
tests = r'''
int main(int argc,char** argv){
    const std::string test=argc>1?argv[1]:"stale_recording";
    ID3D12Resource* expected=nullptr;
    if(test=="fresh_submission"){
        g_stable_tick=1000;
        cvr::camera::published={reinterpret_cast<uintptr_t>(&newImage),2,1990};
        expected=&newImage;
    } else if(test=="missing_submission"){
        cvr::camera::published={};
    } else if(test=="future_stamp"){
        cvr::camera::published.stampMs=2010;
    } else if(test=="boundary"){
        cvr::camera::published.stampMs=1900;
        expected=&oldImage;
    } else if(test=="legacy_own_target"){
        CyberpunkVR_VrcamOwnTarget=1;
        expected=&oldImage;
    } else if(test!="stale_recording") return 2;
    const auto actual=CyberpunkVR_GetVrcamEyeTextureFresh();
    if(actual!=expected){std::cerr<<test<<": freshness selected wrong image\n";return 1;}
    std::cout<<test<<": PASS\n";
}
'''
args.output.mkdir(parents=True, exist_ok=True)
source = args.output / "vrcam_freshness.cpp"
source.write_text(stub + body + tests, encoding="utf-8")
vc = Path(r"<WORKSPACE_ROOT>\tools\Cyberpunk-Modding\MSVC-2022\msvc\VC\Tools\MSVC\14.44.35207")
sdk = Path(r"<WORKSPACE_ROOT>\tools\Cyberpunk-Modding\MSVC-2022\msvc\Windows Kits\10")
version = "10.0.26100.0"
env = os.environ.copy()
env["INCLUDE"] = ";".join(map(str,[vc/"include",sdk/f"Include/{version}/ucrt",sdk/f"Include/{version}/shared",sdk/f"Include/{version}/um"]))
env["LIB"] = ";".join(map(str,[vc/"lib/x64",sdk/f"Lib/{version}/ucrt/x64",sdk/f"Lib/{version}/um/x64"]))
env["PATH"] = str(vc/"bin/Hostx64/x64") + ";" + env["PATH"]
exe = args.output / "vrcam_freshness.exe"
build = subprocess.run([str(vc/"bin/Hostx64/x64/cl.exe"),"/nologo","/std:c++20","/EHsc","/utf-8",str(source),"/Fe:"+str(exe)],cwd=args.output,env=env,capture_output=True,text=True)
(args.output/"build.log").write_text(build.stdout+build.stderr,encoding="utf-8")
if build.returncode:
    print(build.stdout,build.stderr)
    raise SystemExit(build.returncode)
failed=0
results=[]
for case in ("stale_recording","fresh_submission","missing_submission","future_stamp","boundary","legacy_own_target"):
    result=subprocess.run([str(exe),case],capture_output=True,text=True)
    results.append(result.stdout+result.stderr)
    print(results[-1].strip())
    failed += result.returncode!=0
(args.output/"results.txt").write_text("".join(results),encoding="utf-8")
raise SystemExit(bool(failed))
