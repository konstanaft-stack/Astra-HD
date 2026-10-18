"""Extract actual camera metadata functions, compile CPU fixture under shared gate."""
from pathlib import Path
import argparse, ctypes, datetime, hashlib, json, os, subprocess, sys

def function(text, name):
    begin=text.index(name);brace=text.index('{',begin);level=1;end=brace+1
    while level:
        level+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[begin:end]

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--label',required=True);ap.add_argument('--production-tus',action='store_true');args=ap.parse_args()
    if not args.label.replace('-','').isalnum():raise ValueError('invalid label')
    repo=Path(__file__).resolve().parents[2];out=repo.parent.parent/'build/tofu-applied-eye-reference'/args.label
    out.mkdir(parents=True,exist_ok=True)
    checked=subprocess.run(['powershell.exe','-NoProfile','-Command',"$g=@(Get-Process -Name NMS,Cyberpunk2077 -ErrorAction SilentlyContinue); ConvertTo-Json -InputObject @($g|Select-Object ProcessName,Id) -Compress"],check=True,capture_output=True,text=True)
    games=json.loads(checked.stdout or '[]')
    if games:raise RuntimeError('game active: '+str(games))
    k=ctypes.WinDLL('kernel32',use_last_error=True);k.CreateMutexW.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_wchar_p];k.CreateMutexW.restype=ctypes.c_void_p
    k.WaitForSingleObject.argtypes=[ctypes.c_void_p,ctypes.c_uint32];k.WaitForSingleObject.restype=ctypes.c_uint32
    k.ReleaseMutex.argtypes=[ctypes.c_void_p];k.CloseHandle.argtypes=[ctypes.c_void_p]
    mutex=k.CreateMutexW(None,False,'Local\\IRD.SingleBuildTestDeploy.v1')
    if not mutex:raise ctypes.WinError(ctypes.get_last_error())
    if k.WaitForSingleObject(mutex,0) not in (0,0x80):k.CloseHandle(mutex);raise RuntimeError('build/test gate busy')
    try:
        source=repo/'src/Camera/PoseIdentity.cpp';header=repo/'include/Camera/PoseIdentity.hpp';fixture=Path(__file__).with_name('applied_eye_reference.cpp')
        files=[source,header,repo/'include/Camera/PoseAddressLedger.hpp',repo/'include/Camera/AppliedEyeReference.hpp',
            repo/'include/Framegen/MainEyeSourceLatch.hpp',repo/'src/Hooks/PatchCamera.cpp',repo/'src/Hooks/LocateCamera.cpp',fixture,Path(__file__)]
        def manifest():return [{'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest().upper()} for p in files if p.exists()]
        before=manifest();text=source.read_text(encoding='utf-8');decls=header.read_text(encoding='utf-8')
        pose=function(decls,'struct PoseIdentity')+';'
        legacy='appliedEyeReference' not in pose
        if legacy:pose=pose[:-2]+'    AppliedEyeReference appliedEyeReference{}; // RED-only observation slot\n};'
        signatures=['void InvalidatePoseAddress','PoseReceipt CapturePoseAddress','bool TransferPoseAddress','void PublishComponentPose','void PublishCameraSetupPose','void PublishSerializedPose','bool ReadCameraPoseIdentity','bool PublishBlendedPose']
        declarations=[decls[decls.index(n):decls.index(';',decls.index(n))+1] for n in signatures]
        (out/'applied_eye_types.inc').write_text(pose+'\nusing PoseReceipt=PoseAddressLedger<PoseIdentity>::Receipt;\nstruct WeightedPoseReceipt {PoseReceipt source;float weight{};};\n'+'\n'.join(declarations)+'\n',encoding='utf-8')
        names=['bool Fingerprint']+signatures
        (out/'applied_eye_functions.inc').write_text('\n\n'.join(function(text,n) for n in names)+'\n',encoding='utf-8')
        patch=(repo/'src/Hooks/PatchCamera.cpp').read_text(encoding='utf-8')
        scene=(repo/'src/Hooks/LocateCamera.cpp').read_text(encoding='utf-8')
        (out/'applied_eye_patch_call.inc').write_text(function(patch,'if(positionWritten && orientationWritten && translationLabelKnown && writtenPoseId &&')+'\n',encoding='utf-8')
        (out/'applied_eye_scene_call.inc').write_text(function(scene,'if(aerSceneSource && cvr::camera::SceneSourceWriteComplete')+'\n',encoding='utf-8')
        vc=Path('<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/VC/Tools/MSVC/14.44.35207');sdk=vc.parents[3]/'Windows Kits/10';ver='10.0.26100.0'
        env=os.environ.copy();env['PATH']=str(vc/'bin/Hostx64/x64')+';'+str(sdk/f'bin/{ver}/x64')+';'+env['PATH']
        env['INCLUDE']=';'.join(map(str,[vc/'include']+[sdk/f'Include/{ver}/{s}' for s in ['ucrt','shared','um','winrt','cppwinrt']]))
        env['LIB']=';'.join(map(str,[vc/'lib/x64',sdk/f'Lib/{ver}/ucrt/x64',sdk/f'Lib/{ver}/um/x64']))
        exe=out/'applied_eye_reference.exe';cmd=[str(vc/'bin/Hostx64/x64/cl.exe'),'/nologo','/std:c++20','/EHsc','/O2','/W4','/WX','/permissive-','/fp:strict','/MT','/utf-8','/I'+str(repo/'include'),'/I'+str(out),str(fixture),'/Fe:'+str(exe),'/Fo:'+str(out/'fixture.obj')]
        cc=subprocess.run(cmd,cwd=out,env=env,capture_output=True,text=True);(out/'compile.log').write_text(cc.stdout+cc.stderr,encoding='utf-8')
        test=None;units=[]
        if cc.returncode==0:
            test=subprocess.run([str(exe)],cwd=out,capture_output=True,text=True,timeout=20);(out/'tests.log').write_text(test.stdout+test.stderr,encoding='utf-8')
        if args.production_tus and test and test.returncode==0:
            lines=(repo.parent.parent/'build/tofu-ru/build.ninja').read_text(encoding='utf-8').splitlines()
            for path in ['src/Camera/PoseIdentity.cpp','src/Hooks/PatchCamera.cpp','src/Hooks/LocateCamera.cpp']:
                suffix=path.replace('/','\\')+'.obj:';at=next(i for i,line in enumerate(lines) if line.startswith('build ') and suffix in line)
                fields={}
                for line in lines[at+1:]:
                    if not line.strip():break
                    if ' = ' in line:key,value=line.strip().split(' = ',1);fields[key]=value
                stem=Path(path).stem;command=[str(vc/'bin/Hostx64/x64/cl.exe'),'/nologo']+fields['DEFINES'].split()+fields['FLAGS'].split()+fields['INCLUDES'].split()+['/c',str(repo/path),'/Fo:'+str(out/(stem+'.obj')),'/Fd:'+str(out/(stem+'.pdb'))]
                compiled=subprocess.run(command,cwd=out,env=env,capture_output=True,text=True,timeout=120);(out/(stem+'-compile.log')).write_text(compiled.stdout+compiled.stderr,encoding='utf-8')
                units.append({'source':path,'exit':compiled.returncode,'command':command});print('TU',path,compiled.returncode)
                if compiled.returncode:print((compiled.stdout+compiled.stderr)[-4500:]);break
        after=manifest();receipt={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'games':games,'legacy_observation_slot':legacy,'compile_exit':cc.returncode,'test_exit':None if test is None else test.returncode,'command':cmd,'production_units':units,'before':before,'after':after,'source_changed':before!=after}
        (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
        print(cc.stdout+cc.stderr)
        if test:print(test.stdout+test.stderr)
        print('EVIDENCE',out)
        return cc.returncode or (test.returncode if test else 1) or next((u['exit'] for u in units if u['exit']),0) or int(before!=after)
    finally:k.ReleaseMutex(mutex);k.CloseHandle(mutex)
if __name__=='__main__':sys.exit(main())
