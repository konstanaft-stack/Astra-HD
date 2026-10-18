#pragma once
#include <windows.h>
#include <d3d12.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct cpvr_nv_camera { float f[104]; uint32_t flags[7]; } cpvr_nv_camera;
typedef struct cpvr_nv_region {uint32_t left,top,right,bottom;} cpvr_nv_region;
typedef struct cpvr_nv_frame {
    ID3D12Resource *color,*motion,*depth;
    D3D12_RESOURCE_STATES resting[3];
    ID3D12Fence *ready[2]; uint64_t ready_value[2];
    uint64_t source_f,owner_runtime; uint32_t feature;
    cpvr_nv_region output_region;
    cpvr_nv_camera camera;
} cpvr_nv_frame;
typedef struct cpvr_nv_status {int code;unsigned ngx_result,hint,count;uint64_t completed;char message[192];} cpvr_nv_status;
typedef int (*cpvr_nv_configure_fn)(const wchar_t*,const char*,const char*,int);
typedef int (*cpvr_nv_attach_fn)(ID3D12Device*,const wchar_t*,int);
typedef int (*cpvr_nv_run_fn)(unsigned,uint64_t,const cpvr_nv_frame*,const cpvr_nv_frame*,unsigned,ID3D12Resource*const[5]);
typedef void (*cpvr_nv_ready_callback)(void*,unsigned,unsigned,unsigned,uint64_t,uint64_t);
typedef int (*cpvr_nv_run_ready_fn)(unsigned,uint64_t,const cpvr_nv_frame*,const cpvr_nv_frame*,unsigned,ID3D12Resource*const[5],cpvr_nv_ready_callback,void*);
typedef cpvr_nv_status (*cpvr_nv_status_fn)(void);
typedef ID3D12CommandQueue* (*cpvr_nv_queue_fn)(void);
typedef ID3D12Fence* (*cpvr_nv_fence_fn)(void);
typedef int (*cpvr_nv_detach_fn)(void);
/* Optional: returns 1 only after a proven pre-NGX identity rejection has
   released all partial owners. Missing/false must retain the failed backend. */
typedef int (*cpvr_nv_abort_unstarted_attach_fn)(void);
#ifdef __cplusplus
}
#endif
