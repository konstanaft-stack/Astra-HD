"""Compile the actual FinalCamera write block with observable camera boundaries."""
from pathlib import Path
import argparse, ctypes, datetime, hashlib, json, os, subprocess, sys

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--label',required=True);ap.add_argument('--production-tu',action='store_true');args=ap.parse_args()
    if not args.label.replace('-','').isalnum():raise ValueError('invalid label')
    repo=Path(__file__).resolve().parents[2];out=repo.parent.parent/'build/tofu-aer-policy/final-camera'/args.label;out.mkdir(parents=True,exist_ok=True)
    source=repo/'src/Hooks/FinalCamera.cpp';text=source.read_text(encoding='utf-8')
    marker=text.index('// ---- MONO: THE PER-VIEW WRITE SITE')
    start=text.index('    {',marker);end=text.index('    // THE LATE IPD SHIFT',start)
    block=text[start:end]
    assert text.index('PushRenderedFramePose(matched)')<marker
    assert text.index('BarrelFramePublish(bf)')<marker
    assert 'WriteRenderCameraBasis' in block and 'posFP[i] +=' in block
    harness=r'''
#include <Windows.h>
#include <cstdint>
#include <iostream>
#include <stdexcept>
int CyberpunkVR_CamWriteInFinal=1,CyberpunkVR_CamFinalViewScope=1;
int CyberpunkVR_DebugViewCamMain{},CyberpunkVR_DebugViewCamVrcam{},CyberpunkVR_DebugViewCamOther{};
bool g_headQuatValid=true,requested=false,mainView=true,vrcamView=false;
int basisWrites{},sampleReads{};
#define CVR_DIAGNOSTIC(expr) (expr)
namespace cvr::camera {
bool MainEyeSourceRequested(){return requested;}
bool CamWriteQuatRead(float out[4]){++sampleReads;out[0]=out[1]=out[2]=0;out[3]=1;return true;}
}
int CyberpunkVR_IsVrcamViewActive(){return vrcamView;}
int CyberpunkVR_IsMainViewActive(){return mainView;}
bool IsPlausibleUnitQuaternion(const float*){return true;}
bool IsPlausibleUnitVector3(const float*){return true;}
float GetDesiredHalfIpd(){return 0.032f;}
void ComputeRightVectorFromQuaternion(const float*,float out[3]){out[0]=1;out[1]=out[2]=0;}
void WriteRenderCameraBasis(float* ptr,const float*){++basisWrites;ptr[20]=77;}
void ActualProductionWriteBlock(float* rsiPtr){
'''+block+r'''
}
void Check(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
void Test(bool aer,bool flag,bool main,bool vrcam,bool expectWrite){
    float camera[64]{};basisWrites=sampleReads=0;requested=aer;CyberpunkVR_CamWriteInFinal=flag;
    mainView=main;vrcamView=vrcam;ActualProductionWriteBlock(camera);
    const auto x=reinterpret_cast<const int32_t*>(camera)[0];
    if(!expectWrite)Check(basisWrites==0 && sampleReads==0 && x==0 && camera[20]==0,"active source or disabled flag allowed an unbound basis/IPD write");
    else {
        Check(basisWrites==1 && sampleReads==1 && camera[20]==77,"OFF mode stopped existing final camera write");
        if(vrcam)Check(x>0,"legacy VRCAM eye shift changed");
        else if(main)Check(x<0,"legacy MAIN eye shift changed");
        else Check(x==0,"unclassified legacy view gained IPD");
    }
}
int main(){int passed=0,failed=0;
    struct Case{const char* name;bool aer,flag,main,vrcam,write;};
    const Case cases[]={{"active_main",true,true,true,false,false},{"active_vrcam",true,true,false,true,false},
        {"active_other",true,true,false,false,false},{"flag_off",false,false,true,false,false},
        {"off_main",false,true,true,false,true},{"off_vrcam",false,true,false,true,true},
        {"off_other",false,true,false,false,true}};
    for(const auto& c:cases)try{Test(c.aer,c.flag,c.main,c.vrcam,c.write);++passed;std::cout<<"PASS "<<c.name<<'\n';}
        catch(const std::exception& e){++failed;std::cout<<"FAIL "<<c.name<<": "<<e.what()<<'\n';}
    std::cout<<"RESULT "<<passed<<" passed, "<<failed<<" failed\n";return failed?1:0;}
'''
    cpp=out/'actual-final-write.cpp';cpp.write_text(harness,encoding='utf-8')
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
        cl=str(vc/'bin/Hostx64/x64/cl.exe');exe=out/'final-write.exe'
        command=[cl,'/nologo','/std:c++20','/EHsc','/O2','/W4','/WX','/permissive-','/utf-8',str(cpp),'/Fe:'+str(exe),'/Fo:'+str(out/'final-write.obj')]
        compiled=subprocess.run(command,cwd=out,env=env,text=True,capture_output=True);(out/'compile.log').write_text(compiled.stdout+compiled.stderr,encoding='utf-8')
        tested=None;production=None
        if compiled.returncode==0:
            tested=subprocess.run([str(exe)],cwd=out,text=True,capture_output=True);(out/'tests.log').write_text(tested.stdout+tested.stderr,encoding='utf-8');print(tested.stdout+tested.stderr)
        else:print(compiled.stdout+compiled.stderr)
        if args.production_tu and tested and tested.returncode==0:
            lines=(repo.parent.parent/'build/tofu-ru/build.ninja').read_text(encoding='utf-8').splitlines()
            at=next(i for i,line in enumerate(lines) if line.startswith('build ') and r'\src\Hooks\FinalCamera.cpp.obj:' in line)
            properties={}
            for line in lines[at+1:]:
                if not line.strip():break
                if ' = ' in line:k,v=line.strip().split(' = ',1);properties[k]=v
            command=[cl,'/nologo']+properties['DEFINES'].split()+properties['FLAGS'].split()+properties['INCLUDES'].split()+['/c',str(source),'/Fo:'+str(out/'FinalCamera.obj'),'/Fd:'+str(out/'FinalCamera.pdb')]
            production=subprocess.run(command,cwd=out,env=env,text=True,capture_output=True);(out/'production-compile.log').write_text(production.stdout+production.stderr,encoding='utf-8');print('production TU exit='+str(production.returncode))
        paths=[source,Path(__file__),cpp]
        receipt={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'games':games,'compileExit':compiled.returncode,'testExit':None if tested is None else tested.returncode,'productionCompileExit':None if production is None else production.returncode,'readbackPrecedesWriteBlock':True,'files':[{'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest().upper()} for p in paths]}
        (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8');print('EVIDENCE '+str(out))
        return compiled.returncode or (tested.returncode if tested else 1) or (production.returncode if production else 0)
    finally:kernel.ReleaseMutex(mutex);kernel.CloseHandle(mutex)
if __name__=='__main__':sys.exit(main())
