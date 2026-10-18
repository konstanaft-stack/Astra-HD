typedef struct NvEye {
 NVSDK_NGX_Handle *handle;NVSDK_NGX_Parameter *parameters;
 NVSDK_NGX_DLSSG_Opt_Eval_Params options;
 Tex work;ID3D12Resource *hint_buffer,*hint_upload,*hint_readback;
 uint64_t epoch,last_source,owner_runtime;uint32_t owner_feature;UINT w,h,rw,rh;int primed;
} NvEye;
static NvEye nv_eyes[2];
static volatile LONG64 nv_evaluations;
static int nv_gpu_timing_enabled;
static uint64_t nv_gpu_frequency;
static ID3D12QueryHeap *nv_timestamp_heap;
static ID3D12Resource *nv_timestamp_readback;
static fg_nv_gpu_timing nv_last_gpu_timing;
static int nv_run_active;
static fg_nv_output_ready nv_output_ready;
static void *nv_output_context;
static ID3D12CommandAllocator *nv_phase_allocators[5];
static ID3D12GraphicsCommandList *nv_phase_lists[5];

static void nv_prepare_phase_lists(unsigned count){
 for(unsigned i=0;i<count;i++){
  if(!nv_phase_allocators[i]&&ID3D12Device_CreateCommandAllocator(dev,D3D12_COMMAND_LIST_TYPE_DIRECT,&IID_ID3D12CommandAllocator,(void**)&nv_phase_allocators[i])!=S_OK)FATAL("phase allocator Create",0);
  if(!nv_phase_lists[i]){
   if(ID3D12Device_CreateCommandList(dev,0,D3D12_COMMAND_LIST_TYPE_DIRECT,nv_phase_allocators[i],NULL,&IID_ID3D12GraphicsCommandList,(void**)&nv_phase_lists[i])!=S_OK)FATAL("phase command list Create",0);
   if(ID3D12GraphicsCommandList_Close(nv_phase_lists[i])!=S_OK)FATAL("phase initial Close",0);
  }
 }
}

