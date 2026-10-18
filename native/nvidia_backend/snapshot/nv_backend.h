#ifndef FG_NV_BACKEND_H
#define FG_NV_BACKEND_H
#include <stdint.h>
#include <windows.h>
#include <d3d12.h>
typedef struct fg_nv_camera {float f[104];uint32_t flags[7];} fg_nv_camera;
typedef struct fg_nv_region {uint32_t left,top,right,bottom;} fg_nv_region;
/* All-zero means the whole source. A non-empty region is an exact pixel crop,
   never a scale or projection transform. NVIDIA still receives the full grid. */
static inline int fg_nv_normalize_region(fg_nv_region region,uint32_t w,uint32_t h,fg_nv_region *out){
    if(!out||!w||!h)return 0;
    if(!region.left&&!region.top&&!region.right&&!region.bottom)region=(fg_nv_region){0,0,w,h};
    if(region.left>=region.right||region.top>=region.bottom||region.right>w||region.bottom>h)return 0;
    *out=region;return 1;
}
typedef struct fg_nv_frame {
    ID3D12Resource *color,*motion,*depth;
    D3D12_RESOURCE_STATES resting[3];
    ID3D12Fence *ready[2];uint64_t ready_value[2];
    uint64_t source_f,owner_runtime;uint32_t feature;
    fg_nv_region output_region;
    fg_nv_camera camera;
} fg_nv_frame;
typedef struct fg_nv_status {int code;unsigned ngx_result,hint,count;uint64_t completed;char message[192];} fg_nv_status;
/* Optional diagnostic ABI. GPU ticks are meaningful only with valid=1 and a
   completed fence. They measure batch start and completion of each output copy,
   not CPU queue wait, source age, OpenXR transport or display latency. */
typedef struct fg_nv_gpu_timing {
    uint64_t frequency,completed,source_f,ticks[6];
    unsigned count,valid,hints[5];
} fg_nv_gpu_timing;
typedef void (*fg_nv_output_ready)(void *context,unsigned index,unsigned count,unsigned hint,
                                  uint64_t completed,uint64_t batch_completed);
/* All calls have one dedicated CPU worker owner. Caller retains all inputs and
   output targets until run returns. Ready values must denote already scheduled
   producer signals. Targets enter/leave COMMON and are not shared with a live
   consumer until the complete successful run has returned. */
#ifdef __cplusplus
extern "C" {
#endif
int fg_nv_attach(ID3D12Device *device,const wchar_t *snippet_directory,int initialize_ngx);
int fg_nv_abort_unstarted_attach(void);
int fg_nv_run(unsigned eye,uint64_t epoch,const fg_nv_frame *previous,const fg_nv_frame *current,unsigned count,ID3D12Resource *const outputs[5]);
/* Optional progressive publication. All inputs/targets remain owned until
   return. The callback may expose only its completed, hint-zero output. It
   runs on the owner thread; do not throw or reenter mutating backend APIs. */
int fg_nv_run_ready(unsigned eye,uint64_t epoch,const fg_nv_frame *previous,const fg_nv_frame *current,unsigned count,
                    ID3D12Resource *const outputs[5],fg_nv_output_ready callback,void *context);
fg_nv_status fg_nv_get_status(void);
ID3D12CommandQueue *fg_nv_queue(void);
ID3D12Fence *fg_nv_fence(void);
uint64_t fg_nv_evaluation_count(void);
int fg_nv_enable_gpu_timing(int enabled);
fg_nv_gpu_timing fg_nv_get_gpu_timing(void);
#ifdef __cplusplus
}
#endif
#endif
