#include <bcrypt.h>
#include <winver.h>
#include <psapi.h>
#ifndef CPVR_IDENTITY_CLEAN_MISMATCH
#define CPVR_IDENTITY_CLEAN_MISMATCH() ((void)0)
#endif
static wchar_t driver_loader[MAX_PATH],driver_dir[MAX_PATH],snippet_path[MAX_PATH];
static char driver_sha[65],snippet_sha[65];
static char core_sha[65]="91D3742F0DF3F2DD85141FA39EA257F7B9243C3E6755CD7FEA894C721E7491AA";
static int cpvr_native_x2_only;
static const char* cpvr_profile_name="legacy-310.9.1";
static HMODULE cpvr_snippet_refs[2];
static unsigned cpvr_snippet_ref_count;
static const char* identity_stage="unspecified";
int cpvr_nv_configure(const wchar_t*driver,const char*driver_hash,const char*snippet_hash,int gate){
 if(nv_attached||!driver||!driver_hash||!snippet_hash)return 0;
 /* Atomic driver/core/snippet profiles, never independent version allowlists.
    A new profile still needs the offline ABI/GPU fixture before installation. */
 const int legacy_3109=!strcmp(snippet_hash,"FF6E90EB78B827927DFF5B4ECC6B1C870C2E9BCA29ED9F48C7D348CC9E170B82");
 const int baseline_3108=!strcmp(snippet_hash,"5D5CBF14D2727D47F93FD10BF77BD91708AE122482A6F86FD564971641EBD47B");
 const int old_driver=!strcmp(driver_hash,"3F8517407C8869B6CF28BD85D17B8FF0707EF380E86AF39F85A73E3401CC6FC4");
 const int current_stack=!strcmp(driver_hash,"B79D0ED1B677E9813476F91A447DA443D1897117BC4FF47F6D69FBDC618D6CEE")&&
    !strcmp(snippet_hash,"CADEE1AA6DB3907BABDC3285DA2AEBC4553D92B89C38BA3EAE8ABD8BBA1BEA72");
 if(!(old_driver&&(legacy_3109||baseline_3108))&&!current_stack)return 0;
 DWORD n=GetFullPathNameW(driver,MAX_PATH,driver_loader,NULL);if(!n||n>=MAX_PATH)return 0;
 wcscpy(driver_dir,driver_loader);wchar_t*slash=wcsrchr(driver_dir,L'\\');if(!slash)return 0;*slash=0;
 /* X2 on310.8 uses native interpolation. The old >X2 compatibility patch
    is qualified only for310.9 and must not silently migrate to this binary. */
 memcpy(driver_sha,driver_hash,65);memcpy(snippet_sha,snippet_hash,65);
 memcpy(core_sha,current_stack?"F039CD8A42E2D87CE867AEC2A9C2DA98A1AC45E5C9C63D4F044D75C5A2A7086E":
    "91D3742F0DF3F2DD85141FA39EA257F7B9243C3E6755CD7FEA894C721E7491AA",65);
 cpvr_native_x2_only=current_stack;
 cpvr_profile_name=current_stack?"617.42-310.9.16-native-X2":(legacy_3109?"legacy-310.9.1":"legacy-310.8");
 cpvr_enable_gate=gate!=0&&legacy_3109;return 1;
}
static void verify_file(const wchar_t*path,const char*expected){
 BCRYPT_ALG_HANDLE alg=NULL;BCRYPT_HASH_HANDLE hash=NULL;ULONG object_size=0,used=0;
 unsigned char digest[32],bytes[65536];
 if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,NULL,0)||
    BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,(PUCHAR)&object_size,sizeof(object_size),&used,0))FATAL("SHA provider",0);
 unsigned char*object=malloc(object_size);if(!object)FATAL("SHA allocation",0);
 if(BCryptCreateHash(alg,&hash,object,object_size,NULL,0,0))FATAL("SHA create",0);
 HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
 if(file==INVALID_HANDLE_VALUE)FATAL("SHA file",GetLastError());
 DWORD size=0;for(;;){if(!ReadFile(file,bytes,sizeof(bytes),&size,NULL))FATAL("SHA read",GetLastError());if(!size)break;
  if(BCryptHashData(hash,bytes,size,0))FATAL("SHA update",0);}
 if(BCryptFinishHash(hash,digest,32,0))FATAL("SHA finish",0);
 const BOOL file_closed=CloseHandle(file);
 const NTSTATUS hash_closed=BCryptDestroyHash(hash);
 const NTSTATUS provider_closed=BCryptCloseAlgorithmProvider(alg,0);free(object);
 if(!file_closed||hash_closed||provider_closed)FATAL("SHA cleanup",0);
 char hex[65];for(unsigned i=0;i<32;i++)sprintf(hex+i*2,"%02X",digest[i]);hex[64]=0;
 if(strcmp(hex,expected)){char detail[192];const wchar_t*leaf=wcsrchr(path,L'\\');leaf=leaf?leaf+1:path;
  snprintf(detail,sizeof(detail),"DLL SHA mismatch stage=%s expected=%.16s actual=%s file=%.40ls",identity_stage,expected,hex,leaf);
  CPVR_IDENTITY_CLEAN_MISMATCH();FATAL(detail,0);}
 printf("VERIFIED_NVIDIA path=%ls sha256=%s\n",path,hex);
}
static HMODULE verify_snippet(void){
 HMODULE module=GetModuleHandleW(L"nvngx_dlssg.dll");wchar_t actual[MAX_PATH],loaded[MAX_PATH];
 if(!module||!GetModuleFileNameW(module,loaded,MAX_PATH)||!GetFullPathNameW(loaded,MAX_PATH,actual,NULL))FATAL("loaded snippet missing",0);
 if(_wcsicmp(actual,snippet_path))FATAL("loaded snippet path mismatch",0);
 verify_file(actual,snippet_sha);return module;
}
static void verify_loaded_snippets(int require_loaded){
 /* CreateFeature exposes an opaque handle, not its implementing HMODULE.
    Validate every loaded copy, not an arbitrary basename match. The current
    author package intentionally contains the same pinned binary at two paths. */
 HMODULE modules[4096];DWORD needed=0;unsigned found=0;
 if(!K32EnumProcessModules(GetCurrentProcess(),modules,sizeof(modules),&needed)||needed>sizeof(modules))
  FATAL("incomplete snippet module enumeration",GetLastError());
 wchar_t alternate[MAX_PATH];wcscpy(alternate,snippet_path);
 wchar_t* slash=wcsrchr(alternate,L'\\');if(!slash)FATAL("snippet canonical directory",0);*slash=0;
 const size_t prefix=wcslen(alternate);
 const wchar_t* suffix=L"\\OptiScaler\\streamline\\nvngx_dlssg.dll";
 if(prefix+wcslen(suffix)>=MAX_PATH)FATAL("alternate snippet path size",0);
 wcscat(alternate,suffix);
 for(unsigned i=0;i<needed/sizeof(HMODULE);i++){
  wchar_t name[MAX_PATH],actual[MAX_PATH];
  DWORD length=K32GetModuleBaseNameW(GetCurrentProcess(),modules[i],name,MAX_PATH);
  if(!length||length>=MAX_PATH)FATAL("snippet module name enumeration",GetLastError());
  if(_wcsicmp(name,L"nvngx_dlssg.dll"))continue;
  length=GetModuleFileNameW(modules[i],actual,MAX_PATH);
  if(!length||length>=MAX_PATH)FATAL("loaded snippet canonical path",GetLastError());
  if(_wcsicmp(actual,snippet_path)&&(!cpvr_native_x2_only||_wcsicmp(actual,alternate)))
   FATAL("unqualified loaded snippet path",0);
  if(++found>2)FATAL("unexpected duplicate snippet modules",0);
  unsigned held=0;for(;held<cpvr_snippet_ref_count;held++)if(cpvr_snippet_refs[held]==modules[i])break;
  if(held==cpvr_snippet_ref_count){
   HMODULE retained=NULL;
   if(cpvr_snippet_ref_count>=2||!GetModuleHandleExW(0,actual,&retained)||retained!=modules[i])
    FATAL("cannot retain verified snippet module",GetLastError());
   cpvr_snippet_refs[cpvr_snippet_ref_count++]=retained;
  }
  identity_stage="loaded-snippet-module";verify_file(actual,snippet_sha);
 }
 if(require_loaded&&!found)FATAL("no qualified loaded snippet after CreateFeature",0);
 printf("VERIFIED_SNIPPET_SET profile=%s count=%u require_loaded=%d; no feature-module attribution\n",cpvr_profile_name,found,require_loaded);
}
static void release_snippet_refs(void){
 while(cpvr_snippet_ref_count)FreeLibrary(cpvr_snippet_refs[--cpvr_snippet_ref_count]);
 memset(cpvr_snippet_refs,0,sizeof(cpvr_snippet_refs));
}
