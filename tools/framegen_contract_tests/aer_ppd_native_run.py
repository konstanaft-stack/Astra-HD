"""Compile/test the production PPD native boundary without loading the game."""
from pathlib import Path
import argparse, ctypes, datetime, hashlib, json, os, re, subprocess, sys

def production_consumers(repo,out):
    swap=(repo/'src/Hooks/SwapChain.cpp').read_text(encoding='utf-8')
    fov=(repo/'src/Hooks/CameraFov.cpp').read_text(encoding='utf-8')
    def function(signature):
        start=swap.index(signature);brace=swap.index('{',start);depth=1;end=brace+1
        while depth:
            if swap[end]=='{':depth+=1
            elif swap[end]=='}':depth-=1
            end+=1
        return swap[start:end]
    result='namespace productionConsumers {\n'
    core=(repo/'src/Core/VrCore.cpp').read_text(encoding='utf-8')
    for name in ['GetForcedDisplayModeWidth','GetForcedDisplayModeHeight']:
        start=core.index('extern "C" UINT '+name+'(');brace=core.index('{',start);depth=1;end=brace+1
        while depth:
            depth+=(core[end]=='{')-(core[end]=='}');end+=1
        result+=core[start:end]+'\n'
    os_source=(repo/'src/Hooks/OsInput.cpp').read_text(encoding='utf-8')
    for signature in ['static cvr::ppd::NativeSize CurrentVirtualGameSize()',
                      'static BOOL WINAPI HookedGetClientRect(','static BOOL WINAPI HookedGetWindowRect(']:
        if signature not in os_source:continue
        start=os_source.index(signature);brace=os_source.index('{',start);depth=1;end=brace+1
        while depth:
            depth+=(os_source[end]=='{')-(os_source[end]=='}');end+=1
        result+=os_source[start:end]+'\n'
    for helper in ['NativeSizeForWindow','NativeSizeForSwapchain']:
        signature='static std::optional<cvr::ppd::NativeSizeRequest> '+helper
        if signature in swap:result+=function(signature)+'\n'
    result+=function('HRESULT STDMETHODCALLTYPE HookedResizeBuffers(')+'\n'
    result+=function('HRESULT STDMETHODCALLTYPE HookedResizeBuffers1(')+'\n'
    signatures=['HRESULT STDMETHODCALLTYPE DXGIFactoryWrapper::CreateSwapChain(',
        'static HRESULT STDMETHODCALLTYPE Detour_FacCreateSwapChain(',
        'static HRESULT STDMETHODCALLTYPE Detour_FacCreateSwapChainForHwnd(',
        'HRESULT STDMETHODCALLTYPE DXGIFactoryWrapper::CreateSwapChainForHwnd(']
    for i,signature in enumerate(signatures):
        body=function(signature)
        start=body.find('const auto nativeSize=')
        if start<0:start=body.index('const UINT ')
        pair=re.search(r'const UINT (forcedHeight|fh) = [^;]+;',body[start:])
        assert pair,signature
        snippet=body[start:start+pair.end()];width='forcedWidth' if pair[1]=='forcedHeight' else 'fw'
        result+=f'NativeSize ActualFactory{i}(DXGI_SWAP_CHAIN_DESC* pDesc,HWND hWnd){{(void)pDesc;(void)hWnd;\n'+snippet+f'\nreturn {{{width},{pair[1]}}};}}\n'
    start=fov.find('const auto nativeSize=')
    if start<0:start=fov.index('const float aspect =')
    result+='float ActualProjectionAspect(){\n'+fov[start:fov.index('float targetHDeg',start)]+'return aspect;\n}\n}\n'
    generated=out/'ppd-consumer-sites.hpp';generated.write_text(result,encoding='utf-8');return generated

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--label',required=True);ap.add_argument('--production-tus',action='store_true');ap.add_argument('--consumers',action='store_true');args=ap.parse_args()
    if not args.label.replace('-','').isalnum():raise ValueError('invalid label')
    repo=Path(__file__).resolve().parents[2];out=repo.parent.parent/'build/tofu-ppd-native'/args.label;out.mkdir(parents=True,exist_ok=True)
    core=(repo/'src/Core/VrCore.cpp').read_text(encoding='utf-8')
    hooks=(repo/'src/Hooks/SettingsRes.cpp').read_text(encoding='utf-8')
    begin=core.index('bool SettingsResolutionOverrideSuspended()');end=core.index('// Only the SETTINGS override',begin)
    callback=hooks[hooks.index('extern "C" void __fastcall OnSettingsResCallback'):hooks.index('bool InstallSettingsResHook()')]
    callsites=out/'ppd-call-sites.hpp'
    callsites.write_text('''#include "Utils/MemorySafe.hpp"
namespace productionCallSites {
uint64_t g_settingsResHits{};
std::atomic<uint64_t> CyberpunkVR_PpdSettingsHookVisits{0};
volatile uintptr_t g_settingsResPtr{};
volatile int g_verboseLog{};
uint32_t shared[82]{};
UINT width=2048,height=2048;
float* GetShotShared(){return reinterpret_cast<float*>(shared);}
UINT GetForcedWindowWidthValue(){return width;}
UINT GetForcedWindowHeightValue(){return height;}
UINT GetForcedRenderWidthValue(){return 100;}
UINT GetForcedRenderHeightValue(){return 100;}
void Log(const char*,...){}
// This existing PPD fixture covers the FG-off call-site contract. GPU/FG
// readiness is not exercised here; the production store/admission regression
// lives in tools/producer_reset_tests/native_boundary.cpp.
bool NativeFgResizeReady(){return true;}
'''+core[begin:end]+callback+'\n}\n',encoding='utf-8')
    consumer_sites=production_consumers(repo,out) if args.consumers else None
    consumer_source=Path(__file__).with_name('aer_ppd_consumers.cpp')
    checked=subprocess.run(['powershell.exe','-NoProfile','-Command',"$g=@(Get-Process -Name NMS,Cyberpunk2077 -ErrorAction SilentlyContinue); ConvertTo-Json -InputObject @($g|Select-Object ProcessName,Id) -Compress"],capture_output=True,text=True,check=True)
    games=json.loads(checked.stdout or '[]')
    if games:raise RuntimeError('game active')
    kernel=ctypes.WinDLL('kernel32',use_last_error=True);kernel.CreateMutexW.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_wchar_p];kernel.CreateMutexW.restype=ctypes.c_void_p
    kernel.WaitForSingleObject.argtypes=[ctypes.c_void_p,ctypes.c_uint32];kernel.WaitForSingleObject.restype=ctypes.c_uint32
    kernel.ReleaseMutex.argtypes=[ctypes.c_void_p];kernel.CloseHandle.argtypes=[ctypes.c_void_p]
    mutex=kernel.CreateMutexW(None,False,'Local\\IRD.SingleBuildTestDeploy.v1')
    if not mutex:raise ctypes.WinError(ctypes.get_last_error())
    if kernel.WaitForSingleObject(mutex,0) not in [0,0x80]:kernel.CloseHandle(mutex);raise RuntimeError('build/test owner busy')
    try:
        tool=Path('<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc');vc=tool/'VC/Tools/MSVC/14.44.35207';sdk=tool/'Windows Kits/10';ver='10.0.26100.0'
        env=os.environ.copy();env['PATH']=str(vc/'bin/Hostx64/x64')+';'+env['PATH']
        env['INCLUDE']=';'.join(map(str,[vc/'include']+[sdk/f'Include/{ver}/{x}' for x in ['ucrt','shared','um','winrt','cppwinrt']]))
        env['LIB']=';'.join(map(str,[vc/'lib/x64',sdk/f'Lib/{ver}/ucrt/x64',sdk/f'Lib/{ver}/um/x64']))
        source=repo/'src/Overlay/PpdNativeApply.cpp';test=Path(__file__).with_name('aer_ppd_native.cpp');exe=out/'ppd-native.exe';cl=str(vc/'bin/Hostx64/x64/cl.exe')
        command=[cl,'/nologo','/std:c++20','/EHsc','/O2','/W4','/WX','/permissive-','/utf-8','/I'+str(repo/'include'),'/I'+str(out),str(test),str(source),'/Fe:'+str(exe)]
        compiled=subprocess.run(command,cwd=out,env=env,text=True,capture_output=True);(out/'compile.log').write_text(compiled.stdout+compiled.stderr,encoding='utf-8')
        tested=None;units=[];consumer={}
        if compiled.returncode==0:
            tested=subprocess.run([str(exe)],cwd=out,text=True,capture_output=True,timeout=30);(out/'tests.log').write_text(tested.stdout+tested.stderr,encoding='utf-8');print(tested.stdout+tested.stderr)
        else:print(compiled.stdout+compiled.stderr)
        if args.consumers and tested and tested.returncode==0:
            consumer_exe=out/'ppd-consumers.exe'
            cmd=command.copy();cmd[cmd.index(str(test))]=str(consumer_source);cmd[-1]='/Fe:'+str(consumer_exe)
            cc=subprocess.run(cmd,cwd=out,env=env,text=True,capture_output=True);(out/'consumer-compile.log').write_text(cc.stdout+cc.stderr,encoding='utf-8');consumer={'compileExit':cc.returncode,'testExit':None,'command':cmd}
            if cc.returncode==0:
                done=subprocess.run([str(consumer_exe)],cwd=out,text=True,capture_output=True,timeout=30);(out/'consumer-tests.log').write_text(done.stdout+done.stderr,encoding='utf-8');consumer['testExit']=done.returncode;print(done.stdout+done.stderr)
            else:print(cc.stdout+cc.stderr)
        if args.production_tus and tested and tested.returncode==0 and (not args.consumers or consumer.get('testExit')==0):
            lines=(repo.parent.parent/'build/tofu-ru/build.ninja').read_text(encoding='utf-8').splitlines()
            for relative in ['src/Overlay/PpdNativeApply.cpp','src/Hooks/SettingsRes.cpp','src/Core/VrCore.cpp']+(['src/Hooks/SwapChain.cpp','src/Hooks/CameraFov.cpp'] if args.consumers else []):
                suffix=relative.replace('/','\\')+'.obj:'
                at=next((i for i,line in enumerate(lines) if line.startswith('build ') and suffix in line),None)
                if at is None:at=next(i for i,line in enumerate(lines) if line.startswith('build ') and r'\src\Hooks\SettingsRes.cpp.obj:' in line)
                properties={}
                for line in lines[at+1:]:
                    if not line.strip():break
                    if ' = ' in line:k,v=line.strip().split(' = ',1);properties[k]=v
                stem=Path(relative).stem
                cmd=[cl,'/nologo']+properties['DEFINES'].split()+properties['FLAGS'].split()+properties['INCLUDES'].split()+['/c',str(repo/relative),'/Fo:'+str(out/(stem+'-production.obj')),'/Fd:'+str(out/(stem+'.pdb'))]
                cc=subprocess.run(cmd,cwd=out,env=env,text=True,capture_output=True,timeout=90)
                (out/(stem+'-compile.log')).write_text(cc.stdout+cc.stderr,encoding='utf-8');units.append({'source':relative,'exit':cc.returncode,'command':cmd});print('TU '+relative+' exit='+str(cc.returncode))
                if cc.returncode:print(cc.stdout+cc.stderr);break
        files=[source,repo/'include/Overlay/PpdNativeApply.hpp',repo/'src/Hooks/SettingsRes.cpp',repo/'src/Core/VrCore.cpp',repo/'include/Core/VrCoreShared.hpp',test,Path(__file__),callsites]
        if args.consumers:files+=[repo/'src/Hooks/SwapChain.cpp',repo/'src/Hooks/CameraFov.cpp',consumer_source,consumer_sites]
        receipt={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'games':games,'compileExit':compiled.returncode,'testExit':None if tested is None else tested.returncode,'consumers':consumer,'productionUnits':units,'command':command,'files':[{'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest().upper()} for p in files]}
        (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8');print('EVIDENCE '+str(out))
        return compiled.returncode or (tested.returncode if tested else 1) or consumer.get('compileExit',0) or consumer.get('testExit',0) or next((u['exit'] for u in units if u['exit']),0)
    finally:kernel.ReleaseMutex(mutex);kernel.CloseHandle(mutex)
if __name__=='__main__':sys.exit(main())
