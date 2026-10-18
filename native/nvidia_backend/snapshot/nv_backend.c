/* Real P11 pair adapter. Loader/queue/evaluator copied from proven ngx-mixed-count; no synthetic scene. */
#define INITGUID
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <setjmp.h>
#include "nv_backend.h"
static jmp_buf nv_jump;
static fg_nv_status nv_status;
static DWORD nv_thread;
static wchar_t nv_directory[MAX_PATH];
static int nv_attached;
static int nv_failed; static int nv_unsafe_gpu; static int cpvr_enable_gate; static int cpvr_initialized_ngx; static int cpvr_gate_applied;
/* Positive abort eligibility exists only inside shared snippet preflight.
   Ever-executed is monotonic: completed work is not an unstarted attachment. */
static int nv_shared_snippet_preflight;
static int nv_clean_snippet_identity_reject;
static int nv_ever_executed;
static _Noreturn void nv_fail(const char *message,unsigned code){
 nv_failed=1;
 nv_status.code=-1;nv_status.ngx_result=code;
 snprintf(nv_status.message,sizeof(nv_status.message),"%s",message);
 longjmp(nv_jump,1);
}


#include "nvsdk_ngx.h"
#include "nvsdk_ngx_defs_dlssg.h"
#include "nvsdk_ngx_params_dlssg_c.h"

