// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdio>
#include <cwchar>
inline LONG WINAPI xone_crash_trace(EXCEPTION_POINTERS *e) {
    // Launchers supply the checkout/install root as WorkingDirectory. Candidate
    // EXEs may be nested under outputs/direct-gip.
    wchar_t path[4096]{};const auto length=GetCurrentDirectoryW(4096,path);
    if(!length || length>=4000)return EXCEPTION_CONTINUE_SEARCH;
    wchar_t executable[MAX_PATH]{};GetModuleFileNameW(nullptr,executable,MAX_PATH);
    const auto *process_base=std::wcsrchr(executable,L'\\');process_base=process_base?process_base+1:executable;
    wchar_t suffix[MAX_PATH]{};swprintf_s(suffix,L"\\work\\%ls-crash-stack.txt",process_base);
    if(length+std::wcslen(suffix)>=4096)return EXCEPTION_CONTINUE_SEARCH;
    wcscat_s(path,suffix);
    FILE *file=nullptr;_wfopen_s(&file,path,L"w");if(!file)return EXCEPTION_CONTINUE_SEARCH;
    std::fprintf(file,"Exception=%08lx RIP=%llx\n",e->ExceptionRecord->ExceptionCode,e->ContextRecord->Rip);
    if(e->ExceptionRecord->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters>=2)
        std::fprintf(file,"access_kind=%llu target=%llx\n",static_cast<unsigned long long>(e->ExceptionRecord->ExceptionInformation[0]),static_cast<unsigned long long>(e->ExceptionRecord->ExceptionInformation[1]));
    CONTEXT context=*e->ContextRecord;
    for(unsigned n=0;n<20 && context.Rip;++n){
        DWORD64 image=0;auto entry=RtlLookupFunctionEntry(context.Rip,&image,nullptr);
        std::fprintf(file,"frame[%u] image=%llx offset=%llx\n",n,image,image?context.Rip-image:context.Rip);
        if(entry){void *data=nullptr;DWORD64 frame=0;RtlVirtualUnwind(UNW_FLAG_NHANDLER,image,context.Rip,entry,&context,&data,&frame,nullptr);}
        else {MEMORY_BASIC_INFORMATION info{};if(!VirtualQuery(reinterpret_cast<void *>(context.Rsp),&info,sizeof(info)) || info.State!=MEM_COMMIT)break;context.Rip=*reinterpret_cast<DWORD64 *>(context.Rsp);context.Rsp+=8;}
    }
    // Only module-relative code addresses; do not log packet/key buffers.
    const auto *stack=reinterpret_cast<const DWORD64 *>(e->ContextRecord->Rsp);
    MEMORY_BASIC_INFORMATION memory{};
    if(VirtualQuery(stack,&memory,sizeof(memory)) && memory.State==MEM_COMMIT){
        const auto limit=reinterpret_cast<uintptr_t>(memory.BaseAddress)+memory.RegionSize;
        for(unsigned n=0;n<128 && reinterpret_cast<uintptr_t>(stack+n+1)<=limit;++n){
            HMODULE module=nullptr;
            if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(stack[n]),&module)){
                wchar_t name[MAX_PATH]{};GetModuleFileNameW(module,name,MAX_PATH);
                const auto *base=std::wcsrchr(name,L'\\');std::fprintf(file,"stack[%u] %ls + %llx\n",n,base?base+1:name,stack[n]-reinterpret_cast<DWORD64>(module));
            }
        }
    }
    std::fclose(file);return EXCEPTION_CONTINUE_SEARCH;
}
