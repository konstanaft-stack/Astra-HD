#define main aer_native_input_fixture_main
#include "aer_native_input.cpp"
#undef main
#include "../../../../native/tests/worker_fixture_faults.hpp"
#include <chrono>
#include <filesystem>
#include <thread>

template<class Predicate>static void WaitFor(Predicate predicate,const char* message){
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!predicate()&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Check(predicate(),message);
}
static void OverwriteBorrowedColor(Context& c,ID3D12Resource* color){
    const auto desc=color->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows{};UINT64 rowBytes{},bytes{};
    c.device->GetCopyableFootprints(&desc,0,1,0,&fp,&rows,&rowBytes,&bytes);auto upload=c.Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD);
    void* mapped{};D3D12_RANGE empty{};Check(SUCCEEDED(upload->Map(0,&empty,&mapped)),"borrowed overwrite map failed");
    for(unsigned y=0;y<desc.Height;++y)for(unsigned x=0;x<desc.Width;++x){
        auto* pixel=static_cast<unsigned char*>(mapped)+fp.Offset+y*fp.Footprint.RowPitch+x*4;
        pixel[0]=204;pixel[1]=pixel[2]=0;pixel[3]=255;
    }
    upload->Unmap(0,nullptr);Commands commands(c.device.Get());Barrier(commands.list.Get(),color,CaptureColorReadState,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
    dst.pResource=color;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;commands.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    Barrier(commands.list.Get(),color,D3D12_RESOURCE_STATE_COPY_DEST,CaptureColorReadState);c.Submit(commands);
}
int main(int argc,char** argv){
    try{
        const bool signalFault=argc>1&&std::string_view(argv[1])=="signal";Context c(true);
        Capture left(c,0,7),right(c,1,8);auto a=left.Adapt(),b=right.Adapt({},a);
        auto previous=AlternatingMainGuideHistory(a),middle=AlternatingMainGuideHistory(b);
        std::weak_ptr<ReadLease> leftLease=left.frame.lease,rightLease=right.frame.lease;
        const auto backendPath=std::filesystem::absolute("worker_copy_backend.dll").wstring();
        HMODULE backend=LoadLibraryExW(backendPath.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Check(backend!=nullptr,"test backend not loaded");
        const auto setBorrowed=reinterpret_cast<void(*)(ID3D12Resource*)>(GetProcAddress(backend,"fixture_borrowed_source"));
        const auto verified=reinterpret_cast<int(*)()>(GetProcAddress(backend,"fixture_verified_private_color"));
        const auto runCount=reinterpret_cast<int(*)()>(GetProcAddress(backend,"fixture_run_count"));
        Check(setBorrowed&&verified&&runCount,"test backend inspection exports missing");setBorrowed(left.color.Get());
        worker_faults::Hooks faults(c.device.Get(),signalFault?worker_faults::Kind::Signal:worker_faults::Kind::None);
        auto worker=std::make_unique<cpvr::GenerationWorker>();cpvr::GenerationConfig config;config.backend_library=backendPath;
        config.snippet_directory=L"WARP_FIXTURE_ONLY";config.policy.generated_per_interval=1;config.policy.cadence=cpvr::Cadence::AlternateEyes;
        Check(worker->start(c.device.Get(),config,{}),"actual worker thread did not start");
        WaitFor([&]{return worker->status().backend_attached||worker->status().failed;},"worker backend startup timed out");
        Check(!worker->status().failed&&worker->submit(a.input),"first actual worker input rejected");a={};left.frame={};
        WaitFor([&]{return worker->status().timings[1].private_snapshot.count==1&&!worker->status().queued_inputs;},"first private copy did not complete");
        Check(!worker->status().failed&&faults.executions==1,"first copy did not execute successfully");
        if(signalFault)faults.arm();
        Check(worker->submit(b.input),"second actual worker input rejected");b={};right.frame={};
        WaitFor([&]{return worker->status().failed||(worker->status().timings[1].private_snapshot.count==2&&!worker->status().queued_inputs);},"second private copy did not complete/fail");
        // Exercise production expiry, without a history reset hiding retained
        // Snapshot.input colors. Both warm real products are older than150ms.
        worker->select(1'200'000'000,11'111'111);
        if(signalFault){
            Check(worker->status().failed&&faults.failures==1,"native Signal failure was not injected");
            worker->stop();Check(!rightLease.expired(),"unproven copy Signal released borrowed source");
            faults.drain(c.device.Get());worker.reset();
            Check(!rightLease.expired(),"unknown-completion teardown released retained source");
            std::cout<<"PASS actual worker broken Signal preserves unsafe source; isolated queue drained\n";
        }else{
            Check(!worker->status().failed,"normal private copy failed");
            WaitFor([&]{return leftLease.expired();},"completed old private snapshot still pins borrowed color");
            Check(!rightLease.expired(),"current replayable recording lost its required source lease");
            OverwriteBorrowedColor(c,left.color.Get());
            Capture current(c,2,9);current.input->contentTimeNs=1'210'000'000;current.input->posePredictionNs=1'230'000'000;
            auto& stamp=current.frame.nativeStamp.eyes[0];stamp.contentTimeNs=current.input->contentTimeNs;stamp.posePredictionNs=current.input->posePredictionNs;
            stamp.mainEyeSource.requestedPoseTimeNs=current.input->posePredictionNs;
            auto d=current.Adapt(previous,middle);Check(d.history==NativeAlternatingHistory::SameEyeDelta2,"post-retirement native span lost metadata");
            Check(worker->submit(d.input),"third actual worker input rejected");d={};current.frame={};
            WaitFor([&]{return runCount()>0||worker->status().failed;},"existing backend path did not consume retired private history");
            WaitFor([&]{return !worker->status().evaluating;},"fixture backend did not finish");
            Check(verified()==1&&!worker->status().failed,"backend lost private color/metadata after borrowed source overwrite");
            worker->stop();worker.reset();
            std::cout<<"PASS actual worker completed-copy retirement; source overwritten; old private GPU pixels preserved\n";
        }
        c.CheckDebug();FreeLibrary(backend);return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}
}
