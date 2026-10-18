#include "Overlay/LauncherDialog.hpp"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
HWND g_gameHwnd=nullptr;
static int width=2064,height=2296,hmd=2,runtimeMode=0,debug=0;
void Log(const char* format,...){
    FILE* file=fopen("launcher-preview.log","a");if(!file)return;
    va_list args;va_start(args,format);vfprintf(file,format,args);va_end(args);fclose(file);
}
extern "C" int GetCurrentWindowWidth(){return width;}
extern "C" int GetCurrentWindowHeight(){return height;}
extern "C" int GetCurrentHmdType(){return hmd;}
extern "C" void SetHmdTypeAndPersist(int value){hmd=value;}
extern "C" int GetXrRuntimeMode(){return runtimeMode;}
extern "C" void SetRuntimeModeAndPersist(int value){runtimeMode=value;}
extern "C" int GetLauncherDebug(){return debug;}
extern "C" void SetLauncherDebugAndPersist(int value){debug=value;}
extern "C" float GetForcedFov(){return 0;}
extern "C" void SetWindowResolutionAndPersist(int w,int h){
    width=w;height=h;
    FILE* file=fopen("launcher-choice.json","w");if(!file)return;
    fprintf(file,"{\"width\":%d,\"height\":%d,\"hmd\":%d,\"runtime\":%d,\"debug\":%d,\"previewOnly\":true}\n",w,h,hmd,runtimeMode,debug);
    fclose(file);
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){
    wchar_t path[MAX_PATH]{};GetModuleFileNameW(nullptr,path,MAX_PATH);
    if(auto* slash=wcsrchr(path,L'\\')){*slash=0;SetCurrentDirectoryW(path);}
    ShowLauncherDialog();return 0;
}
