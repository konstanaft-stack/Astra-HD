#include "Framegen/StatsOverlay.hpp"
#include "Framegen/NativeFgObservation.hpp"
#include "Framegen/GameGenerationOwner.hpp"
#include "Framegen/SrNativeApi.hpp"
#include "Framegen/StatsText.hpp"
#include "Framegen/Framegen.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>

namespace cvr::framegen {
namespace {
struct Canvas {
    uint32_t* pixels;int width,height;
    void Rect(int x,int y,int w,int h,uint32_t color) {
        for(int row=std::max(0,y);row<std::min(height,y+h);++row)
            if(x<width && x+w>0)std::fill(pixels+row*width+std::max(0,x),pixels+row*width+std::min(width,x+w),color);
    }
    void Text(int x,int y,const char* text,uint32_t color=0xFFF2F2F2u,int scale=2) {
    for(const char* p=text;*p && x+5*scale<=width;x+=6*scale) {
        const auto* glyph=stats_text::Glyph(stats_text::NextCodepoint(p));
        for(int col=0;col<5;++col)for(int row=0;row<7;++row)if(glyph[col]&(1u<<row))
            for(int dy=0;dy<scale;++dy)for(int dx=0;dx<scale;++dx)
                if(y+row*scale+dy>=0 && y+row*scale+dy<height && x+col*scale+dx>=0)pixels[(y+row*scale+dy)*width+x+col*scale+dx]=color;
    }
    }
    void TextFit(int y,const char* text,int scale=2) {
        if(stats_text::CodepointCount(text)*6*scale>size_t(width-24))scale=1;
        Text(12,y,text,0xFFF2F2F2u,scale);
    }
    void Panel() {
        for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
            const int dx=std::max({12-x,x-(width-13),0}),dy=std::max({12-y,y-(height-13),0});
            const bool corner=dx || dy;const int radius=dx*dx+dy*dy;
            pixels[y*width+x]=radius>144 ? 0 : ((corner && radius>100) || x<2 || y<2 || x>=width-2 || y>=height-2 ? 0xFFA4A4A4u : 0xDA21170Fu);
        }
    }
    void Graph(int x,int y,int w,int h,const std::array<float,GraphPoints>& values,double budget,uint32_t color) {
        Rect(x-1,y-1,w+2,h+2,0xFF686868);Rect(x,y,w,h,0xFF21170F);
        const double ceiling=std::clamp(std::max(33.34,budget*2),20.0,100.0);
        const int line=y+h-1-int(std::clamp(budget/ceiling,0.0,1.0)*(h-1));Rect(x,line,w,1,0xFF666666);
        for(unsigned i=0;i<GraphPoints;++i) {
            const int left=x+int(i*w/GraphPoints),right=x+int((i+1)*w/GraphPoints);
            const int bar=std::clamp(int(values[i]/ceiling*(h-1)),0,h-1);
            Rect(left,y+h-bar-1,std::max(1,right-left),bar+1,values[i]>budget*1.2 ? 0xFF526CFFu : color);
        }
    }
    void Cores(int x,int y,int w,int h,const HardwareStatistics& hardware) {
        if(!hardware.coreCount || hardware.coreMaximum<0) {Text(x,y,"--");return;}
        for(unsigned i=0;i<hardware.coreCount;++i) {
            const int left=x+int(i*w/hardware.coreCount),right=x+int((i+1)*w/hardware.coreCount);
            Rect(left,y,std::max(1,right-left-2),h,0xFF44382B);
            const int used=int(std::clamp(hardware.cores[i],0.0f,100.0f)*h/100);
            Rect(left,y+h-used,std::max(1,right-left-2),used,hardware.cores[i]>90?0xFF526CFFu:0xFF98EEA5u);
        }
    }
};
std::string Number(double value,bool known=true,unsigned precision=1) {
    if(!known || value<0 || !std::isfinite(value))return "--";
    char text[32]{};std::snprintf(text,sizeof(text),precision ? "%.1f" : "%.0f",value);return text;
}
void Draw(Canvas c,const Settings& settings,const Statistics& stats) {
    constexpr double gb=1024.0*1024*1024;const auto& hardware=stats.hardware;
    c.Panel();char row[128]{};
    const auto low1=Number(stats.low1Fps,stats.low1Fps>0),low01=Number(stats.low01Fps,stats.low01Fps>0);
    const auto gpu=Number(hardware.gpuUsage),cpu=Number(hardware.cpuUsage),temperature=Number(hardware.gpuTemperature,true,0);
    const auto ram=Number(hardware.ramUsed/gb,hardware.ramTotal!=0),ramTotal=Number(hardware.ramTotal/gb,hardware.ramTotal!=0);
    const auto vram=Number(hardware.gpuMemoryUsed/gb,hardware.gpuMemoryValid),vramTotal=Number(hardware.gpuMemoryTotal/gb,hardware.gpuMemoryTotal!=0);
    if(settings.overlayLayout==0) {
        SYSTEMTIME time{};GetLocalTime(&time);
        std::snprintf(row,sizeof(row),"cyberpunk-vr-port  %.1f МИН",stats.elapsedSeconds/60);c.Text(12,10,row);
        std::snprintf(row,sizeof(row),"%02u:%02u",time.wHour,time.wMinute);c.Text(c.width-76,10,row);
        std::snprintf(row,sizeof(row),"НАСТ. %.1f  СГЕН. %.1f  ВЫВОД %.1f",stats.realFps,stats.generatedFps,stats.outputFps);c.Text(12,36,row);
        std::snprintf(row,sizeof(row),"СР. %.1f FPS   МИН. %.1f   МАКС. %.1f",stats.averageFps,stats.minimumFps,stats.maximumFps);c.TextFit(60,row);
        std::snprintf(row,sizeof(row),"1%% НИЗ. %s  0.1%% НИЗ. %s  %dС",low1.c_str(),low01.c_str(),settings.historySeconds);c.Text(12,84,row);
        std::snprintf(row,sizeof(row),"GPU %.1fМС СР. %.1f",stats.gpuMs,stats.gpuAverageMs);c.Text(12,108,row);
        std::snprintf(row,sizeof(row),"CPU %.1fМС СР. %.1f",stats.cpuMs,stats.cpuAverageMs);c.Text(372,108,row);
        c.Graph(12,130,330,76,stats.gpuGraph,stats.budgetMs,0xFF98EEA5u);c.Graph(372,130,330,76,stats.cpuGraph,stats.budgetMs,0xFF98ECFFu);
        std::snprintf(row,sizeof(row),"P99 %s  P99.9 %s",Number(stats.gpuP99Ms,stats.gpuP99Ms>0).c_str(),Number(stats.gpuP999Ms,stats.gpuP999Ms>0).c_str());c.Text(12,214,row);
        std::snprintf(row,sizeof(row),"P99 %s  P99.9 %s",Number(stats.cpuP99Ms,stats.cpuP99Ms>0).c_str(),Number(stats.cpuP999Ms,stats.cpuP999Ms>0).c_str());c.Text(372,214,row);
        std::snprintf(row,sizeof(row),"GPU %s%%  %sC",gpu.c_str(),temperature.c_str());c.Text(12,238,row);
        std::snprintf(row,sizeof(row),"CPU %s%% МАКС. ЯДРО %s%%",cpu.c_str(),Number(hardware.coreMaximum,true,0).c_str());c.Text(372,238,row);
        std::snprintf(row,sizeof(row),"VRAM %s/%s ГБ",vram.c_str(),vramTotal.c_str());c.Text(12,262,row);
        std::snprintf(row,sizeof(row),"RAM %s/%s ГБ",ram.c_str(),ramTotal.c_str());c.Text(372,262,row);
        std::snprintf(row,sizeof(row),"VRAM ИГРЫ %s ГБ",Number(hardware.gpuProcessUsed/gb,hardware.gpuProcessMemoryValid).c_str());c.Text(12,286,row);
        std::snprintf(row,sizeof(row),"RAM ИГРЫ %s ГБ",Number(hardware.appWorkingSet/gb,hardware.fresh).c_str());c.Text(372,286,row);
        std::snprintf(row,sizeof(row),"ЯДРА %u",hardware.coreCount);c.Text(12,312,row);c.Cores(126,310,576,20,hardware);
        if(NativeFgPolicyObservationRequested()||settings.backend==Backend::OwnNvidia)
            std::snprintf(row,sizeof(row),"ГЕН. --МБ ВРЕМЯ --МС"); // Vendor allocation/timing is not measured here.
        else std::snprintf(row,sizeof(row),"ГЕН. %.0fМБ ВРЕМЯ %.1fМС",stats.vramBytes/(1024.0*1024),stats.generationMs);
        c.Text(12,340,row);
        std::snprintf(row,sizeof(row),"ПОВТОР. %.1f%% ПРОПУСК %llu",stats.repeatPercent,stats.skipped);c.Text(372,340,row);
    } else if(settings.overlayLayout==1) {
        std::snprintf(row,sizeof(row),"НАСТ. %.1f СГЕН. %.1f ВЫВОД %.1f",stats.realFps,stats.generatedFps,stats.outputFps);c.Text(12,10,row);
        std::snprintf(row,sizeof(row),"СР. %.1f МИН. %.1f МАКС. %.1f",stats.averageFps,stats.minimumFps,stats.maximumFps);c.TextFit(34,row);
        std::snprintf(row,sizeof(row),"1%% НИЗ. %s 0.1%% %s %dС",low1.c_str(),low01.c_str(),settings.historySeconds);c.TextFit(58,row);
        std::snprintf(row,sizeof(row),"GPU %s%% %sC %.1fМС",gpu.c_str(),temperature.c_str(),stats.gpuMs);c.Text(12,84,row);
        c.Graph(12,106,396,58,stats.gpuGraph,stats.budgetMs,0xFF98EEA5u);
        std::snprintf(row,sizeof(row),"CPU %s%% %.1fМС",cpu.c_str(),stats.cpuMs);c.Text(12,174,row);
        c.Graph(12,196,396,58,stats.cpuGraph,stats.budgetMs,0xFF98ECFFu);
        std::snprintf(row,sizeof(row),"VRAM %s/%s ГБ",vram.c_str(),vramTotal.c_str());c.Text(12,266,row);
        std::snprintf(row,sizeof(row),"RAM %s/%s ГБ",ram.c_str(),ramTotal.c_str());c.Text(12,290,row);
        c.Cores(12,312,396,12,hardware);
    } else {
        std::snprintf(row,sizeof(row),"НАСТ. %.1f СГЕН. %.1f",stats.realFps,stats.generatedFps);c.Text(12,10,row);
        std::snprintf(row,sizeof(row),"СР. %.1f МИН. %.1f МАКС. %.1f",stats.averageFps,stats.minimumFps,stats.maximumFps);c.TextFit(34,row);
        std::snprintf(row,sizeof(row),"ВЫВОД %.1f FPS",stats.outputFps);c.Text(12,58,row);
        std::snprintf(row,sizeof(row),"1%% %s  0.1%% %s",low1.c_str(),low01.c_str());c.Text(12,82,row);
        std::snprintf(row,sizeof(row),"GPU %s%% CPU %s%%",gpu.c_str(),cpu.c_str());c.Text(12,106,row);
        std::snprintf(row,sizeof(row),"GPU %.1fМС CPU %.1fМС",stats.gpuMs,stats.cpuMs);c.Text(12,130,row);
    }
    const bool detailed=settings.overlayLayout==0;
    const int footer=detailed?378:(settings.overlayLayout==1?344:164);
    c.Rect(12,footer-10,c.width-24,1,0xFF686868);
    std::snprintf(row,sizeof(row),"CPU: %s",hardware.cpuName[0]?hardware.cpuName:"--");c.TextFit(footer,row,detailed?2:1);
    std::snprintf(row,sizeof(row),"GPU: %s",hardware.gpuName[0]?hardware.gpuName:"--");c.TextFit(footer+(detailed?24:14),row,detailed?2:1);
    const char* generator=settings.backend==Backend::OwnNvidia?"Наш NVIDIA":(settings.backend==Backend::Nvidia?"NVIDIA OFA + FidelityFX":"FidelityFX");
    const bool native=NativeFgPolicyObservationRequested();
    char nativeLabel[96]{};
    const auto choice=ReadGameGenerationChoice();
    if(!stats.nativePolicy.known){
        if(choice.known&&choice.provider==GameGenerationProvider::Nvidia&&choice.multiplier)
            std::snprintf(nativeLabel,sizeof(nativeLabel),"NVIDIA: %s; ЗАПР. X%u",stats.nativeGenerationActive?"G ИДУТ":"ЖДЁТ G",choice.multiplier);
        else if(stats.nativeGenerationActive)std::snprintf(nativeLabel,sizeof(nativeLabel),"NVIDIA: G ИДУТ; РЕЖИМ ?");
        else if(choice.known&&choice.provider==GameGenerationProvider::Off&&!NativeFgGameWasEnabled())std::snprintf(nativeLabel,sizeof(nativeLabel),"ВЫКЛЮЧЕНО");
        else std::snprintf(nativeLabel,sizeof(nativeLabel),"NVIDIA: НЕИЗВЕСТНО");
    }
    else if(!stats.nativePolicy.enabled)std::snprintf(nativeLabel,sizeof(nativeLabel),"ВЫКЛЮЧЕНО");
    else std::snprintf(nativeLabel,sizeof(nativeLabel),"NVIDIA X%u%s",stats.nativePolicy.multiplier,
        stats.nativeGenerationActive ? "" : ": ЖДЁТ G");
    char ownLabel[96]{};
    std::snprintf(ownLabel,sizeof(ownLabel),"%s X%u%s",generator,
        settings.backend==Backend::OwnNvidia?unsigned(settings.ownGeneratedFrames+1):2u,stats.active?"":": ЖДЁТ G");
    const char* label=native ? nativeLabel : settings.enabled?ownLabel:"ВЫКЛЮЧЕНО";
    const char* camera=stats.cameraMode==PresentedCamera::Paired ? "2 КАМ." :
        stats.cameraMode==PresentedCamera::Alternating ? "ЧЕРЕДОВАНИЕ" : "КАМ. ?";
    std::snprintf(row,sizeof(row),"ГЕН.: %s | %s",label,camera);c.TextFit(footer+(detailed?48:32),row);
}
}
const XrCompositionLayerBaseHeader* StatsOverlay::Prepare(XrSession session,XrSpace view,ID3D12Device* device,ID3D12CommandQueue* queue,
    XrSpace local,const XrPosef* head,uint64_t origin,XrTime displayTime) {
    const auto settings=GetSettings();
    if(!settings.overlay) {
        if(!fence || fence->GetCompletedValue()>=fenceValue)Shutdown();
        return nullptr;
    }
    if(!device || !queue || !session || !view)return nullptr;
    ConfigureHardwareTelemetry(device);
    const uint32_t wantedWidth=settings.overlayLayout==0 ? 720u : (settings.overlayLayout==1 ? 420u : 360u);
    const uint32_t wantedHeight=settings.overlayLayout==0 ? 452u : (settings.overlayLayout==1 ? 404u : 222u);
    if(swapchain && (width!=wantedWidth || height!=wantedHeight) && fence->GetCompletedValue()>=fenceValue)Shutdown();
    if(!swapchain) {
        width=wantedWidth;height=wantedHeight;rowPitch=(width*4+255)&~255u;pixels.resize(size_t(width)*height);
        uint32_t count{};if(XR_FAILED(xrEnumerateSwapchainFormats(session,0,&count,nullptr)))return nullptr;
        std::vector<int64_t> formats(count);if(XR_FAILED(xrEnumerateSwapchainFormats(session,count,&count,formats.data())))return nullptr;
        if(std::find(formats.begin(),formats.end(),DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)==formats.end())return nullptr;
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};info.usageFlags=XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT|XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        info.format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;info.sampleCount=1;info.width=width;info.height=height;info.faceCount=1;info.arraySize=1;info.mipCount=1;
        if(XR_FAILED(xrCreateSwapchain(session,&info,&swapchain)))return nullptr;
        if(XR_FAILED(xrEnumerateSwapchainImages(swapchain,0,&count,nullptr))) {Shutdown();return nullptr;}
        images.assign(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        if(XR_FAILED(xrEnumerateSwapchainImages(swapchain,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()))) ||
           FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))) ||
           FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))) ||
           FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)))) {Shutdown();return nullptr;}
        list->Close();D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=rowPitch*height;
        desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;
        if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)))) {Shutdown();return nullptr;}
    }
    const auto now=NowMs();
    if((now>=nextUpdate || acquired) && fence->GetCompletedValue()>=fenceValue) {
        if(!acquired) {XrSwapchainImageAcquireInfo info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};if(XR_SUCCEEDED(xrAcquireSwapchainImage(swapchain,&info,&acquiredIndex)))acquired=true;}
        if(acquired) {
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=0;
            if(xrWaitSwapchainImage(swapchain,&wait)==XR_SUCCESS) {
                const auto stats=GetStatistics();void* data{};D3D12_RANGE none{};
                if(SUCCEEDED(upload->Map(0,&none,&data))) {
                    Draw({pixels.data(),int(width),int(height)},settings,stats);
                    for(uint32_t row=0;row<height;++row)std::memcpy(static_cast<uint8_t*>(data)+size_t(row)*rowPitch,pixels.data()+size_t(row)*width,size_t(width)*4);
                    upload->Unmap(0,nullptr);
                    if(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(),nullptr))) {
                        auto* target=images[acquiredIndex].texture;
                        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        b.Transition={target,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST};list->ResourceBarrier(1,&b);
                        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        src.PlacedFootprint.Footprint={target->GetDesc().Format,width,height,1,rowPitch};dst.pResource=target;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);
                        if(SUCCEEDED(list->Close())) {ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);queue->Signal(fence.Get(),++fenceValue);published=true;nextUpdate=now+250;}
                    }
                }
                XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                if(XR_FAILED(xrReleaseSwapchainImage(swapchain,&release)))published=false;
                acquired=false;
            }
        }
    }
    if(!published)return nullptr;
    layer.space=view;layer.eyeVisibility=XR_EYE_VISIBILITY_BOTH;layer.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.subImage={swapchain,{{0,0},{int32_t(width),int32_t(height)}},0};
    constexpr float positions[9][2]={{-18,18},{18,18},{-18,-18},{18,-18},{0,18},{0,-18},{-18,0},{18,0},{0,0}};
    constexpr float radians=.01745329252f;
    const float distance=settings.overlayDistance;
    const auto position=positions[std::clamp(settings.overlayCorner,0,8)];
    const float x=std::tan(std::clamp(position[0]+settings.overlayX,-70.0f,70.0f)*radians)*distance;
    const float y=std::tan(std::clamp(position[1]+settings.overlayY,-60.0f,60.0f)*radians)*distance;
    layer.pose={{0,0,0,1},{x,y,-distance}};
    if(originSerial!=origin || followMode!=settings.overlayFollow) {follow.Reset();clock.Reset();originSerial=origin;}
    if(settings.overlayFollow && local && head) {
        const auto& q=head->orientation;
        const float yaw=follow.Update(cvr::hud::HeadYaw(q.x,q.y,q.z,q.w,follow.yaw),settings.overlayCone*radians,clock.Step(displayTime));
        anchor.orientation={0,std::sin(yaw*.5f),0,std::cos(yaw*.5f)};
        anchor.position={head->position.x+std::cos(yaw)*x-std::sin(yaw)*distance,head->position.y+y,
            head->position.z-std::sin(yaw)*x-std::cos(yaw)*distance};
        layer.pose=anchor;layer.space=local;
    } else if(settings.overlayFollow && local) {
        if(!follow.valid)return nullptr;layer.pose=anchor;layer.space=local;
    }
    followMode=settings.overlayFollow;
    const float angle=settings.overlayLayout==0 ? 36.0f : (settings.overlayLayout==1 ? 25.0f : 22.0f);
    const float w=2*distance*std::tan(angle*radians*.5f)*settings.overlayScale;
    layer.size={w,w*height/width};return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
}
void StatsOverlay::Shutdown(bool wait) {
    if(!swapchain && !fence && pixels.empty())return;
    if(wait && fence && fence->GetCompletedValue()<fenceValue) {
        const auto event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(event) {if(SUCCEEDED(fence->SetEventOnCompletion(fenceValue,event)))WaitForSingleObject(event,1000);CloseHandle(event);}
    }
    if(fence && fence->GetCompletedValue()<fenceValue)return;
    if(acquired && swapchain) {
        XrSwapchainImageWaitInfo ready{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};ready.timeout=0;
        if(xrWaitSwapchainImage(swapchain,&ready)!=XR_SUCCESS)return;
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if(XR_FAILED(xrReleaseSwapchainImage(swapchain,&release)))return;
        acquired=false;
    }
    if(swapchain) {xrDestroySwapchain(swapchain);swapchain=XR_NULL_HANDLE;}
    std::vector<XrSwapchainImageD3D12KHR>().swap(images);upload.Reset();allocator.Reset();list.Reset();fence.Reset();
    fenceValue=0;nextUpdate=0;acquired=published=false;
    std::vector<uint32_t>().swap(pixels);width=height=rowPitch=0;follow.Reset();clock.Reset();followMode=-1;
}
}