/* ---- runtime loading of the driver's nvngx.dll (no import lib shipped) ---- */
#define FATAL(msg,hr) nv_fail((msg),(unsigned)(hr))
static NVSDK_NGX_Result (NVSDK_CONV *glm_init_pid6)(const char *, NVSDK_NGX_EngineType, const char *, const wchar_t *, ID3D12Device *, const NVSDK_NGX_FeatureCommonInfo *);
static NVSDK_NGX_Result (NVSDK_CONV *glm_init_pid7)(const char *, NVSDK_NGX_EngineType, const char *, const wchar_t *, ID3D12Device *, unsigned long long, const NVSDK_NGX_FeatureCommonInfo *);
static NVSDK_NGX_Result (NVSDK_CONV *glm_init_ext)(const NVSDK_NGX_FeatureCommonInfo *, unsigned, const wchar_t *, ID3D12Device *);
static NVSDK_NGX_Result (NVSDK_CONV *glm_get_caps)(NVSDK_NGX_Parameter **);
static NVSDK_NGX_Result (NVSDK_CONV *glm_alloc_params)(NVSDK_NGX_Parameter **);
static NVSDK_NGX_Result (NVSDK_CONV *glm_create)(ID3D12GraphicsCommandList *, NVSDK_NGX_Feature, NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
static NVSDK_NGX_Result (NVSDK_CONV *glm_eval_c)(ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *, const NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback_C);

static NVSDK_NGX_Result (NVSDK_CONV *glm_eval_c5)(ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *, const NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback_C, void *);
static NVSDK_NGX_Result (NVSDK_CONV *glm_eval_c5b)(ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *, const NVSDK_NGX_Parameter *, void *, PFN_NVSDK_NGX_ProgressCallback_C);
static NVSDK_NGX_Result (NVSDK_CONV *glm_release)(NVSDK_NGX_Handle *);
static NVSDK_NGX_Result (NVSDK_CONV *glm_destroy_params)(NVSDK_NGX_Parameter *);
static NVSDK_NGX_Result (NVSDK_CONV *glm_shutdown1)(ID3D12Device *, unsigned *); /* pinned driver export ABI, not SDK wrapper */

#define NVSDK_NGX_D3D12_GetCapabilityParameters glm_get_caps
#define NVSDK_NGX_D3D12_AllocateParameters glm_alloc_params
#define NVSDK_NGX_D3D12_CreateFeature glm_create
#define NVSDK_NGX_D3D12_EvaluateFeature_C glm_eval_c
#define NVSDK_NGX_D3D12_ReleaseFeature glm_release
#define NVSDK_NGX_D3D12_DestroyParameters glm_destroy_params
#define NVSDK_NGX_D3D12_Shutdown1 glm_shutdown1

#include <stdlib.h>
#include "nvsdk_ngx_params_c_impl.h"
#include "nvsdk_ngx_helpers_dlssg_d3d.h"

#define CPVR_IDENTITY_CLEAN_MISMATCH() do { if(nv_shared_snippet_preflight)nv_clean_snippet_identity_reject=1; } while(0)
#include "native_identity.h"

#include <tlhelp32.h>
#include <stddef.h>

static HMODULE ngx_dll;
static void load_ngx(int shared_core) {
    identity_stage="wrapper-file";
    verify_file(driver_loader,driver_sha);
    wchar_t actual[MAX_PATH];
    if(shared_core){
        wchar_t core_path[MAX_PATH];
        if(swprintf(core_path,MAX_PATH,L"%ls\\_nvngx.dll",driver_dir)<0)FATAL("core path size",0);
        identity_stage="shared-core-file";verify_file(core_path,core_sha);
        // Reuse the host's initialized core. A new LoadLibrary(nvngx.dll) may
        // return an SDK interceptor; a second Init would change host ownership.
        if(!GetModuleHandleExW(0,core_path,&ngx_dll))FATAL("verified host NGX core not loaded",GetLastError());
        if(!GetModuleFileNameW(ngx_dll,actual,MAX_PATH)||_wcsicmp(actual,core_path))FATAL("shared core path mismatch",0);
        identity_stage="shared-core-module";verify_file(actual,core_sha);
    }else{
        ngx_dll=LoadLibraryExW(driver_loader,NULL,LOAD_WITH_ALTERED_SEARCH_PATH);
        if(!ngx_dll) FATAL("pinned driver loader",GetLastError());
        if(!GetModuleFileNameW(ngx_dll,actual,MAX_PATH)) FATAL("loader path",0);
        identity_stage="wrapper-returned-module";verify_file(actual,driver_sha);
    }
    struct { const char *name; void **pp; const char *alt; } syms[] = {
        { "NVSDK_NGX_D3D12_Init_ProjectID", (void **)&glm_init_pid6, NULL },
        { "NVSDK_NGX_D3D12_Init_ProjectID", (void **)&glm_init_pid7, NULL },
        { "NVSDK_NGX_D3D12_Init_Ext", (void **)&glm_init_ext, NULL },
        { "NVSDK_NGX_D3D12_GetCapabilityParameters", (void **)&glm_get_caps, NULL },
        { "NVSDK_NGX_D3D12_AllocateParameters", (void **)&glm_alloc_params, NULL },
        { "NVSDK_NGX_D3D12_CreateFeature", (void **)&glm_create, NULL },
        { "NVSDK_NGX_D3D12_EvaluateFeature", (void **)&glm_eval_c, "NVSDK_NGX_D3D12_EvaluateFeature_C" },
        { "NVSDK_NGX_D3D12_EvaluateFeature", (void **)&glm_eval_c5, NULL },
        { "NVSDK_NGX_D3D12_EvaluateFeature", (void **)&glm_eval_c5b, NULL },
        { "NVSDK_NGX_D3D12_ReleaseFeature", (void **)&glm_release, NULL },
        { "NVSDK_NGX_D3D12_DestroyParameters", (void **)&glm_destroy_params, NULL },
        { "NVSDK_NGX_D3D12_Shutdown1", (void **)&glm_shutdown1, NULL },
    };
    for (unsigned i = 0; i < sizeof(syms) / sizeof(syms[0]); i++) {
        void *p = (void *)GetProcAddress(ngx_dll, syms[i].name);
        if (!p && syms[i].alt) p = (void *)GetProcAddress(ngx_dll, syms[i].alt);
        if (!p) { fprintf(stderr, "FATAL missing export %s\n", syms[i].name); FATAL("missing NVIDIA export",4); }
        *syms[i].pp = p;
    }
}

#define FATAL(msg,hr) nv_fail((msg),(unsigned)(hr))
#define NGXCHK(res,msg) do{NVSDK_NGX_Result r_=(res);if(r_!=NVSDK_NGX_Result_Success)nv_fail((msg),(unsigned)r_);}while(0)

static UINT W, H, RW, RH;
static const DXGI_FORMAT FMT_COLOR = DXGI_FORMAT_R8G8B8A8_UNORM; /* isolated SDR diagnostic */
static const DXGI_FORMAT FMT_MV = DXGI_FORMAT_R16G16_FLOAT;
static const DXGI_FORMAT FMT_DEPTH = DXGI_FORMAT_R32_FLOAT;

static ID3D12Device *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *cmd;
static ID3D12Fence *fence;
static HANDLE fev;
static UINT64 fval;

static NVSDK_NGX_Parameter *caps_params;
static NVSDK_NGX_Parameter *eval_params;
static NVSDK_NGX_Handle *fg_feature;

typedef struct { ID3D12Resource *res; UINT w, h; DXGI_FORMAT fmt; D3D12_RESOURCE_STATES state; } Tex;
static Tex t_back, t_mv, t_depth, t_out;
static ID3D12Resource *t_disable_interp;

static void gfx_begin(void) {
    if (ID3D12CommandAllocator_Reset(alloc) != S_OK) FATAL("allocator reset", 0);
    if (ID3D12GraphicsCommandList_Reset(cmd, alloc, NULL) != S_OK) FATAL("cmdlist reset", 0);
}
static UINT64 gfx_enqueue_closed(void) {
    ID3D12CommandList *lists[1]={0};

    if(ID3D12GraphicsCommandList_QueryInterface(cmd,&IID_ID3D12CommandList,(void**)&lists[0])!=S_OK) FATAL("QI submitted list",0);
    nv_unsafe_gpu=1; nv_ever_executed=1; ID3D12CommandQueue_ExecuteCommandLists(queue,1,lists);
    ID3D12CommandList_Release(lists[0]);
    if(fval>=UINT64_MAX-1) FATAL("fence overflow",0);
    fval++;
    if(ID3D12CommandQueue_Signal(queue,fence,fval)!=S_OK) FATAL("Signal",0);
    return fval;
}
static void gfx_wait_for(UINT64 value) {
    if(!value||value>fval) FATAL("invalid scheduled fence wait",0);
    UINT64 done=ID3D12Fence_GetCompletedValue(fence);
    if(done==UINT64_MAX) FATAL("device removed sentinel",0);
    if(done<value) {
        if(!ResetEvent(fev)) FATAL("ResetEvent",GetLastError());
        if(ID3D12Fence_SetEventOnCompletion(fence,value,fev)!=S_OK) FATAL("SetEvent",0);
        DWORD wait=WaitForSingleObject(fev,5000);
        if(wait!=WAIT_OBJECT_0) FATAL("bounded fence wait; retain GPU objects",wait);
    }
    done=ID3D12Fence_GetCompletedValue(fence);
    if(done==UINT64_MAX || done<value) FATAL("unproven completion; retain GPU objects",0);
    if(done>=fval)nv_unsafe_gpu=0;
}
static void gfx_submit(void) {
    if(ID3D12GraphicsCommandList_Close(cmd)!=S_OK) FATAL("Close",0);
    gfx_wait_for(gfx_enqueue_closed());
}
static void transition(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b; memset(&b, 0, sizeof(b));
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(cmd, 1, &b);
}

static void set_state(Tex *t,D3D12_RESOURCE_STATES state) {
    if(t->state!=state) { transition(t->res,t->state,state); t->state=state; }
}
static Tex make_tex(UINT width, UINT height, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags) {
    Tex t; t.w = width; t.h = height; t.fmt = fmt; t.state=D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_HEAP_PROPERTIES hp; memset(&hp, 0, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d; memset(&d, 0, sizeof(d));
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = width; d.Height = height;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = fmt; d.SampleDesc.Count = 1;
    d.Flags = flags;
    if (ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&t.res) != S_OK)
        FATAL("CreateCommittedResource(tex)", 0);
    return t;
}
static ID3D12Resource *make_buf(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp; memset(&hp, 0, sizeof(hp)); hp.Type = type;
    D3D12_RESOURCE_DESC d; memset(&d, 0, sizeof(d));
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *r;
    if (ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, state,
            NULL, &IID_ID3D12Resource, (void **)&r) != S_OK) FATAL("CreateCommittedResource(buf)", 0);
    return r;
}
static void footprints(Tex *t, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *fp, UINT64 *total) {
    D3D12_RESOURCE_DESC desc=ID3D12Resource_GetDesc(t->res);UINT rows=0;UINT64 row_bytes=0;
    ID3D12Device_GetCopyableFootprints(dev,&desc,0,1,0,fp,&rows,&row_bytes,total);
    unsigned bpp=t->fmt==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;
    if(rows!=t->h || row_bytes!=(UINT64)t->w*bpp || fp->Footprint.RowPitch<row_bytes) FATAL("copyable footprints",0);
}
static void upload(Tex *t, const void *data, size_t src_row_bytes) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT64 total;
    footprints(t, &fp, &total);
    ID3D12Resource *stg = make_buf(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mappedv = NULL;
    if (ID3D12Resource_Map(stg, 0, NULL, &mappedv) != S_OK) FATAL("upload map", 0);
    for (UINT y = 0; y < t->h; y++)
        memcpy((unsigned char *)mappedv + fp.Offset + (size_t)y * fp.Footprint.RowPitch,
               (const unsigned char *)data + (size_t)y * src_row_bytes, src_row_bytes);
    ID3D12Resource_Unmap(stg, 0, NULL);
    gfx_begin();
    set_state(t,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst, src;
    memset(&dst, 0, sizeof(dst)); dst.pResource = t->res; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    memset(&src, 0, sizeof(src)); src.pResource = stg; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
    ID3D12GraphicsCommandList_CopyTextureRegion(cmd, &dst, 0, 0, 0, &src, NULL);
    gfx_submit();
    ID3D12Resource_Release(stg);
}
static void readback(Tex *t, unsigned char *out4, size_t dst_row_bytes, DXGI_FORMAT expected_fmt) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT64 total;
    footprints(t, &fp, &total);
    ID3D12Resource *stg = make_buf(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    gfx_begin();
    set_state(t,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst, src;
    memset(&dst, 0, sizeof(dst)); dst.pResource = stg; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    memset(&src, 0, sizeof(src)); src.pResource = t->res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    ID3D12GraphicsCommandList_CopyTextureRegion(cmd, &dst, 0, 0, 0, &src, NULL);
    set_state(t,D3D12_RESOURCE_STATE_COPY_DEST);
    gfx_submit();
    void *mappedv = NULL; D3D12_RANGE rg = { 0, (SIZE_T)total };
    if (ID3D12Resource_Map(stg, 0, &rg, &mappedv) != S_OK) FATAL("readback map", 0);
    size_t rb = fp.Footprint.RowPitch;
    for (UINT y = 0; y < t->h; y++)
        memcpy(out4 + (size_t)y * dst_row_bytes, (unsigned char *)mappedv + fp.Offset + (size_t)y * rb, dst_row_bytes);
    ID3D12Resource_Unmap(stg, 0, NULL);
    ID3D12Resource_Release(stg);
    (void)expected_fmt;
}

static void init_d3d12(void) {
    D3D12_COMMAND_QUEUE_DESC qd; memset(&qd, 0, sizeof(qd)); qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue) != S_OK) FATAL("CreateCommandQueue", 0);
    if (ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc) != S_OK) FATAL("CreateCommandAllocator", 0);
    if (ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cmd) != S_OK) FATAL("CreateCommandList", 0);
    if(ID3D12GraphicsCommandList_Close(cmd)!=S_OK) FATAL("initial Close",0);
    if (ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence) != S_OK) FATAL("CreateFence", 0);
    fev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if(!fev) FATAL("CreateEvent",GetLastError());
}

