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
