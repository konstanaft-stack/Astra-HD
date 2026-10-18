"""Isolated CPU/production-header runner for immutable MAIN eye source receipts."""
from pathlib import Path
import argparse,ctypes,datetime,hashlib,json,os,subprocess,sys

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--label',default='green');ap.add_argument('--production-tus',action='store_true');ap.add_argument('--existing-suite',action='store_true');ap.add_argument('--diagnostic-tus',action='store_true');args=ap.parse_args()
    if not args.label.replace('-','').isalnum():raise ValueError('invalid label')
    repo=Path(__file__).resolve().parents[2];build=repo.parent.parent/'build/tofu-aer-policy/source-pose'/args.label;build.mkdir(parents=True,exist_ok=True)
    result=subprocess.run(['powershell.exe','-NoProfile','-Command',"$g=@(Get-Process -Name NMS,Cyberpunk2077 -ErrorAction SilentlyContinue); ConvertTo-Json -InputObject @($g|Select-Object ProcessName,Id) -Compress"],check=True,capture_output=True,text=True)
    games=json.loads(result.stdout or '[]')
    if games:raise RuntimeError('game active')
    kernel=ctypes.WinDLL('kernel32',use_last_error=True);kernel.CreateMutexW.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_wchar_p];kernel.CreateMutexW.restype=ctypes.c_void_p
    kernel.WaitForSingleObject.argtypes=[ctypes.c_void_p,ctypes.c_uint32];kernel.WaitForSingleObject.restype=ctypes.c_uint32
    kernel.ReleaseMutex.argtypes=[ctypes.c_void_p];kernel.CloseHandle.argtypes=[ctypes.c_void_p]
    mutex=kernel.CreateMutexW(None,False,'Local\\IRD.SingleBuildTestDeploy.v1')
    if not mutex:raise ctypes.WinError(ctypes.get_last_error())
    if kernel.WaitForSingleObject(mutex,0) not in [0,0x80]:kernel.CloseHandle(mutex);raise RuntimeError('build/test owner busy')
    try:
        tool=Path('<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc');vc=tool/'VC/Tools/MSVC/14.44.35207';sdk=tool/'Windows Kits/10';ver='10.0.26100.0'
        env=os.environ.copy();env['PATH']=str(vc/'bin/Hostx64/x64')+';'+str(sdk/f'bin/{ver}/x64')+';'+env['PATH']
        env['INCLUDE']=';'.join(map(str,[vc/'include']+[sdk/f'Include/{ver}/{x}' for x in ['ucrt','shared','um','winrt','cppwinrt']]))
        env['LIB']=';'.join(map(str,[vc/'lib/x64',sdk/f'Lib/{ver}/ucrt/x64',sdk/f'Lib/{ver}/um/x64']))
        source=Path(__file__).with_name('aer_source_pose.cpp');exe=build/'aer_source_pose.exe'
        cmd=[str(vc/'bin/Hostx64/x64/cl.exe'),'/nologo','/std:c++20','/EHsc','/O2','/W4','/WX','/permissive-','/fp:strict','/MT','/utf-8','/I'+str(repo/'include'),str(source),'/Fe:'+str(exe),'/Fo:'+str(build/'aer_source_pose.obj')]
        cc=subprocess.run(cmd,cwd=build,env=env,text=True,capture_output=True);(build/'compile.log').write_text(cc.stdout+cc.stderr,encoding='utf-8');test=None
        if cc.returncode==0:
            test=subprocess.run([str(exe)],cwd=build,text=True,capture_output=True,timeout=20);(build/'tests.log').write_text(test.stdout+test.stderr,encoding='utf-8')
        units=[]
        if (args.production_tus or args.diagnostic_tus) and test and test.returncode==0:
            # Reuse the configured game's exact includes/defines, but emit only
            # these .obj/.pdb files in the isolated evidence directory. No link.
            ninja=repo.parent.parent/'build/tofu-ru/build.ninja'
            lines=ninja.read_text(encoding='utf-8').splitlines()
            selected=['src/Natives/Registration.cpp','src/Framegen/SourceFrameSystem.cpp','src/Camera/PoseIdentity.cpp',
                      'src/Hooks/PatchCamera.cpp','src/Hooks/LocateCamera.cpp','src/Runtimes/OpenXRCapture.cpp','src/Runtimes/OpenXRManager.cpp']
            if args.diagnostic_tus:selected=['src/Camera/PoseIdentity.cpp','src/Runtimes/OpenXRManager.cpp','src/Hooks/PatchCamera.cpp','src/Hooks/LocateCamera.cpp','src/Hooks/CameraPoseTransport.cpp']
            for relative in selected:
                suffix=relative.replace('/','\\')+'.obj:'
                at=next((i for i,line in enumerate(lines) if line.startswith('build ') and suffix in line),None)
                if at is None:
                    at=next(i for i,line in enumerate(lines) if line.startswith('build ') and r'\src\Camera\PoseIdentity.cpp.obj:' in line)
                properties={}
                for line in lines[at+1:]:
                    if not line.strip():break
                    if ' = ' in line:
                        k,v=line.strip().split(' = ',1);properties[k]=v
                stem=Path(relative).stem
                command=[str(vc/'bin/Hostx64/x64/cl.exe'),'/nologo']+properties['DEFINES'].split()+properties['FLAGS'].split()+properties['INCLUDES'].split()+[
                    '/c',str(repo/relative),'/Fo:'+str(build/(stem+'.obj')),'/Fd:'+str(build/(stem+'.pdb'))]
                compiled=subprocess.run(command,cwd=build,env=env,text=True,capture_output=True,timeout=90)
                (build/(stem+'-compile.log')).write_text(compiled.stdout+compiled.stderr,encoding='utf-8')
                units.append({'source':relative,'exit':compiled.returncode,'command':command})
                print('TU '+relative+' exit='+str(compiled.returncode))
                if compiled.returncode:print((compiled.stdout+compiled.stderr)[-5000:]);break
        existing=[]
        if args.existing_suite and test and test.returncode==0 and all(u['exit']==0 for u in units):
            cpu=repo.parent.parent/'build/tofu-framegen-contract'
            for name,command in [('existing-build',['D:/Qt/Tools/CMake_64/bin/cmake.exe','--build',str(cpu),'--parallel','2']),
                                 ('existing-tests',['D:/Qt/Tools/CMake_64/bin/ctest.exe','--test-dir',str(cpu),'--output-on-failure'])]:
                done=subprocess.run(command,cwd=build,env=env,text=True,capture_output=True,timeout=180)
                (build/(name+'.log')).write_text(done.stdout+done.stderr,encoding='utf-8');existing.append({'name':name,'exit':done.returncode})
                print(name+' exit='+str(done.returncode))
                if done.returncode:print((done.stdout+done.stderr)[-5000:]);break
        files=[repo/'include/Framegen/MainEyeSourceLatch.hpp',repo/'include/Camera/PoseIdentity.hpp',repo/'src/Camera/PoseIdentity.cpp',
               repo/'include/Framegen/MainEyePoseSample.hpp',repo/'include/Framegen/MainEyePoseDiagnostics.hpp',repo/'include/Camera/MainEyeWriteDiagnostics.hpp',
               repo/'include/Camera/MainEyeCameraRoute.hpp',repo/'include/Camera/MainEyeScenePose.hpp',repo/'include/Camera/MainEyeTransportDiagnostics.hpp',repo/'src/Hooks/CameraPoseTransport.cpp',
               repo/'include/Framegen/SourceSceneDelivery.hpp',repo/'include/Framegen/SourceSceneEpoch.hpp',
               repo/'src/Framegen/SourceFrameSystem.cpp',repo/'src/Natives/Registration.cpp',repo/'src/Hooks/PatchCamera.cpp',repo/'src/Hooks/LocateCamera.cpp',
               repo/'include/Runtimes/OpenXRManager.hpp',repo/'src/Runtimes/OpenXRManager.cpp',repo/'src/Runtimes/OpenXRCapture.cpp',source,Path(__file__)]
        receipt={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'compile_exit':cc.returncode,'test_exit':None if test is None else test.returncode,'games':games,'command':cmd,'production_units':units,'existing_suite':existing,
            'files':[{'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest().upper()} for p in files]}
        (build/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
        print(cc.stdout+cc.stderr)
        if test:print(test.stdout+test.stderr)
        print('EVIDENCE '+str(build));return cc.returncode or (test.returncode if test else 1) or next((u['exit'] for u in units+existing if u['exit']),0)
    finally:kernel.ReleaseMutex(mutex);kernel.CloseHandle(mutex)
if __name__=='__main__':sys.exit(main())
