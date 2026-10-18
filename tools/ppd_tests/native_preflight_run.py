"""Execute only reviewed read-only CP2077 predicates from the pinned on-disk PE."""
from pathlib import Path
import argparse, ctypes, datetime, hashlib, json, os, struct, subprocess

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--audit',required=True)
    parser.add_argument('--verify-float-regression',action='store_true')
    parser.add_argument('--exe',default=r'C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077\bin\x64\Cyberpunk2077.exe')
    args=parser.parse_args()
    repo=Path(__file__).resolve().parents[2];out=Path(args.audit).resolve();out.mkdir(parents=True,exist_ok=True)
    checked=subprocess.run(['powershell.exe','-NoProfile','-Command',"ConvertTo-Json -InputObject @(Get-Process Cyberpunk2077,NMS,Vortex -ErrorAction SilentlyContinue | Select-Object Id,ProcessName) -Compress"],capture_output=True,text=True,check=True)
    if json.loads(checked.stdout or '[]'):raise RuntimeError('Game or mod manager active')
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.CreateMutexW.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_wchar_p];kernel.CreateMutexW.restype=ctypes.c_void_p
    kernel.WaitForSingleObject.argtypes=[ctypes.c_void_p,ctypes.c_uint32];kernel.WaitForSingleObject.restype=ctypes.c_uint32
    kernel.ReleaseMutex.argtypes=[ctypes.c_void_p];kernel.CloseHandle.argtypes=[ctypes.c_void_p]
    mutex=kernel.CreateMutexW(None,False,'Local\\IRD.SingleBuildTestDeploy.v1')
    if not mutex:raise ctypes.WinError(ctypes.get_last_error())
    if kernel.WaitForSingleObject(mutex,0) not in [0,0x80]:kernel.CloseHandle(mutex);raise RuntimeError('Build/test owner busy')
    try:
        data=Path(args.exe).read_bytes();digest=hashlib.sha256(data).hexdigest().upper()
        if digest!='A7DE82945C03E041FC7339FCF9066224D98DB2F5D80FEA50F7947BB350A60991':raise RuntimeError('Unreviewed game image; do not execute its predicates')
        pe=struct.unpack_from('<I',data,0x3c)[0]
        if data[pe:pe+4]!=b'PE\0\0' or struct.unpack_from('<H',data,pe+4)[0]!=0x8664:raise RuntimeError('Not pinned x64 PE')
        count=struct.unpack_from('<H',data,pe+6)[0];optional=struct.unpack_from('<H',data,pe+20)[0]
        sections=[]
        for i in range(count):
            offset=pe+24+optional+40*i
            virtual_size,rva,raw_size,raw=struct.unpack_from('<IIII',data,offset+8)
            sections.append((rva,raw_size,raw))
        ranges=[(0x21c3e4,0x21c5ab),(0x1d56d90,0x1d56ea4),(0x1d56b00,0x1d56b25)]
        header=['#pragma once','#include <cstdint>','struct CloneRange { uint32_t rva, size; const wchar_t* file; };','inline constexpr CloneRange cloneRanges[]{']
        blocks=[]
        for begin,end in ranges:
            matches=[(rva,size,raw) for rva,size,raw in sections if rva<=begin and end<=rva+size]
            if len(matches)!=1:raise RuntimeError('Code range is not fully backed by one PE section')
            rva,size,raw=matches[0];blob=data[raw+begin-rva:raw+end-rva]
            if len(blob)!=end-begin:raise RuntimeError('Short PE code range')
            name=f'native-{begin:X}.bin';(out/name).write_bytes(blob)
            header.append(f'{{0x{begin:X},0x{end-begin:X},L"{name}"}},')
            blocks.append({'rva':hex(begin),'end':hex(end),'sha256':hashlib.sha256(blob).hexdigest().upper(),'path':str(out/name)})
        header.append('};');(out/'native-clone-ranges.hpp').write_text('\n'.join(header)+'\n',encoding='utf-8')
        vc=Path('<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/VC/Tools/MSVC/14.44.35207')
        sdk=Path('<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/Windows Kits/10');version='10.0.26100.0'
        env=os.environ.copy();env['PATH']=str(vc/'bin/Hostx64/x64')+';'+env['PATH']
        env['INCLUDE']=';'.join(map(str,[vc/'include']+[sdk/f'Include/{version}/{x}' for x in ['ucrt','shared','um']]))
        env['LIB']=';'.join(map(str,[vc/'lib/x64',sdk/f'Lib/{version}/ucrt/x64',sdk/f'Lib/{version}/um/x64']))
        source=Path(__file__).with_name('native_preflight_test.cpp');exe=out/'native-preflight.exe'
        command=[str(vc/'bin/Hostx64/x64/cl.exe'),'/nologo','/std:c++20','/EHsc','/W4','/fp:precise','/O2','/I'+str(repo/'include'),'/I'+str(out),str(source),'/Fo:'+str(out/'native-preflight.obj'),'/Fe:'+str(exe)]
        built=subprocess.run(command,env=env,cwd=out,capture_output=True,text=True,timeout=60)
        (out/'build.log').write_text(built.stdout+built.stderr,encoding='utf-8')
        result=None
        if built.returncode==0:
            result=subprocess.run([str(exe),str(out)],capture_output=True,text=True,timeout=30)
            (out/'test.log').write_text(result.stdout+result.stderr,encoding='utf-8')
            print(result.stdout+result.stderr)
        else:print(built.stdout+built.stderr)
        mutation={'status':'NOT_RUN'}
        if args.verify_float_regression and result and result.returncode==0:
            mutant=out/'mutant-include'/'Hooks';mutant.mkdir(parents=True,exist_ok=True)
            header_path=repo/'include/Hooks/NativeSettingsPreflight.hpp';text=header_path.read_text(encoding='utf-8')
            guard='if((left&0x7fffffffu)>0x7f800000u || (right&0x7fffffffu)>0x7f800000u)return false;'
            if text.count(guard)!=1:raise RuntimeError('Native unordered comparison mutation target not unique')
            (mutant/header_path.name).write_text(text.replace(guard,'// Audit-only regression: ordinary C++ != treats unordered as changed.'),encoding='utf-8')
            mutant_command=command.copy();mutant_command.insert(1,'/I'+str(mutant.parent))
            mutant_command[-2]='/Fo:'+str(out/'mutant.obj');mutant_command[-1]='/Fe:'+str(out/'mutant.exe')
            cc=subprocess.run(mutant_command,env=env,cwd=out,capture_output=True,text=True,timeout=60)
            (out/'mutant-build.log').write_text(cc.stdout+cc.stderr,encoding='utf-8')
            if cc.returncode:raise RuntimeError('Float mutation did not compile')
            failed=subprocess.run([str(out/'mutant.exe'),str(out)],capture_output=True,text=True,timeout=30)
            (out/'mutant-test.log').write_text(failed.stdout+failed.stderr,encoding='utf-8')
            if failed.returncode!=1 or 'production preflight differs from native helper' not in failed.stderr:
                raise RuntimeError('Fixture failed to detect native unordered comparison regression')
            mutation={'status':'EXPECTED_FAILURE_DETECTED','exit':failed.returncode}
            print('Native float regression: EXPECTED_FAILURE_DETECTED')
        receipt={'time':datetime.datetime.now(datetime.timezone.utc).isoformat(),'game_sha256':digest,'blocks':blocks,
            'compile_exit':built.returncode,'test_exit':result.returncode if result else None,
            'float_mutation':mutation,
            'source_files':[{'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest().upper()} for p in
                [repo/'include/Hooks/NativeSettingsPreflight.hpp',source,Path(__file__)]],
            'scope':'Native helper execution plus reviewed aggregate decision; not original full apply pass, game run or HMD acceptance'}
        (out/'RESULT.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
        return built.returncode or (result.returncode if result else 1)
    finally:kernel.ReleaseMutex(mutex);kernel.CloseHandle(mutex)

if __name__=='__main__':raise SystemExit(main())
