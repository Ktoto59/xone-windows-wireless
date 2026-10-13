// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <mmdeviceapi.h>
#include <devicetopology.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl.h>
#include <array>
#include <cwctype>
#include <functional>
#include "appliance_config.hpp"
namespace xone {
// Windows' desktop policy interface (also used by EarTrumpet). No DLL hooks.
struct __declspec(uuid("F8679F50-850A-41CF-9C72-430F290290C8")) AudioPolicy: IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Unused1()=0;virtual HRESULT STDMETHODCALLTYPE Unused2()=0;
    virtual HRESULT STDMETHODCALLTYPE Unused3()=0;virtual HRESULT STDMETHODCALLTYPE Unused4()=0;
    virtual HRESULT STDMETHODCALLTYPE Unused5()=0;virtual HRESULT STDMETHODCALLTYPE Unused6()=0;
    virtual HRESULT STDMETHODCALLTYPE Unused7()=0;virtual HRESULT STDMETHODCALLTYPE Unused8()=0;
    virtual HRESULT STDMETHODCALLTYPE GetProperty(PCWSTR,const PROPERTYKEY &,PROPVARIANT *)=0;
    virtual HRESULT STDMETHODCALLTYPE SetProperty(PCWSTR,const PROPERTYKEY &,const PROPVARIANT *)=0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR,ERole)=0;
    virtual HRESULT STDMETHODCALLTYPE SetVisibility(PCWSTR,BOOL)=0;
};
inline std::wstring wide(std::string_view value){
    if(value.empty())return {};const auto n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);
    if(!n)throw std::runtime_error("Invalid UTF-8 value");std::wstring result(n,0);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),n);return result;
}
inline std::string narrow(std::wstring_view value){
    if(value.empty())return {};const auto n=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    std::string result(n,0);WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),n,nullptr,nullptr);return result;
}
class ApplianceAudio {
    template<class T>using ComPtr=Microsoft::WRL::ComPtr<T>;
    ComPtr<IMMDeviceEnumerator> enumerator_;ComPtr<AudioPolicy> policy_;
    Json saved_=Json::array();std::array<std::wstring,2> selected_{};
    bool applied_=false;
    std::function<void(const Json &)> persist_;
    std::wstring current(EDataFlow flow,ERole role){ComPtr<IMMDevice> d;if(FAILED(enumerator_->GetDefaultAudioEndpoint(flow,role,&d)))return {};LPWSTR id=nullptr;if(FAILED(d->GetId(&id)))return {};std::wstring out=id;CoTaskMemFree(id);return out;}
    bool active(const std::wstring &id){ComPtr<IMMDevice> d;DWORD state=0;return !id.empty() && SUCCEEDED(enumerator_->GetDevice(id.c_str(),&d)) && SUCCEEDED(d->GetState(&state)) && (state&DEVICE_STATE_ACTIVE);}
    std::wstring find(uint16_t pid,EDataFlow flow){
        if(!enumerator_)return {};ComPtr<IMMDeviceCollection> list;if(FAILED(enumerator_->EnumAudioEndpoints(flow,DEVICE_STATE_ACTIVE,&list)))return {};UINT count=0;list->GetCount(&count);
        wchar_t wanted[32]{};swprintf_s(wanted,L"vid_045e&pid_%04x",pid);
        for(UINT n=0;n<count;++n){
            ComPtr<IMMDevice> d;ComPtr<IDeviceTopology> topology;ComPtr<IConnector> connector;LPWSTR adapter_id=nullptr;
            if(FAILED(list->Item(n,&d)) || FAILED(d->Activate(__uuidof(IDeviceTopology),CLSCTX_ALL,nullptr,&topology)) || FAILED(topology->GetConnector(0,&connector)) || FAILED(connector->GetDeviceIdConnectedTo(&adapter_id)))continue;
            std::wstring hardware=adapter_id;ComPtr<IMMDevice> adapter;ComPtr<IPropertyStore> properties;
            if(SUCCEEDED(enumerator_->GetDevice(adapter_id,&adapter)) && SUCCEEDED(adapter->OpenPropertyStore(STGM_READ,&properties))){PROPVARIANT v{};if(SUCCEEDED(properties->GetValue(PKEY_Device_InstanceId,&v)) && v.vt==VT_LPWSTR && v.pwszVal)hardware=v.pwszVal;PropVariantClear(&v);}
            CoTaskMemFree(adapter_id);std::transform(hardware.begin(),hardware.end(),hardware.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});
            if(hardware.find(wanted)==hardware.npos)continue;LPWSTR id=nullptr;if(FAILED(d->GetId(&id)))continue;std::wstring result=id;CoTaskMemFree(id);return result;
        }return {};
    }
public:
    ApplianceAudio(Json saved,std::function<void(const Json &)> persist):persist_(std::move(persist)){
        if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator_))))return;
        constexpr GUID clsid{0x870af99c,0x171d,0x4f9e,{0xaf,0x0d,0xe6,0x3d,0xf4,0x0c,0x2b,0xc9}};
        CoCreateInstance(clsid,nullptr,CLSCTX_ALL,__uuidof(AudioPolicy),&policy_);
        if(saved.is_object()){saved_=saved.value("Previous",Json::array());auto chosen=saved.value("Selected",Json::array());if(chosen.size()==2)for(size_t n=0;n<2;++n)selected_[n]=wide(chosen[n].get<std::string>());}
    }
    bool select(uint16_t pid){
        if(!policy_)return false;const auto render=find(pid,eRender),capture=find(pid,eCapture);if(render.empty() || capture.empty())return false;
        if(selected_[0]==render && selected_[1]==capture && applied_)return true;
        if(selected_[0]!=render || selected_[1]!=capture){
            restore();saved_=Json::array();selected_={render,capture};
            for(int flow=0;flow<2;++flow)for(int role=0;role<3;++role){const auto before=current(static_cast<EDataFlow>(flow),static_cast<ERole>(role));if(before!=selected_[flow] && !before.empty())saved_.push_back({{"Flow",flow},{"Role",role},{"Id",narrow(before)}});}
            persist_({{"Previous",saved_},{"Selected",{narrow(render),narrow(capture)}}});
        }
        bool ok=true;for(int flow=0;flow<2;++flow)for(int role=0;role<3;++role)ok=SUCCEEDED(policy_->SetDefaultEndpoint(selected_[flow].c_str(),static_cast<ERole>(role))) && ok;
        applied_=ok;return ok;
    }
    void restore(){
        if(!policy_ || !enumerator_)return;
        for(const auto &row:saved_){const int flow=row.value("Flow",-1),role=row.value("Role",-1);if(flow<0 || flow>1 || role<0 || role>2)continue;const auto id=wide(row.value("Id",std::string{}));const auto now=current(static_cast<EDataFlow>(flow),static_cast<ERole>(role));if((now==selected_[flow] || now.empty()) && active(id))policy_->SetDefaultEndpoint(id.c_str(),static_cast<ERole>(role));}
        selected_={};saved_=Json::array();applied_=false;persist_({{"Previous",saved_},{"Selected",{"",""}}});
    }
};
}
