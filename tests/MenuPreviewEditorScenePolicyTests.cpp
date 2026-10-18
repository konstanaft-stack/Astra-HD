#include "MenuPreview/EditorScenePolicy.hpp"
#include <cstdio>
using namespace cvr::menu_preview;
int main(){int failed{};auto check=[&](bool actual,bool expected,const char* name){if(actual!=expected){std::printf("FAIL %s\n",name);++failed;}};
 check(EditorSceneRecordingSubmissionValid(false,true,11,11),true,"actual successful Close");
 check(EditorSceneRecordingSubmissionValid(false,false,11,11),false,"failed Close is not successful execution evidence");
 check(EditorSceneRecordingSubmissionValid(true,false,12,12),false,"Reset opens a fresh unclosed recording");
 check(EditorSceneRecordingSubmissionValid(false,true,12,11),false,"prepared generation recycled before Submitted");
 check(EditorSceneRecordingSubmissionValid(false,true,0,0),false,"unknown native generation");
 std::printf("raw scene submission contract: %d failures\n",failed);return failed?1:0;}
