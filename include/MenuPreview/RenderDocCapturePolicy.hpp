#pragma once
#include <cstdint>
#include <string>
namespace cvr::menu_preview {
struct CaptureRequestIdentity {uint32_t pid{},previewEpoch{};uint64_t creation{},expires{},key{};std::string run;uint32_t previewScreen{1};};
inline bool ValidCaptureRequest(const CaptureRequestIdentity& r,uint32_t pid,uint64_t creation,const std::string& run,uint64_t now){
    return r.pid==pid&&r.creation==creation&&!run.empty()&&r.run==run&&r.previewEpoch&&r.key&&now<r.expires&&(r.previewScreen==1||r.previewScreen==2);
}
inline bool ValidCaptureScope(const CaptureRequestIdentity& r,uint32_t screen,uint32_t epoch,uint64_t key){
    return (r.previewScreen==1||r.previewScreen==2)&&r.previewEpoch&&r.key&&screen==r.previewScreen&&epoch==r.previewEpoch&&key==r.key;
}
}