int fg_nv_enable_gpu_timing(int enabled){
 if(!nv_attached||nv_failed||nv_run_active||GetCurrentThreadId()!=nv_thread)return 0;
 memset(&nv_last_gpu_timing,0,sizeof(nv_last_gpu_timing));nv_gpu_timing_enabled=0;
 if(!enabled)return 1;
 if(setjmp(nv_jump))return 0;
 if(ID3D12CommandQueue_GetTimestampFrequency(queue,&nv_gpu_frequency)!=S_OK||!nv_gpu_frequency)return 0;
 if(!nv_timestamp_heap){
  D3D12_QUERY_HEAP_DESC desc={.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP,.Count=6,.NodeMask=0};
  if(ID3D12Device_CreateQueryHeap(dev,&desc,&IID_ID3D12QueryHeap,(void**)&nv_timestamp_heap)!=S_OK)return 0;
 }
 if(!nv_timestamp_readback)nv_timestamp_readback=make_buf(6*sizeof(uint64_t),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
 nv_gpu_timing_enabled=1;return 1;
}
fg_nv_gpu_timing fg_nv_get_gpu_timing(void){
 if(!nv_attached||nv_failed||!nv_gpu_timing_enabled||GetCurrentThreadId()!=nv_thread)return (fg_nv_gpu_timing){0};
 return nv_last_gpu_timing;
}

static int nv_reject(const char *message){nv_status.code=0;snprintf(nv_status.message,sizeof(nv_status.message),"%s",message);return 0;}
static int nv_same_device(ID3D12DeviceChild *child){
 ID3D12Device *other_device=NULL;IUnknown *a=NULL,*b=NULL;int same=0;
 if(ID3D12DeviceChild_GetDevice(child,&IID_ID3D12Device,(void**)&other_device)!=S_OK||!other_device)return 0;
 if(ID3D12Device_QueryInterface(dev,&IID_IUnknown,(void**)&a)==S_OK&&ID3D12Device_QueryInterface(other_device,&IID_IUnknown,(void**)&b)==S_OK)same=a==b;
 if(a)IUnknown_Release(a);if(b)IUnknown_Release(b);ID3D12Device_Release(other_device);return same;
}
static void nv_frame_shapes(const fg_nv_frame *f,D3D12_RESOURCE_DESC d[3]){
 ID3D12Resource *r[]={f->color,f->motion,f->depth};
 for(unsigned i=0;i<3;i++){
  if(!r[i]||!nv_same_device((ID3D12DeviceChild*)r[i]))FATAL("missing/foreign owned frame texture",0);
  d[i]=ID3D12Resource_GetDesc(r[i]);
  if(d[i].Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d[i].DepthOrArraySize!=1||d[i].MipLevels!=1||d[i].SampleDesc.Count!=1||!d[i].Width||!d[i].Height||d[i].Width>16384||d[i].Height>16384)FATAL("frame texture shape",0);
 }
 if(d[0].Format!=DXGI_FORMAT_R8G8B8A8_UNORM||d[1].Format!=DXGI_FORMAT_R16G16_FLOAT||
   (d[2].Format!=DXGI_FORMAT_R32_FLOAT&&d[2].Format!=DXGI_FORMAT_R32_TYPELESS)||d[1].Width!=d[2].Width||d[1].Height!=d[2].Height)FATAL("frame texture format/extent",0);
 if(!f->ready[0]||!f->ready_value[0]||f->ready_value[0]==UINT64_MAX)FATAL("frame lacks scheduled producer completion",0);
 for(unsigned i=0;i<104;i++)if(!isfinite(f->camera.f[i])||fabsf(f->camera.f[i])>=3.4e38f)FATAL("invalid owned camera",0);
 if(f->camera.f[82]!=1||f->camera.f[83]!=1||f->camera.flags[1]!=1||f->camera.flags[2]!=0)FATAL("motion transfer contract",0);
}
static void nv_bind(const fg_nv_frame *f,unsigned count,unsigned reset){
 for(unsigned i=0;i<2;i++)if(f->ready[i]){
  if(!f->ready_value[i]||f->ready_value[i]==UINT64_MAX)FATAL("invalid producer fence value",0);
  if(ID3D12CommandQueue_Wait(queue,f->ready[i],f->ready_value[i])!=S_OK)FATAL("producer wait",0);
 }
 t_back=(Tex){f->color,W,H,FMT_COLOR,f->resting[0]};
 t_mv=(Tex){f->motion,RW,RH,FMT_MV,f->resting[1]};
 t_depth=(Tex){f->depth,RW,RH,FMT_DEPTH,f->resting[2]};
 const float *v=f->camera.f;const uint32_t *flags=f->camera.flags;
 memset(active_opt,0,sizeof(*active_opt));
 active_opt->multiFrameCount=count;active_opt->multiFrameIndex=1;
 memcpy(active_opt->cameraViewToClip,v,64);memcpy(active_opt->clipToCameraView,v+16,64);memcpy(active_opt->clipToLensClip,v+32,64);
 memcpy(active_opt->clipToPrevClip,v+48,64);memcpy(active_opt->prevClipToClip,v+64,64);
 memcpy(active_opt->jitterOffset,v+80,8);memcpy(active_opt->mvecScale,v+82,8);memcpy(active_opt->cameraPinholeOffset,v+84,8);
 memcpy(active_opt->cameraPos,v+86,12);memcpy(active_opt->cameraUp,v+89,12);memcpy(active_opt->cameraRight,v+92,12);memcpy(active_opt->cameraFwd,v+95,12);
 active_opt->cameraNear=v[98];active_opt->cameraFar=v[99];active_opt->cameraFOV=v[100];active_opt->cameraAspectRatio=v[101];
 active_opt->motionVectorsInvalidValue=v[102];active_opt->minRelativeLinearDepthObjectSeparation=v[103];
 active_opt->depthInverted=(bool)flags[0];active_opt->cameraMotionIncluded=(bool)flags[1];active_opt->reset=reset!=0;
 active_opt->orthoProjection=(bool)flags[4];active_opt->motionVectorsDilated=(bool)flags[5];active_opt->colorBuffersHDR=false;
 active_opt->mvecsSubrectSize=(NVSDK_NGX_Dimensions){RW,RH};active_opt->depthSubrectSize=active_opt->mvecsSubrectSize;
 active_opt->hudLessSubrectSize=(NVSDK_NGX_Dimensions){W,H};active_opt->backbufferSubrectSize=active_opt->hudLessSubrectSize;active_opt->outputInterpSubrectSize=active_opt->hudLessSubrectSize;
 NVSDK_NGX_Parameter_SetUI(eval_params,NVSDK_NGX_DLSSG_Parameter_MvecJittered,flags[6]);
}
static void nv_restore_inputs(const fg_nv_frame *f){
 gfx_begin();set_state(&t_back,f->resting[0]);set_state(&t_mv,f->resting[1]);set_state(&t_depth,f->resting[2]);gfx_submit();
}
/* Always record the complete temporal batch before waiting. The optional
   ready path closes one list per output and enqueues all lists/fences before
   reporting completed phases; later GPU work runs during earlier callbacks.
   Legacy callers retain the original single-list/single-fence behavior. */
static unsigned nv_batch(NvEye *e,const fg_nv_frame *frame,unsigned count,unsigned reset,ID3D12Resource *const *outputs,const fg_nv_region *region){
 if(!e->hint_upload){
  e->hint_upload=make_buf(16,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
  e->hint_readback=make_buf(80,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  void*p=NULL;D3D12_RANGE no_read={0,0};if(ID3D12Resource_Map(e->hint_upload,0,&no_read,&p)!=S_OK)FATAL("hint sentinel Map",0);
  memset(p,0xcd,16);ID3D12Resource_Unmap(e->hint_upload,0,NULL);
 }
 const int progressive=outputs&&nv_output_ready;
 ID3D12GraphicsCommandList *base_cmd=cmd;ID3D12CommandAllocator *base_alloc=alloc;
 UINT64 phase_values[5]={0};
 if(progressive){nv_prepare_phase_lists(count);cmd=nv_phase_lists[0];alloc=nv_phase_allocators[0];}
 nv_bind(frame,count,reset);gfx_begin();
 if(nv_gpu_timing_enabled)ID3D12GraphicsCommandList_EndQuery(cmd,nv_timestamp_heap,D3D12_QUERY_TYPE_TIMESTAMP,0);
 transition(t_disable_interp,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
 ID3D12GraphicsCommandList_CopyBufferRegion(cmd,t_disable_interp,0,e->hint_upload,0,16);
 transition(t_disable_interp,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 set_state(&t_back,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);set_state(&t_mv,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);set_state(&t_depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);set_state(&t_out,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 for(unsigned i=1;i<=count;i++){
  active_opt->multiFrameIndex=i;NVSDK_NGX_D3D12_DLSSG_Eval_Params rp={0};
  rp.pBackbuffer=t_back.res;rp.pDepth=t_depth.res;rp.pMVecs=t_mv.res;rp.pHudless=t_back.res;rp.pOutputInterpFrame=t_out.res;rp.pOutputDisableInterpolation=t_disable_interp;
  NVSDK_NGX_Parameter_SetUI(eval_params,NVSDK_NGX_DLSSG_Parameter_MultiFrameCount,count);
  NVSDK_NGX_Parameter_SetUI(eval_params,NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex,i);
  unsigned got_count=0,got_index=0;
  NGXCHK(NVSDK_NGX_Parameter_GetUI(eval_params,NVSDK_NGX_DLSSG_Parameter_MultiFrameCount,&got_count),"Count roundtrip");
  NGXCHK(NVSDK_NGX_Parameter_GetUI(eval_params,NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex,&got_index),"Index roundtrip");
  if(got_count!=count||got_index!=i)FATAL("parameter ABI roundtrip",0);
  NGXCHK(NGX_D3D12_EVALUATE_DLSSG(cmd,fg_feature,eval_params,&rp,active_opt),"EVALUATE batch");
  InterlockedIncrement64(&nv_evaluations);
  transition(t_disable_interp,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
  ID3D12GraphicsCommandList_CopyBufferRegion(cmd,e->hint_readback,(i-1)*16,t_disable_interp,0,16);
  transition(t_disable_interp,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if(outputs){
   set_state(&t_out,D3D12_RESOURCE_STATE_COPY_SOURCE);transition(outputs[i-1],D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
   if(!region||(!region->left&&!region->top&&region->right==W&&region->bottom==H))ID3D12GraphicsCommandList_CopyResource(cmd,outputs[i-1],t_out.res);
   else {
    D3D12_TEXTURE_COPY_LOCATION dst={.pResource=outputs[i-1],.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,.SubresourceIndex=0};
    D3D12_TEXTURE_COPY_LOCATION src={.pResource=t_out.res,.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,.SubresourceIndex=0};
    D3D12_BOX box={region->left,region->top,0,region->right,region->bottom,1};
    ID3D12GraphicsCommandList_CopyTextureRegion(cmd,&dst,0,0,0,&src,&box);
   }
   transition(outputs[i-1],D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);set_state(&t_out,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }else{D3D12_RESOURCE_BARRIER u={0};u.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;u.UAV.pResource=t_out.res;ID3D12GraphicsCommandList_ResourceBarrier(cmd,1,&u);}
  if(nv_gpu_timing_enabled)ID3D12GraphicsCommandList_EndQuery(cmd,nv_timestamp_heap,D3D12_QUERY_TYPE_TIMESTAMP,i);
  if(progressive&&i<count){
   if(ID3D12GraphicsCommandList_Close(cmd)!=S_OK)FATAL("phase recording Close",0);
   cmd=nv_phase_lists[i];alloc=nv_phase_allocators[i];gfx_begin();
  }
 }
 if(nv_gpu_timing_enabled)ID3D12GraphicsCommandList_ResolveQueryData(cmd,nv_timestamp_heap,D3D12_QUERY_TYPE_TIMESTAMP,0,count+1,nv_timestamp_readback,0);
 set_state(&t_back,frame->resting[0]);set_state(&t_mv,frame->resting[1]);set_state(&t_depth,frame->resting[2]);
 if(progressive){
  if(ID3D12GraphicsCommandList_Close(cmd)!=S_OK)FATAL("last phase Close",0);
  for(unsigned i=0;i<count;i++){cmd=nv_phase_lists[i];phase_values[i]=gfx_enqueue_closed();}
  cmd=base_cmd;alloc=base_alloc;
  for(unsigned i=0;i<count;i++){
   gfx_wait_for(phase_values[i]);
   void *data=NULL;D3D12_RANGE ready_range={i*16,(i+1)*16},unchanged={0,0};
   if(ID3D12Resource_Map(e->hint_readback,0,&ready_range,&data)!=S_OK)FATAL("ready phase hint Map",0);
   const unsigned hint=((const unsigned char*)data)[i*16];
   ID3D12Resource_Unmap(e->hint_readback,0,&unchanged);
   nv_output_ready(nv_output_context,i+1,count,hint,phase_values[i],phase_values[count-1]);
  }
 }else gfx_submit();
 void *hints=NULL;D3D12_RANGE range={0,count*16};if(ID3D12Resource_Map(e->hint_readback,0,&range,&hints)!=S_OK)FATAL("batch hint Map",0);
 unsigned result=0;for(unsigned i=0;i<count;i++){
  const unsigned hint=((unsigned char*)hints)[i*16];result|=hint;
  if(nv_gpu_timing_enabled)nv_last_gpu_timing.hints[i]=hint;
 }
 D3D12_RANGE unchanged={0,0};ID3D12Resource_Unmap(e->hint_readback,0,&unchanged);
 if(nv_gpu_timing_enabled){
  void *ticks=NULL;D3D12_RANGE range={0,(count+1)*sizeof(uint64_t)};
  if(ID3D12Resource_Map(nv_timestamp_readback,0,&range,&ticks)!=S_OK)FATAL("GPU timestamp readback Map",0);
  memcpy(nv_last_gpu_timing.ticks,ticks,range.End);
  ID3D12Resource_Unmap(nv_timestamp_readback,0,&unchanged);
  nv_last_gpu_timing.frequency=nv_gpu_frequency;nv_last_gpu_timing.completed=fval;
  nv_last_gpu_timing.source_f=frame->source_f;nv_last_gpu_timing.count=count;nv_last_gpu_timing.valid=1;
 }
 return result;
}
int fg_nv_attach(ID3D12Device *device,const wchar_t *directory,int initialize_ngx){
 if(nv_failed)return 0;
 if(!device||!directory||!directory[0])return nv_reject("backend attach arguments");
 if(nv_attached)return device==dev&&GetCurrentThreadId()==nv_thread;
 nv_thread=GetCurrentThreadId();memset(&nv_status,0,sizeof(nv_status));
 if(setjmp(nv_jump))return 0;
 if(!GetFullPathNameW(directory,MAX_PATH,nv_directory,NULL))FATAL("snippet directory",GetLastError());
 dev=device;ID3D12Device_AddRef(dev);init_d3d12();
 if(initialize_ngx){cpvr_initialized_ngx=1;init_ngx(1);}
 else {
  if(swprintf(snippet_path,MAX_PATH,L"%ls\\nvngx_dlssg.dll",nv_directory)<0)FATAL("snippet directory size",0);
  identity_stage="snippet-file";nv_shared_snippet_preflight=1;
  verify_file(snippet_path,snippet_sha);
  nv_shared_snippet_preflight=0;load_ngx(1);
  NGXCHK(NVSDK_NGX_D3D12_GetCapabilityParameters(&caps_params),"Get game NGX capabilities");
 }
 nv_attached=1;nv_status.code=1;snprintf(nv_status.message,sizeof(nv_status.message),"backend attached; profile=%s; %s",cpvr_profile_name,initialize_ngx?"owned NGX init":"shared pinned core; no additional Init");return 1;
}
static int nv_run_core(unsigned eye,uint64_t epoch,const fg_nv_frame *previous,const fg_nv_frame *current,unsigned count,ID3D12Resource *const outputs[5]){
 if(GetCurrentThreadId()==nv_thread)memset(&nv_last_gpu_timing,0,sizeof(nv_last_gpu_timing));
 if(nv_failed)return 0;
 if(!nv_attached||nv_status.code<0||GetCurrentThreadId()!=nv_thread||eye>1||!epoch||!previous||!current||!outputs||count<1||count>5)return nv_reject("backend run arguments/state");
 if(cpvr_native_x2_only&&count!=1)return nv_reject("current NVIDIA profile qualified for native X2 only");
 if(current->source_f<=previous->source_f||current->source_f-previous->source_f!=2)return nv_reject("nonconsecutive same-eye source pair");
 if(!current->owner_runtime||current->owner_runtime==UINT64_MAX||current->feature>=64||current->owner_runtime!=previous->owner_runtime||current->feature!=previous->feature)return nv_reject("input owner mismatch");
 if(setjmp(nv_jump)){memset(&nv_last_gpu_timing,0,sizeof(nv_last_gpu_timing));return 0;}
 D3D12_RESOURCE_DESC a[3],b[3];nv_frame_shapes(previous,a);nv_frame_shapes(current,b);
 for(unsigned i=0;i<3;i++)if(a[i].Width!=b[i].Width||a[i].Height!=b[i].Height||a[i].Format!=b[i].Format)FATAL("pair shape changed",0);
 W=(UINT)a[0].Width;H=a[0].Height;RW=(UINT)a[1].Width;RH=a[1].Height;
 fg_nv_region old_region,region;
 if(!fg_nv_normalize_region(previous->output_region,W,H,&old_region)||!fg_nv_normalize_region(current->output_region,W,H,&region)||memcmp(&old_region,&region,sizeof(region)))return nv_reject("invalid or changing output crop");
 for(unsigned i=0;i<count;i++){
  if(!outputs[i]||!nv_same_device((ID3D12DeviceChild*)outputs[i]))FATAL("missing/foreign output target",0);
  for(unsigned j=0;j<i;j++)if(outputs[i]==outputs[j])FATAL("aliased output set",0);
  if(outputs[i]==previous->color||outputs[i]==current->color)FATAL("output aliases input",0);
  D3D12_RESOURCE_DESC d=ID3D12Resource_GetDesc(outputs[i]);
  if(d.Width!=region.right-region.left||d.Height!=region.bottom-region.top||d.MipLevels!=1||d.DepthOrArraySize!=1||d.SampleDesc.Count!=1||(d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM&&d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))FATAL("output shape/format",0);
 }
 NvEye *e=&nv_eyes[eye];active_opt=&e->options;
 if(e->handle&&(e->w!=W||e->h!=H||e->rw!=RW||e->rh!=RH)){
  /* All worker GPU work is drained before reconfiguration; presentation targets
     are caller-owned and independent of these private work resources. */
  NGXCHK(glm_release(e->handle),"Release resized NGX feature");
  NGXCHK(glm_destroy_params(e->parameters),"Destroy resized NGX parameters");
  ID3D12Resource_Release(e->work.res);ID3D12Resource_Release(e->hint_buffer);
  if(e->hint_upload)ID3D12Resource_Release(e->hint_upload);if(e->hint_readback)ID3D12Resource_Release(e->hint_readback);
  memset(e,0,sizeof(*e));active_opt=&e->options;
 }
 if(!e->handle){
  t_back=(Tex){previous->color,W,H,FMT_COLOR,previous->resting[0]};
  create_fg(count+1);e->handle=fg_feature;e->parameters=eval_params;e->work=t_out;e->hint_buffer=t_disable_interp;e->w=W;e->h=H;e->rw=RW;e->rh=RH;
 }
 fg_feature=e->handle;eval_params=e->parameters;t_out=e->work;t_disable_interp=e->hint_buffer;if(cpvr_enable_gate && count>1 && !cpvr_gate_applied){if(!patch_mfg_gate())FATAL("hash-qualified hot MFG gate unavailable",0);cpvr_gate_applied=1;}
 if(!e->primed||e->last_source!=previous->source_f||e->epoch!=epoch||e->owner_runtime!=current->owner_runtime||e->owner_feature!=current->feature){
  /* History initialization has no temporal pair or delivered output. One
     reset evaluation establishes it; repeating every requested phase wastes
     GPU work. The phase probe covers both first use and epoch resets at X6. */
  nv_batch(e,previous,1,1,NULL,NULL);e->primed=1;e->last_source=previous->source_f;e->epoch=epoch;
 }
 const unsigned hint=nv_batch(e,current,count,current->camera.flags[3],outputs,&region);
 e->last_source=current->source_f;e->work=t_out;e->epoch=epoch;e->owner_runtime=current->owner_runtime;e->owner_feature=current->feature;
 nv_status=(fg_nv_status){hint?0:1,1,hint,count,fval,{0}};
 snprintf(nv_status.message,sizeof(nv_status.message),"%s",hint?"vendor disabled interpolation":"complete GPU batch");
 return hint==0;
}
static int nv_run_owned(unsigned eye,uint64_t epoch,const fg_nv_frame *previous,const fg_nv_frame *current,unsigned count,
 ID3D12Resource *const outputs[5],fg_nv_output_ready callback,void *context){
 if(GetCurrentThreadId()!=nv_thread||nv_run_active)return 0;
 ID3D12GraphicsCommandList *base_cmd=cmd;ID3D12CommandAllocator *base_alloc=alloc;
 nv_run_active=1;nv_output_ready=callback;nv_output_context=context;
 const int result=nv_run_core(eye,epoch,previous,current,count,outputs);
 // Restore even after an inner longjmp. Failed/in-flight resources remain
 // retained by the existing nv_failed/nv_unsafe_gpu and caller lease contract.
 cmd=base_cmd;alloc=base_alloc;nv_output_ready=NULL;nv_output_context=NULL;nv_run_active=0;
 return result;
}
int fg_nv_run(unsigned eye,uint64_t epoch,const fg_nv_frame *previous,const fg_nv_frame *current,unsigned count,ID3D12Resource *const outputs[5]){
 return nv_run_owned(eye,epoch,previous,current,count,outputs,NULL,NULL);
}
int fg_nv_run_ready(unsigned eye,uint64_t epoch,const fg_nv_frame *previous,const fg_nv_frame *current,unsigned count,
 ID3D12Resource *const outputs[5],fg_nv_output_ready callback,void *context){
 return nv_run_owned(eye,epoch,previous,current,count,outputs,callback,context);
}
fg_nv_status fg_nv_get_status(void){return nv_status;}
ID3D12CommandQueue *fg_nv_queue(void){return queue;}
ID3D12Fence *fg_nv_fence(void){return fence;}
uint64_t fg_nv_evaluation_count(void){return (uint64_t)InterlockedCompareExchange64(&nv_evaluations,0,0);}

int fg_nv_abort_unstarted_attach(void){
 /* Do not infer safety from an error string or from the current fence alone.
    This proof deliberately excludes every NGX entry and any prior GPU work. */
 if(GetCurrentThreadId()!=nv_thread || !nv_failed || !nv_shared_snippet_preflight || !nv_clean_snippet_identity_reject ||
    nv_attached || nv_run_active || nv_unsafe_gpu || nv_ever_executed || fval ||
    ngx_dll || cpvr_initialized_ngx || caps_params || eval_params || fg_feature ||
    cpvr_snippet_ref_count || t_back.res || t_mv.res || t_depth.res || t_out.res ||
    t_disable_interp || nv_timestamp_heap || nv_timestamp_readback ||
    nv_output_ready || nv_output_context)return 0;
 for(unsigned i=0;i<2;i++){
  const NvEye*e=&nv_eyes[i];
  if(e->handle || e->parameters || e->work.res || e->hint_buffer || e->hint_upload || e->hint_readback)return 0;
 }
 for(unsigned i=0;i<5;i++)if(nv_phase_lists[i] || nv_phase_allocators[i])return 0;
 if(cmd)ID3D12GraphicsCommandList_Release(cmd);cmd=NULL;
 if(alloc)ID3D12CommandAllocator_Release(alloc);alloc=NULL;
 if(queue)ID3D12CommandQueue_Release(queue);queue=NULL;
 if(fence)ID3D12Fence_Release(fence);fence=NULL;
 if(fev&&!CloseHandle(fev))return 0;fev=NULL;
 if(dev)ID3D12Device_Release(dev);dev=NULL;
 nv_shared_snippet_preflight=0;nv_clean_snippet_identity_reject=0;nv_failed=0;nv_thread=0;
 /* Keep the diagnostic status until the next attempt. No NGX API is called. */
 return 1;
}

int fg_nv_detach(void){
 if(GetCurrentThreadId()!=nv_thread || nv_run_active || nv_failed || nv_unsafe_gpu)return 0;
 if(setjmp(nv_jump))return 0;
 for(unsigned i=0;i<2;i++){
  NvEye*e=&nv_eyes[i];
  if(e->handle)NGXCHK(glm_release(e->handle),"Release worker feature");
  if(e->parameters)NGXCHK(glm_destroy_params(e->parameters),"Destroy worker parameters");
  if(e->work.res)ID3D12Resource_Release(e->work.res);
  if(e->hint_buffer)ID3D12Resource_Release(e->hint_buffer);
  if(e->hint_upload)ID3D12Resource_Release(e->hint_upload);
  if(e->hint_readback)ID3D12Resource_Release(e->hint_readback);
  memset(e,0,sizeof(*e));
 }
 if(caps_params)NGXCHK(glm_destroy_params(caps_params),"Destroy worker capabilities");caps_params=NULL;
 if(nv_timestamp_heap)ID3D12QueryHeap_Release(nv_timestamp_heap);nv_timestamp_heap=NULL;
 if(nv_timestamp_readback)ID3D12Resource_Release(nv_timestamp_readback);nv_timestamp_readback=NULL;
 for(unsigned i=0;i<5;i++){
  if(nv_phase_lists[i])ID3D12GraphicsCommandList_Release(nv_phase_lists[i]);nv_phase_lists[i]=NULL;
  if(nv_phase_allocators[i])ID3D12CommandAllocator_Release(nv_phase_allocators[i]);nv_phase_allocators[i]=NULL;
 }
 nv_gpu_timing_enabled=0;nv_gpu_frequency=0;memset(&nv_last_gpu_timing,0,sizeof(nv_last_gpu_timing));
 if(cpvr_initialized_ngx){unsigned remaining=0;NGXCHK(glm_shutdown1(dev,&remaining),"Shutdown own NGX");printf("SHUTDOWN own NGX remaining=%u\n",remaining);}
 if(cmd)ID3D12GraphicsCommandList_Release(cmd);if(alloc)ID3D12CommandAllocator_Release(alloc);
 if(queue)ID3D12CommandQueue_Release(queue);if(fence)ID3D12Fence_Release(fence);
 if(fev)CloseHandle(fev);if(dev)ID3D12Device_Release(dev);
 if(ngx_dll)FreeLibrary(ngx_dll);
 release_snippet_refs();
 dev=NULL;queue=NULL;alloc=NULL;cmd=NULL;fence=NULL;fev=NULL;ngx_dll=NULL;nv_attached=0;
 return 1;
}
