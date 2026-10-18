"""Compile only the production AER policy/clock headers and CPU contract cases."""
from pathlib import Path
import argparse,ctypes,datetime,hashlib,json,os,subprocess,sys

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--label',default='green');args=parser.parse_args()
    if not args.label.replace('-','').replace('_','').isalnum():raise ValueError('label must be alphanumeric')
    repo=Path(__file__).resolve().parents[2];project=repo.parent.parent
    build=project/'build/tofu-aer-policy'/args.label;build.mkdir(parents=True,exist_ok=True)
    gate=subprocess.run(['powershell.exe','-NoProfile','-Command',
        "$g=@(Get-Process -Name NMS,Cyberpunk2077 -ErrorAction SilentlyContinue); ConvertTo-Json -InputObject @($g|Select-Object ProcessName,Id) -Compress"],capture_output=True,text=True,check=True)
    games=json.loads(gate.stdout or '[]')
    if games:raise RuntimeError('CPU compilation deferred: game active '+gate.stdout)
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.CreateMutexW.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_wchar_p];kernel.CreateMutexW.restype=ctypes.c_void_p
    kernel.WaitForSingleObject.argtypes=[ctypes.c_void_p,ctypes.c_uint32];kernel.WaitForSingleObject.restype=ctypes.c_uint32
    kernel.ReleaseMutex.argtypes=[ctypes.c_void_p];kernel.CloseHandle.argtypes=[ctypes.c_void_p]
    mutex=kernel.CreateMutexW(None,False,'Local\\IRD.SingleBuildTestDeploy.v1')
    if not mutex:raise ctypes.WinError(ctypes.get_last_error())
    result=kernel.WaitForSingleObject(mutex,0)
    if result not in [0,0x80]:kernel.CloseHandle(mutex);raise RuntimeError('Build/test/deploy owner is busy')
    try:
        tool=Path('<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc');vc=tool/'VC/Tools/MSVC/14.44.35207';sdk=tool/'Windows Kits/10';ver='10.0.26100.0'
        env=os.environ.copy();env['PATH']=str(vc/'bin/Hostx64/x64')+';'+str(sdk/f'bin/{ver}/x64')+';'+env['PATH']
        env['INCLUDE']=';'.join(map(str,[vc/'include']+[sdk/f'Include/{ver}/{x}' for x in ['ucrt','shared','um','winrt','cppwinrt']]))
        env['LIB']=';'.join(map(str,[vc/'lib/x64',sdk/f'Lib/{ver}/ucrt/x64',sdk/f'Lib/{ver}/um/x64']))
        source=Path(__file__).with_name('aer_policy_clock.cpp');exe=build/'aer_policy_clock.exe'
        command=[str(vc/'bin/Hostx64/x64/cl.exe'),'/nologo','/std:c++20','/EHsc','/O2','/W4','/WX','/permissive-','/fp:strict','/MT','/utf-8',
                 '/I'+str(repo/'include'),str(source),'/Fe:'+str(exe),'/Fo:'+str(build/'aer_policy_clock.obj')]
        compilation=subprocess.run(command,cwd=build,env=env,capture_output=True,text=True)
        (build/'compile.log').write_text(compilation.stdout+compilation.stderr,encoding='utf-8')
        test=None
        if compilation.returncode==0:
            test=subprocess.run([str(exe)],cwd=build,env=env,capture_output=True,text=True,timeout=20)
            (build/'tests.log').write_text(test.stdout+test.stderr,encoding='utf-8')
        files=[repo/'include/Framegen/AerV2Policy.hpp',repo/'include/Framegen/SourceDisplayClock.hpp',source,Path(__file__)]
        receipt={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'scope':'CPU-only production-header compile and contract tests; no DLL/full build/game/GPU',
          'games':games,'source_head':subprocess.run(['git','-C',str(repo),'rev-parse','HEAD'],capture_output=True,text=True,check=True).stdout.strip(),
          'compile_exit':compilation.returncode,'test_exit':None if test is None else test.returncode,
          'command':command,'files':[{'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest().upper()} for p in files]}
        (build/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
        print(compilation.stdout+compilation.stderr)
        if test is not None:print(test.stdout+test.stderr)
        print('EVIDENCE '+str(build));return compilation.returncode or (test.returncode if test is not None else 1)
    finally:kernel.ReleaseMutex(mutex);kernel.CloseHandle(mutex)

if __name__=='__main__':sys.exit(main())
