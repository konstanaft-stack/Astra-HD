"""Extract current consumer statements and test real WARP pass lifetime + FOV.

--red reinstates only the two reviewed old fragments, without editing production.
The generated header is retained with its source hash in the evidence directory.
"""
from pathlib import Path
import argparse, ctypes, datetime, hashlib, json, os, subprocess, sys

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()

def brace_block(text, start):
    begin=text.index('{',start);depth=0
    for i in range(begin,len(text)):
        if text[i]=='{':depth+=1
        elif text[i]=='}':
            depth-=1
            if not depth:return text[start:i+1]
    raise ValueError('unterminated production block')

def fragments(source, red):
    completion_start=source.index('if(m_aerReconstruction)for(auto& ticket:aerTickets)if(ticket)',source.index('capturedReadFence=m_fenceValue;'))
    completion=brace_block(source,completion_start)
    fov_start=source.index('const bool bothEyesShareOneImage =')
    if_start=source.index('if (bothEyesShareOneImage',fov_start)
    fov=source[fov_start:if_start]+brace_block(source,if_start)
    if red:
        completion='''if(m_aerReconstruction)for(auto& ticket:aerTickets)if(ticket) {
            m_aerReconstruction->SubmissionReported(ticket,capturedCopySubmitted,capturedCopySignaled);
            ticket={};
        }'''
        fov='const bool bothEyesShareOneImage = !vrcamEye;\n'+brace_block(source,if_start)
    return f'''// Automatically extracted from OpenXRFrameLoop.cpp; {'RED old behavior' if red else 'exact production statements'}.
template<class Pass> static void ActualCompletion(Pass* m_aerReconstruction,
    std::array<AerRecordTicket,2>& aerTickets,bool capturedCopySubmitted,bool capturedCopySignaled){{
    {completion}
}}
static void ActualFov(bool aerSelected,bool fgSelected,const FakeSelection& fgSelection,
    void* vrcamEye,float CyberpunkVR_FlatDistanceM,bool CyberpunkVR_MainIsRightEye,float runtimeIpd,
    std::array<ProjectionView,2>& projectionViews,unsigned eye){{
    (void)aerSelected;(void)fgSelected;(void)fgSelection;
    const auto GetRuntimeIpd=[&]{{return runtimeIpd;}};
    {fov}
}}
'''

def game_guard():
    result=subprocess.run(['powershell.exe','-NoProfile','-Command',
        "$g=@(Get-Process -Name NMS,Cyberpunk2077 -ErrorAction SilentlyContinue); ConvertTo-Json -InputObject @($g|Select-Object ProcessName,Id) -Compress"],capture_output=True,text=True,check=True)
    games=json.loads(result.stdout or '[]')
    if games:raise RuntimeError('HOLD_USER_GAME')
    return {'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'games':games,'adapter':'WARP software only'}

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--label',required=True);parser.add_argument('--red',action='store_true');args=parser.parse_args()
    if not args.label.replace('-','').isalnum():raise ValueError('invalid label')
    repo=Path(__file__).resolve().parents[2];out=repo.parent.parent/'build/tofu-aer-consumer-review'/args.label;out.mkdir(parents=True,exist_ok=True)
    guard=game_guard()
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.CreateMutexW.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_wchar_p];kernel.CreateMutexW.restype=ctypes.c_void_p
    kernel.WaitForSingleObject.argtypes=[ctypes.c_void_p,ctypes.c_uint32];kernel.WaitForSingleObject.restype=ctypes.c_uint32
    kernel.ReleaseMutex.argtypes=[ctypes.c_void_p];kernel.CloseHandle.argtypes=[ctypes.c_void_p]
    mutex=kernel.CreateMutexW(None,False,'Local\\IRD.SingleBuildTestDeploy.v1')
    if not mutex:raise ctypes.WinError(ctypes.get_last_error())
    if kernel.WaitForSingleObject(mutex,0) not in [0,0x80]:kernel.CloseHandle(mutex);raise RuntimeError('build/test owner busy')
    try:
        # Pin the actual extraction input before compilation. Other owners may
        # edit unrelated consumer lines while this independent fixture builds.
        snapshot=out/'OpenXRFrameLoop.cpp.snapshot'
        snapshot.write_bytes((repo/'src/Runtimes/OpenXRFrameLoop.cpp').read_bytes())
        generated=out/'aer-consumer-sites.hpp';generated.write_text(fragments(snapshot.read_text(encoding='utf-8'),args.red),encoding='utf-8')
        tool=Path('<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc');vc=tool/'VC/Tools/MSVC/14.44.35207';sdk=tool/'Windows Kits/10';v='10.0.26100.0'
        env=os.environ.copy();env['PATH']=str(vc/'bin/Hostx64/x64')+';'+env['PATH']
        env['INCLUDE']=';'.join(map(str,[vc/'include']+[sdk/f'Include/{v}/{part}' for part in ['ucrt','shared','um','winrt','cppwinrt']]))
        env['LIB']=';'.join(map(str,[vc/'lib/x64',sdk/f'Lib/{v}/ucrt/x64',sdk/f'Lib/{v}/um/x64']))
        test=Path(__file__).with_name('aer_consumer_fragments.cpp');production=repo/'src/Render/AerReconstructionPass.cpp';exe=out/'aer-consumer-fragments.exe'
        command=[str(vc/'bin/Hostx64/x64/cl.exe'),'/nologo','/EHsc','/O2','/std:c++20','/W4','/WX','/MT','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN',
            '/I'+str(repo/'include'),'/I'+str(out),'/Fe:'+str(exe),str(test),str(production),'/link','d3d12.lib','dxgi.lib','d3dcompiler.lib']
        cc=subprocess.run(command,cwd=out,env=env,text=True,capture_output=True,timeout=90)
        (out/'compile.log').write_text(cc.stdout+cc.stderr,encoding='utf-8')
        done=None
        if cc.returncode==0:
            guard=game_guard()
            done=subprocess.run([str(exe)],cwd=out,text=True,capture_output=True,timeout=60)
            (out/'tests.log').write_text(done.stdout+done.stderr,encoding='utf-8');print(done.stdout+done.stderr)
        else:print(cc.stdout+cc.stderr)
        files=[test,Path(__file__),generated,snapshot,production,repo/'include/Render/AerReconstructionPass.hpp',repo/'src/Render/AerReconstructionShaders.hpp']
        receipt={'guard':guard,'red':args.red,'compileExit':cc.returncode,'testExit':None if done is None else done.returncode,
            'command':command,'exeSha256':digest(exe) if exe.exists() else None,
            'files':[{'path':str(path),'sha256':digest(path)} for path in files]}
        (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8');print('EVIDENCE '+str(out))
        expected=1 if args.red else 0
        return 0 if cc.returncode==0 and done and done.returncode==expected else 1
    finally:kernel.ReleaseMutex(mutex);kernel.CloseHandle(mutex)

if __name__=='__main__':sys.exit(main())
