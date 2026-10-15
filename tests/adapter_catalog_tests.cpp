// SPDX-License-Identifier: GPL-2.0-or-later
#include "adapter_catalog.hpp"
#include <iostream>
#include <string>
int main(){
    for(auto pid:{L"02FE",L"02E6",L"02F9",L"091E"}) {
        if(!xone::supported_adapter_instance(std::wstring(L"USB\\VID_045E&PID_")+pid+L"\\EXAMPLE-1&2"))return 1;
    }
    if(!xone::supported_adapter_instance(L"usb\\vid_045e&pid_02fe\\example"))return 2;
    for(auto id:{L"",L"USB\\VID_045E&PID_02FE\\",L"USB\\VID_045E&PID_02FE&MI_00\\EXAMPLE",L"USB\\VID_045E&PID_0000\\EXAMPLE",L"USB\\VID_0000&PID_02FE\\EXAMPLE",L"USB\\VID_045E&PID_02FE\\EXAMPLE\\CHILD",L"USB\\VID_045E&PID_02FE\\EXAMPLE\n",L"USB\\VID_045E&PID_02FE\\EXAMPLE\""})if(xone::supported_adapter_instance(id))return 3;
    std::cout<<"PASS four adapter models, case folding and malformed-instance rejection\n";
}