static void NVSDK_CONV ngx_log(const char *msg, NVSDK_NGX_Logging_Level lvl, NVSDK_NGX_Feature f) {
    (void)lvl; (void)f;
    if (msg) fprintf(stderr, "[NGX] %s\n", msg);
}

static void init_ngx(int with_loginfo) {
    NVSDK_NGX_FeatureCommonInfo ci; memset(&ci, 0, sizeof(ci));
    static wchar_t folder[MAX_PATH];
    if(swprintf(snippet_path,MAX_PATH,L"%ls\\nvngx_dlssg.dll",nv_directory)<0)FATAL("snippet directory",0);
    verify_file(snippet_path,snippet_sha);
    wcscpy(folder,snippet_path); *wcsrchr(folder,L'\\')=0;
    const wchar_t *ngx_paths[2]={folder,driver_dir};
    ci.PathListInfo.Path=ngx_paths; ci.PathListInfo.Length=2;
    printf("PathListInfo first=%ls\n",folder);
    if (with_loginfo) {
        ci.LoggingInfo.LoggingCallback = ngx_log;
        ci.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
        ci.LoggingInfo.DisableOtherLoggingSinks = 1;
    }
    wchar_t wdir[MAX_PATH]; GetFullPathNameW(nv_directory,MAX_PATH,wdir,NULL);
    load_ngx(0);
    fprintf(stderr, "[glm-mfg] calling NGX init (loginfo=%d)... ngx base=%p initpid=%p\n", with_loginfo, (void *)ngx_dll, (void *)(size_t)glm_init_pid6);
    fflush(stderr);
    const char *v = "0x15";
    unsigned sdkver = v ? (unsigned)strtoul(v, NULL, 0) : 0;
    const char *m = NULL;
    NVSDK_NGX_Result r;
    if (m && !strcmp(m, "ext")) {
        fprintf(stderr, "[glm-mfg] trying Init_Ext(4-arg)...\n");
        r = glm_init_ext(with_loginfo ? &ci : NULL, NVSDK_NGX_Version_API, wdir, dev);
    } else if (sdkver) {
        fprintf(stderr, "[glm-mfg] trying 7-arg init with SDKVersion=0x%x\n", sdkver);
        r = glm_init_pid7("5b1c7d3e-9a2f-4c8b-b6e0-1f2d3a4b5c6d", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
            "0.1", wdir, dev, (unsigned long long)sdkver, with_loginfo ? &ci : NULL);
    } else {
        r = glm_init_pid6("5b1c7d3e-9a2f-4c8b-b6e0-1f2d3a4b5c6d", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
            "0.1", wdir, dev, with_loginfo ? &ci : NULL);
    }
    fprintf(stderr, "[glm-mfg] init result 0x%08X\n", (unsigned)r);
    NGXCHK(r, "NGX_D3D12_Init");
    NGXCHK(NVSDK_NGX_D3D12_GetCapabilityParameters(&caps_params), "GetCapabilityParameters");

}

