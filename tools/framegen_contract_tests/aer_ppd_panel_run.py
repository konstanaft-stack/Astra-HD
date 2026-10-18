"""PPD controller/geometry checks plus compile-only production panel."""
from pathlib import Path
import argparse, ctypes, datetime, hashlib, json, os, subprocess, sys

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--label',required=True);ap.add_argument('--production',action='store_true');ap.add_argument('--baseline',action='store_true');args=ap.parse_args()
    if not args.label.replace('-','').isalnum():raise ValueError('invalid label')
    repo=Path(__file__).resolve().parents[2];out=repo.parent.parent/'build/tofu-ppd-panel'/args.label;out.mkdir(parents=True,exist_ok=True)
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
        baseline=repo.parent.parent/'build/tofu-ppd-panel/red-baseline'
        test=(baseline/'aer_ppd_panel.cpp') if args.baseline else Path(__file__).with_name('aer_ppd_panel.cpp');exe=out/'ppd-panel.exe';cl=str(vc/'bin/Hostx64/x64/cl.exe')
        command=[cl,'/nologo','/std:c++20','/EHsc','/Od' if args.baseline else '/O2','/W4','/WX','/permissive-','/fp:strict','/DNOMINMAX','/utf-8']+(['/I'+str(baseline)] if args.baseline else [])+['/I'+str(repo/'include'),str(test),'/Fe:'+str(exe)]
        cc=subprocess.run(command,cwd=out,env=env,text=True,capture_output=True);(out/'compile.log').write_text(cc.stdout+cc.stderr,encoding='utf-8')
        done=None;production=None
        if cc.returncode==0:
            done=subprocess.run([str(exe)],cwd=out,text=True,capture_output=True,timeout=30);(out/'tests.log').write_text(done.stdout+done.stderr,encoding='utf-8');print(done.stdout+done.stderr)
        else:print(cc.stdout+cc.stderr)
        if args.production and done and done.returncode==0:
            lines=(repo.parent.parent/'build/tofu-ru/build.ninja').read_text(encoding='utf-8').splitlines()
            at=next(i for i,line in enumerate(lines) if line.startswith('build ') and r'\src\Overlay\OverlayPanels.cpp.obj:' in line)
            properties={}
            for line in lines[at+1:]:
                if not line.strip():break
                if ' = ' in line:k,v=line.strip().split(' = ',1);properties[k]=v
            cmd=[cl,'/nologo']+properties['DEFINES'].split()+properties['FLAGS'].split()+properties['INCLUDES'].split()+['/c',str(repo/'src/Overlay/PpdPanel.cpp'),'/Fo:'+str(out/'PpdPanel.obj'),'/Fd:'+str(out/'PpdPanel.pdb')]
            production=subprocess.run(cmd,cwd=out,env=env,text=True,capture_output=True,timeout=90);(out/'production-compile.log').write_text(production.stdout+production.stderr,encoding='utf-8');print('production exit='+str(production.returncode))
            if production.returncode:print(production.stdout+production.stderr)
        files=[baseline/'Overlay/PpdPanel.hpp' if args.baseline else repo/'include/Overlay/PpdPanel.hpp',repo/'include/Overlay/PpdCalibration.hpp',
            repo/'include/Overlay/PpdResolution.hpp',repo/'include/Overlay/PpdNativeApply.hpp',repo/'src/Overlay/PpdPanel.cpp',test,Path(__file__)]
        receipt={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'games':games,'compileExit':cc.returncode,'testExit':None if done is None else done.returncode,'productionExit':None if production is None else production.returncode,'command':command,'files':[{'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest().upper()} for p in files if p.exists()]}
        (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8');print('EVIDENCE '+str(out))
        return cc.returncode or (done.returncode if done else 1) or (production.returncode if production else 0)
    finally:kernel.ReleaseMutex(mutex);kernel.CloseHandle(mutex)
if __name__=='__main__':sys.exit(main())
