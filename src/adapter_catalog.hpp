// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string_view>
#include <initializer_list>
namespace xone {
inline bool supported_adapter_instance(std::wstring_view instance) {
    constexpr std::wstring_view prefix=L"USB\\VID_045E&PID_";
    constexpr size_t separator=prefix.size()+4;
    if(instance.size()<=separator+1 || instance[separator]!=L'\\')return false;
    auto upper=[](wchar_t c){return c>=L'a' && c<=L'z'?static_cast<wchar_t>(c-L'a'+L'A'):c;};
    for(size_t i=0;i<prefix.size();++i)if(upper(instance[i])!=prefix[i])return false;
    bool known=false;
    for(auto pid:{std::wstring_view(L"02FE"),std::wstring_view(L"02E6"),std::wstring_view(L"02F9"),std::wstring_view(L"091E")}) {
        bool equal=true;for(size_t i=0;i<4;++i)if(upper(instance[prefix.size()+i])!=pid[i])equal=false;
        known=known || equal;
    }
    if(!known)return false;
    for(auto c:instance.substr(separator+1))if(!((c>=L'0' && c<=L'9') || (upper(c)>=L'A' && upper(c)<=L'Z') || c==L'&' || c==L'_' || c==L'-'))return false;
    return true;
}
}
