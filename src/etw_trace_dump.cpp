// SPDX-License-Identifier: GPL-2.0-or-later
// Read-only raw event export for local stock-driver diagnostics.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <iostream>
#include <iomanip>
#include <cstdio>
static void WINAPI event_record(EVENT_RECORD *event){
    const auto &h=event->EventHeader;const auto &g=h.ProviderId;
    char guid[40]{};
    sprintf_s(guid,"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",g.Data1,g.Data2,g.Data3,g.Data4[0],g.Data4[1],g.Data4[2],g.Data4[3],g.Data4[4],g.Data4[5],g.Data4[6],g.Data4[7]);
    std::cout<<"{\"Time100ns\":"<<h.TimeStamp.QuadPart<<",\"Guid\":\""<<guid<<"\",\"Id\":"<<h.EventDescriptor.Id<<",\"Flags\":"<<h.Flags<<",\"Pid\":"<<h.ProcessId<<",\"Tid\":"<<h.ThreadId<<",\"Data\":\"";
    const auto *data=static_cast<const unsigned char *>(event->UserData);
    for(unsigned n=0;n<event->UserDataLength;++n)std::cout<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(data[n]);
    std::cout<<std::dec<<"\"}\n";
}
int wmain(int argc,wchar_t **argv){
    if(argc!=2){std::cerr<<"Usage: etw-trace-dump.exe FILE.etl\n";return 1;}
    EVENT_TRACE_LOGFILEW log{};log.LogFileName=argv[1];log.ProcessTraceMode=PROCESS_TRACE_MODE_EVENT_RECORD;log.EventRecordCallback=event_record;
    auto trace=OpenTraceW(&log);if(trace==INVALID_PROCESSTRACE_HANDLE){std::cerr<<"OpenTrace error="<<GetLastError()<<'\n';return 1;}
    const auto result=ProcessTrace(&trace,1,nullptr,nullptr);CloseTrace(trace);
    if(result!=ERROR_SUCCESS){std::cerr<<"ProcessTrace error="<<result<<'\n';return 1;}return 0;
}
