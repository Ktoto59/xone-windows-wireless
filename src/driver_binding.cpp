// SPDX-License-Identifier: GPL-2.0-or-later
// Select a driver already present in Windows' signed driver store for ONE instance.
#define UNICODE
#define _UNICODE
#include "winusb_transport.hpp"
#include <setupapi.h>
#include <newdev.h>
#include <iostream>
#include <filesystem>
#include <stdexcept>
#include <array>

namespace {
struct InfoSet { HDEVINFO value; ~InfoSet() { if (value!=INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(value); } };
bool administrator() {
    SID_IDENTIFIER_AUTHORITY authority=SECURITY_NT_AUTHORITY;
    PSID group=nullptr;
    if (!AllocateAndInitializeSid(&authority,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,0,0,0,0,0,0,&group)) return false;
    BOOL member=FALSE;
    CheckTokenMembership(nullptr,group,&member); FreeSid(group);
    return member!=FALSE;
}
void require(bool value,const char *message) {
    if (!value) throw std::runtime_error(std::string(message)+": "+xone::win_error(GetLastError()));
}
}
int wmain(int argc,wchar_t **argv) {
    SetConsoleOutputCP(CP_UTF8);
    try {
        if (argc!=8 || (std::wstring(argv[1])!=L"plan" && std::wstring(argv[1])!=L"install") ||
            std::wstring(argv[2])!=L"--instance" || std::wstring(argv[4])!=L"--inf" || std::wstring(argv[6])!=L"--section") {
            std::cout << "xone-bind plan|install --instance USB-instance --inf INBOX-OR-OEM-INF --section INSTALL-SECTION\n"
                "Only the external 045E:02FE adapter is supported by this development helper.\n"
                "plan builds a driver list in memory; install requires an administrator.\n";
            return argc==1 ? 0 : 2;
        }
        const bool install=std::wstring(argv[1])==L"install";
        const std::wstring id=argv[3],section=argv[7];
        if (id.find_first_of(L"\r\n")!=std::wstring::npos ||
            id.size()<22 || _wcsnicmp(id.c_str(),L"USB\\VID_045E&PID_02FE\\",22))
            throw std::runtime_error("Refusing an instance other than USB\\VID_045E&PID_02FE\\...");
        std::array<wchar_t,MAX_PATH> windows{};
        require(GetWindowsDirectoryW(windows.data(),static_cast<UINT>(windows.size()))!=0,"GetWindowsDirectory");
        const auto inf_root=std::filesystem::canonical(std::filesystem::path(windows.data())/L"INF");
        const auto inf=std::filesystem::canonical(argv[5]);
        if (_wcsicmp(inf.parent_path().c_str(),inf_root.c_str())) throw std::runtime_error("INF must already exist in Windows\\INF");
        if (install && !administrator()) throw std::runtime_error("Run this operation as administrator");
        InfoSet set{SetupDiCreateDeviceInfoList(nullptr,nullptr)};
        require(set.value!=INVALID_HANDLE_VALUE,"SetupDiCreateDeviceInfoList");
        SP_DEVINFO_DATA device{}; device.cbSize=sizeof(device);
        require(SetupDiOpenDeviceInfoW(set.value,id.c_str(),nullptr,0,&device)!=FALSE,"SetupDiOpenDeviceInfo");
        SP_DEVINSTALL_PARAMS_W params{}; params.cbSize=sizeof(params);
        require(SetupDiGetDeviceInstallParamsW(set.value,nullptr,&params)!=FALSE,"SetupDiGetDeviceInstallParams");
        params.Flags|=DI_ENUMSINGLEINF;
        params.FlagsEx|=DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
        require(wcscpy_s(params.DriverPath,inf.c_str())==0,"Copy INF path");
        require(SetupDiSetDeviceInstallParamsW(set.value,nullptr,&params)!=FALSE,"SetupDiSetDeviceInstallParams");
        require(SetupDiBuildDriverInfoList(set.value,nullptr,SPDIT_CLASSDRIVER)!=FALSE,"SetupDiBuildDriverInfoList");
        SP_DRVINFO_DATA_W selected{};
        bool found=false;
        for (DWORD i=0;;++i) {
            SP_DRVINFO_DATA_W candidate{}; candidate.cbSize=sizeof(candidate);
            if (!SetupDiEnumDriverInfoW(set.value,nullptr,SPDIT_CLASSDRIVER,i,&candidate)) {
                if (GetLastError()!=ERROR_NO_MORE_ITEMS) require(false,"SetupDiEnumDriverInfo");
                break;
            }
            DWORD size=0;
            SetupDiGetDriverInfoDetailW(set.value,nullptr,&candidate,nullptr,0,&size);
            if (size<sizeof(SP_DRVINFO_DETAIL_DATA_W)) continue;
            std::vector<BYTE> data(size);
            auto *detail=reinterpret_cast<SP_DRVINFO_DETAIL_DATA_W *>(data.data()); detail->cbSize=sizeof(*detail);
            require(SetupDiGetDriverInfoDetailW(set.value,nullptr,&candidate,detail,size,nullptr)!=FALSE,"SetupDiGetDriverInfoDetail");
            if (_wcsicmp(detail->SectionName,section.c_str())) continue;
            if (found) throw std::runtime_error("More than one matching driver; refusing ambiguous install");
            found=true; selected=candidate;
            std::cout << "Selected: " << xone::utf8(candidate.Description) << "\n  INF=" << xone::utf8(detail->InfFileName)
                << "\n  section=" << xone::utf8(detail->SectionName) << "\n  instance=" << xone::utf8(id) << '\n';
        }
        if (!found) throw std::runtime_error("No driver with the requested section found in this INF");
        if (!install) { std::cout << "PLAN ONLY: no device settings or driver binding changed.\n"; return 0; }
        // Inherit the global list to allow a signed inbox driver of a different
        // setup class, matching Device Manager's explicit manual selection.
        require(SetupDiSetSelectedDriverW(set.value,nullptr,&selected)!=FALSE,"Select global driver");
        require(SetupDiOpenDeviceInfoW(set.value,id.c_str(),nullptr,DIOD_INHERIT_CLASSDRVS,&device)!=FALSE,"Inherit class driver list");
        selected.cbSize=sizeof(selected);
        require(SetupDiGetSelectedDriverW(set.value,&device,&selected)!=FALSE,"Get inherited driver");
        BOOL reboot=FALSE;
        require(DiInstallDevice(nullptr,set.value,&device,&selected,0,&reboot)!=FALSE,"DiInstallDevice");
        std::cout << "Driver binding changed for the requested instance. Reboot-required=" << (reboot ? "yes" : "no") << '\n';
        return reboot ? 3010 : 0;
    } catch (const std::exception &error) { std::cerr << "Error: " << error.what() << '\n'; return 2; }
}
