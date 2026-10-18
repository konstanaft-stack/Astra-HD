from pathlib import Path
import argparse,ctypes,datetime,hashlib,json,os,subprocess,sys
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--label',required=True);ap.add_argument('--identity-tu',action='store_true');args=ap.parse_args()
    if not args.label.replace('-','').isalnum():raise ValueError('invalid label')
    repo=Path(__file__).resolve().parents[2];out=repo.parent.parent/'build/tofu-image-pose-submission'/args.label;out.mkdir(parents=True,exist_ok=True)
    g=subprocess.run(['powershell.exe','-NoProfile','-Command',"$g=@(Get-Process -Name NMS,Cyberpunk2077 -ErrorAction SilentlyContinue); ConvertTo-Json -InputObject @($g|Select-Object ProcessName,Id) -Compress"],check=True,text=True,capture_output=True)
    games=json.loads(g.stdout or '[]')
    if games:raise RuntimeError('Game active')
    k=ctypes.WinDLL('kernel32',use_last_error=True);k.CreateMutexW.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_wchar_p];k.CreateMutexW.restype=ctypes.c_void_p
    k.WaitForSingleObject.argtypes=[ctypes.c_void_p,ctypes.c_uint32];k.WaitForSingleObject.restype=ctypes.c_uint32;k.ReleaseMutex.argtypes=[ctypes.c_void_p];k.CloseHandle.argtypes=[ctypes.c_void_p]
    gate=k.CreateMutexW(None,False,'Local\\IRD.SingleBuildTestDeploy.v1')
    if not gate:raise ctypes.WinError(ctypes.get_last_error())
    if k.WaitForSingleObject(gate,0) not in (0,0x80):k.CloseHandle(gate);raise RuntimeError('Shared gate busy')
    try:
        vc=Path('<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/VC/Tools/MSVC/14.44.35207');sdk=vc.parents[3]/'Windows Kits/10';ver='10.0.26100.0'
        env=os.environ.copy();env['PATH']=str(vc/'bin/Hostx64/x64')+';'+env['PATH'];env['INCLUDE']=';'.join(map(str,[vc/'include']+[sdk/f'Include/{ver}/{x}' for x in ['ucrt','shared','um','winrt','cppwinrt']]))
        env['LIB']=';'.join(map(str,[vc/'lib/x64',sdk/f'Lib/{ver}/ucrt/x64',sdk/f'Lib/{ver}/um/x64']))
        source=Path(__file__).with_name('image_pose_submission.cpp');files=[repo/'include/Camera/ImagePoseLedger.hpp',repo/'include/Camera/ImagePoseIdentity.hpp',repo/'src/Camera/ImagePoseIdentity.cpp',source,Path(__file__)]
        def hashes():return [{'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest().upper()} for p in files]
        before=hashes()
        for p in files:(out/p.name).write_bytes(p.read_bytes())
        exe=out/'image_pose_submission.exe';cmd=[str(vc/'bin/Hostx64/x64/cl.exe'),'/nologo','/std:c++20','/EHsc','/O2','/W4','/WX','/permissive-','/MT','/utf-8','/I'+str(repo/'include'),str(source),'/Fe:'+str(exe),'/Fo:'+str(out/'fixture.obj')]
        cc=subprocess.run(cmd,cwd=out,env=env,text=True,capture_output=True);(out/'compile.log').write_text(cc.stdout+cc.stderr,encoding='utf-8');run=None;tu=None
        if not cc.returncode:
            run=subprocess.run([str(exe)],cwd=out,text=True,capture_output=True,timeout=20);(out/'run.log').write_text(run.stdout+run.stderr,encoding='utf-8')
        if args.identity_tu and run and not run.returncode:
            lines=(repo.parent.parent/'build/tofu-ru/build.ninja').read_text(encoding='utf-8').splitlines();at=next(i for i,line in enumerate(lines) if line.startswith('build ') and r'\src\Camera\ImagePoseIdentity.cpp.obj:' in line);fields={}
            for line in lines[at+1:]:
                if not line.strip():break
                if ' = ' in line:key,value=line.strip().split(' = ',1);fields[key]=value
            command=[str(vc/'bin/Hostx64/x64/cl.exe'),'/nologo']+fields['DEFINES'].split()+fields['FLAGS'].split()+fields['INCLUDES'].split()+['/c',str(files[2]),'/Fo:'+str(out/'ImagePoseIdentity.obj'),'/Fd:'+str(out/'ImagePoseIdentity.pdb')]
            tu=subprocess.run(command,cwd=out,env=env,text=True,capture_output=True,timeout=90);(out/'identity-compile.log').write_text(tu.stdout+tu.stderr,encoding='utf-8');print('PRODUCTION TU exit',tu.returncode)
        after=hashes();receipt={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'games':games,'compile_exit':cc.returncode,'test_exit':None if run is None else run.returncode,'identity_tu_exit':None if tu is None else tu.returncode,'before':before,'after':after,'source_changed':before!=after,'command':cmd,'gpu':False}
        (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8');print(cc.stdout+cc.stderr)
        if run:print(run.stdout+run.stderr)
        if tu and tu.returncode:print(tu.stdout+tu.stderr)
        print('EVIDENCE',out);return cc.returncode or (run.returncode if run else 1) or (tu.returncode if tu else 0) or int(before!=after)
    finally:k.ReleaseMutex(gate);k.CloseHandle(gate)
if __name__=='__main__':sys.exit(main())
