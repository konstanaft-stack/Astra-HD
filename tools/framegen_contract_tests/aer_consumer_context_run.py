"""Bounded CPU verification of exact current context cleanup and PPD gate."""
from pathlib import Path
import argparse, ctypes, datetime, json, os, subprocess, sys
from aer_consumer_fragments_run import digest

def extract(source,red):
    begin=source.index('bool aerContextHandled=false;',source.index('DWORD OpenXRManager::FrameThreadMain()'))
    end=source.index('uint64_t monoWaitLogCounter',begin)
    prefix=source[begin:end]
    begin=source.index('if(!monoReady || viewCountOutput!=m_eyeSwapchains.size()')
    end=source.index('aerContextHandled=true;',begin)+len('aerContextHandled=true;')
    cleanup=source[begin:end]
    begin=source.index('const bool needRealReceipt=')
    gate=source[begin:source.index(';',begin)+1]
    if red:
        # Negative controls remove the two reviewed repairs only in this fixture.
        prefix=prefix.replace('~ContextExit(){if(!handled)invalidate();}','~ContextExit(){}')
        cleanup='if(monoReady && viewCountOutput==m_eyeSwapchains.size()) {\n'+cleanup+'\n}'
        gate=gate.replace('!ppdObservation || ppd.measuredSource.scope!=ppdObservation->scope ||','')
    return f'''// Exact production fragments; negative control={red}.
struct ActualManager {{
    fixture::HeldPair m_aerRealPresenter;fixture::Framegen m_framegen;
    fixture::Pass pass;fixture::Pass* m_aerReconstruction=&pass;
    std::vector<int> m_eyeSwapchains=std::vector<int>(2);
    uint64_t GetTrackingOriginSerial()const{{return 42;}}
    void Run(bool earlyReturn,bool earlyContinue,bool monoReady,unsigned viewCountOutput,bool menuRectActive){{
        {prefix}
        if(earlyReturn)return;
        do {{
            if(earlyContinue)continue;
            {cleanup}
        }} while(false);
    }}
}};
static bool ActualNeedRealReceipt(const cvr::ppd::PpdPanelSnapshot& ppd,
    const std::optional<cvr::ppd::PpdSourceObservation>& ppdObservation){{
    (void)ppdObservation;
    {gate}
    return needRealReceipt;
}}
'''

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--label',required=True);parser.add_argument('--red',action='store_true');args=parser.parse_args()
    if not args.label.replace('-','').isalnum():raise ValueError('invalid label')
    repo=Path(__file__).resolve().parents[2];out=repo.parent.parent/'build/tofu-aer-consumer-review'/args.label;out.mkdir(parents=True,exist_ok=True)
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.CreateMutexW.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_wchar_p];kernel.CreateMutexW.restype=ctypes.c_void_p
    kernel.WaitForSingleObject.argtypes=[ctypes.c_void_p,ctypes.c_uint32];kernel.WaitForSingleObject.restype=ctypes.c_uint32
    kernel.ReleaseMutex.argtypes=[ctypes.c_void_p];kernel.CloseHandle.argtypes=[ctypes.c_void_p]
    mutex=kernel.CreateMutexW(None,False,'Local\\IRD.SingleBuildTestDeploy.v1')
    if not mutex:raise ctypes.WinError(ctypes.get_last_error())
    if kernel.WaitForSingleObject(mutex,0) not in [0,0x80]:kernel.CloseHandle(mutex);raise RuntimeError('build/test owner busy')
    try:
        snapshot=out/'OpenXRFrameLoop.cpp.snapshot';snapshot.write_bytes((repo/'src/Runtimes/OpenXRFrameLoop.cpp').read_bytes())
        generated=out/'aer-context-sites.hpp';generated.write_text(extract(snapshot.read_text(encoding='utf-8'),args.red),encoding='utf-8')
        tool=Path('<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc');vc=tool/'VC/Tools/MSVC/14.44.35207';sdk=tool/'Windows Kits/10';v='10.0.26100.0'
        env=os.environ.copy();env['PATH']=str(vc/'bin/Hostx64/x64')+';'+env['PATH']
        env['INCLUDE']=';'.join(map(str,[vc/'include']+[sdk/f'Include/{v}/{part}' for part in ['ucrt','shared','um','winrt','cppwinrt']]))
        env['LIB']=';'.join(map(str,[vc/'lib/x64',sdk/f'Lib/{v}/ucrt/x64',sdk/f'Lib/{v}/um/x64']))
        test=Path(__file__).with_name('aer_consumer_context.cpp');exe=out/'aer-consumer-context.exe'
        command=[str(vc/'bin/Hostx64/x64/cl.exe'),'/nologo','/EHsc','/O2','/std:c++20','/W4','/WX','/MT','/DNOMINMAX','/utf-8',
            '/I'+str(repo/'include'),'/I'+str(out),'/Fe:'+str(exe),str(test)]
        cc=subprocess.run(command,cwd=out,env=env,text=True,capture_output=True,timeout=30)
        (out/'compile.log').write_text(cc.stdout+cc.stderr,encoding='utf-8');done=None
        if cc.returncode==0:
            done=subprocess.run([str(exe)],cwd=out,text=True,capture_output=True,timeout=10)
            (out/'tests.log').write_text(done.stdout+done.stderr,encoding='utf-8');print(done.stdout+done.stderr)
        else:print(cc.stdout+cc.stderr)
        files=[test,Path(__file__),Path(__file__).with_name('aer_consumer_fragments_run.py'),snapshot,generated,
            repo/'include/Overlay/PpdPanel.hpp',repo/'include/Overlay/PpdCalibration.hpp',repo/'include/Overlay/PpdNativeApply.hpp']
        receipt={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'gpuUsed':False,'red':args.red,'compileExit':cc.returncode,
            'testExit':None if done is None else done.returncode,'command':command,'exeSha256':digest(exe) if exe.exists() else None,
            'files':[{'path':str(path),'sha256':digest(path)} for path in files]}
        (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8');print('EVIDENCE '+str(out))
        return 0 if cc.returncode==0 and done and done.returncode==(1 if args.red else 0) else 1
    finally:kernel.ReleaseMutex(mutex);kernel.CloseHandle(mutex)

if __name__=='__main__':sys.exit(main())
