// SPDX-License-Identifier: GPL-2.0-or-later
#include "diagnostic_log.hpp"
#include <iostream>
#ifndef XONE_DISABLE_DIAGNOSTICS
#error This regression test requires disabled runtime diagnostics
#endif
int main(int argc,char **argv){
    if(argc!=2)return 2;
    const auto path=std::filesystem::absolute(argv[1]);
    if(std::filesystem::exists(path))return 2;
    if(!SetEnvironmentVariableW(L"XONE_CPP_LOG_DIR",path.c_str()) || !SetEnvironmentVariableW(L"XONE_CPP_TRACE",L"1"))return 2;
    xone::initialize_diagnostics("release",path);
    xone::diagnostic("test_error",{{"win32_error",31}},true);
    if(xone::diagnostic_journal || xone::diagnostic_verbose || std::filesystem::exists(path)){
        std::cerr<<"Disabled release diagnostics created a writer, trace or directory\n";return 1;
    }
    std::cout<<"PASS release diagnostics stay disabled despite directory and packet-trace overrides\n";
    return 0;
}