static int patch_mfg_gate(void) {
    HMODULE module=verify_snippet();
    unsigned char *base=(unsigned char*)module;
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64*)(base+((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec=IMAGE_FIRST_SECTION(nt);
    static const unsigned char pattern[13]={0x84,0xd2,0x0f,0x84,0x03,0x01,0,0,0xbe,0x05,0,0,0};
    static const unsigned char patched[13]={0x84,0xd2,0xeb,0x04,0x03,0x01,0,0,0xbe,0x05,0,0,0};
    unsigned matches=0; unsigned char *match=NULL;
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;i++,sec++) {
        if(!(sec->Characteristics&IMAGE_SCN_MEM_EXECUTE)) continue;
        for(size_t o=0;o+13<=sec->Misc.VirtualSize;o++)
            if(!memcmp(base+sec->VirtualAddress+o,pattern,13)||!memcmp(base+sec->VirtualAddress+o,patched,13)) { match=base+sec->VirtualAddress+o; matches++; }
    }
    printf("GATE signature_matches=%u\n",matches);
    if(matches!=1) return 0;
    if(!memcmp(match,patched,13)){puts("GATE already patched: unique verified site");return 1;}
    printf("GATE RVA=%llx before=",(unsigned long long)(match+2-base));
    for(unsigned i=0;i<13;i++) printf("%02X",match[i]);
    DWORD prior=0,unused=0;
    if(!VirtualProtect(match+2,2,PAGE_EXECUTE_READWRITE,&prior)) return 0;
    match[2]=0xeb;match[3]=0x04;
    if(!VirtualProtect(match+2,2,prior,&unused) || !FlushInstructionCache(GetCurrentProcess(),match+2,2)) FATAL("gate restore protection",GetLastError());
    printf(" after="); for(unsigned i=0;i<13;i++) printf("%02X",match[i]); puts(" RAM_ONLY");
    return 1;
}
static unsigned create_count=0;
static void create_fg(unsigned mult) {
    t_out = make_tex(W,H,FMT_COLOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    {   D3D12_HEAP_PROPERTIES hp; memset(&hp, 0, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d; memset(&d, 0, sizeof(d));
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = 16; d.Height = 1;
        d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&t_disable_interp) != S_OK) FATAL("CreateCommittedResource(disable_interp)", 0); }

    NGXCHK(NVSDK_NGX_D3D12_AllocateParameters(&eval_params), "AllocateParameters");
    NVSDK_NGX_DLSSG_Create_Params cp; memset(&cp, 0, sizeof(cp));
    cp.Width = W; cp.Height = H; cp.NativeBackbufferFormat = FMT_COLOR;
    cp.RenderWidth = RW; cp.RenderHeight = RH; cp.DynamicResolutionScaling = 0;
    NVSDK_NGX_Parameter_SetUI(eval_params, NVSDK_NGX_DLSSG_Parameter_Width, W);
    NVSDK_NGX_Parameter_SetUI(eval_params, NVSDK_NGX_DLSSG_Parameter_Height, H);
    NVSDK_NGX_Parameter_SetUI(eval_params, NVSDK_NGX_DLSSG_Parameter_BackbufferFormat, FMT_COLOR);
    NVSDK_NGX_Parameter_SetD3d12Resource(eval_params, NVSDK_NGX_DLSSG_Parameter_Backbuffer, t_back.res);
    NVSDK_NGX_Parameter_SetD3d12Resource(eval_params, NVSDK_NGX_DLSSG_Parameter_OutputInterpolated, t_out.res);
    gfx_begin();
    verify_loaded_snippets(0);
    NGXCHK(NGX_D3D12_CREATE_DLSSG(cmd, 1, 1, &fg_feature, eval_params, &cp), "CREATE_DLSSG");
    verify_loaded_snippets(1);
    gfx_submit();
    create_count++;
    printf("CREATE feature=%p create_count=%u\n",(void*)fg_feature,create_count);
    if(cpvr_enable_gate && mult>2 && !cpvr_gate_applied){if(!patch_mfg_gate()) FATAL("unique hash-qualified compatibility gate unavailable",0);cpvr_gate_applied=1;}
    (void)mult;
}


static NVSDK_NGX_DLSSG_Opt_Eval_Params *active_opt;
static unsigned last_hint=0xcd;
static void clear_hint(void) {
    ID3D12Resource *up=make_buf(16,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    void *ptr=NULL; if(ID3D12Resource_Map(up,0,NULL,&ptr)!=S_OK) FATAL("hint upload Map",0);
    memset(ptr,0xcd,16); ID3D12Resource_Unmap(up,0,NULL);
    gfx_begin(); transition(t_disable_interp,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyBufferRegion(cmd,t_disable_interp,0,up,0,16);
    transition(t_disable_interp,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gfx_submit(); ID3D12Resource_Release(up);
}
static unsigned read_hint(void) {
    ID3D12Resource *rb=make_buf(16,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    gfx_begin(); transition(t_disable_interp,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyBufferRegion(cmd,rb,0,t_disable_interp,0,16);
    transition(t_disable_interp,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gfx_submit(); void *ptr=NULL; D3D12_RANGE range={0,16};
    if(ID3D12Resource_Map(rb,0,&range,&ptr)!=S_OK) FATAL("hint readback Map",0);
    unsigned result=*(unsigned char*)ptr;
    ID3D12Resource_Unmap(rb,0,NULL); ID3D12Resource_Release(rb); return result;
}
static void evaluate_index(void) {
    if(active_opt->multiFrameIndex==1) clear_hint(); /* v2: fresh sentinel per batch */
    gfx_begin(); set_state(&t_back,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    set_state(&t_mv,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    set_state(&t_depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    set_state(&t_out,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    NVSDK_NGX_D3D12_DLSSG_Eval_Params rp={0};
    rp.pBackbuffer=t_back.res;rp.pDepth=t_depth.res;rp.pMVecs=t_mv.res;
    rp.pHudless=t_back.res;rp.pOutputInterpFrame=t_out.res;rp.pOutputDisableInterpolation=t_disable_interp;
    NVSDK_NGX_Parameter_SetUI(eval_params,NVSDK_NGX_DLSSG_Parameter_MultiFrameCount,active_opt->multiFrameCount);
    NVSDK_NGX_Parameter_SetUI(eval_params,NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex,active_opt->multiFrameIndex);
    unsigned count=0,index=0;
    NGXCHK(NVSDK_NGX_Parameter_GetUI(eval_params,NVSDK_NGX_DLSSG_Parameter_MultiFrameCount,&count),"Count roundtrip");
    NGXCHK(NVSDK_NGX_Parameter_GetUI(eval_params,NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex,&index),"Index roundtrip");
    if(count!=active_opt->multiFrameCount || index!=active_opt->multiFrameIndex) FATAL("parameter ABI roundtrip",0);
    NVSDK_NGX_Result result=NGX_D3D12_EVALUATE_DLSSG(cmd,fg_feature,eval_params,&rp,active_opt);
    printf("EVAL feature=%p count=%u index=%u reset=%u result=0x%08X\n",(void*)fg_feature,count,index,(unsigned)active_opt->reset,(unsigned)result);
    NGXCHK(result,"EVALUATE"); gfx_submit(); last_hint=read_hint();
    printf("HINT count=%u index=%u hint=%u completed=%llu\n",count,index,last_hint,(unsigned long long)fval);
}

/* This ABI is checked against CURRENT P11 declarations, not a former capture. */

#include "nv_backend_impl.inc"

