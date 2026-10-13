// SPDX-License-Identifier: GPL-2.0-or-later
// ViGEm IOCTL protocol follows ViGEmClient (MIT); see third_party/vigem/LICENSE.
#include "winusb_transport.hpp"
#include <setupapi.h>
#include <initguid.h>
#include <winioctl.h>
#include <ViGEm/km/BusShared.h>
#include "input_output.hpp"
#include <thread>
#include <atomic>
#include <array>
#include <iostream>
#include <stdexcept>
#include <mutex>
namespace xone {
namespace {
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~Handle(){if(value && value!=INVALID_HANDLE_VALUE) CloseHandle(value);}
};
DWORD ioctl(HANDLE bus,DWORD code,void *input,DWORD size,void *output=nullptr,DWORD out_size=0,DWORD timeout=2000) {
    Handle event{CreateEventW(nullptr,TRUE,FALSE,nullptr)}; if(!event.value)return GetLastError();
    OVERLAPPED op{};op.hEvent=event.value;DWORD transferred=0;
    if(DeviceIoControl(bus,code,input,size,output,out_size,&transferred,&op))return ERROR_SUCCESS;
    auto error=GetLastError();if(error!=ERROR_IO_PENDING)return error;
    const auto wait=WaitForSingleObject(event.value,timeout);
    if(wait!=WAIT_OBJECT_0) {
        error=wait==WAIT_TIMEOUT?ERROR_TIMEOUT:GetLastError();CancelIoEx(bus,&op);GetOverlappedResult(bus,&op,&transferred,TRUE);return error;
    }
    return GetOverlappedResult(bus,&op,&transferred,FALSE)?ERROR_SUCCESS:GetLastError();
}
void checked(DWORD error,const char *operation) {if(error)throw std::runtime_error(std::string(operation)+": "+win_error(error));}
HANDLE open_bus() {
    const auto set=SetupDiGetClassDevsW(&GUID_DEVINTERFACE_BUSENUM_VIGEM,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if(set==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot enumerate ViGEmBus");
    struct Cleanup {HDEVINFO set;~Cleanup(){SetupDiDestroyDeviceInfoList(set);}} cleanup{set};
    for(DWORD i=0;;++i) {
        SP_DEVICE_INTERFACE_DATA iface{};iface.cbSize=sizeof(iface);
        if(!SetupDiEnumDeviceInterfaces(set,nullptr,&GUID_DEVINTERFACE_BUSENUM_VIGEM,i,&iface))break;
        DWORD n=0;SetupDiGetDeviceInterfaceDetailW(set,&iface,nullptr,0,&n,nullptr);
        if(n<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))continue;
        std::vector<uint8_t> storage(n);auto *detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());detail->cbSize=sizeof(*detail);
        if(!SetupDiGetDeviceInterfaceDetailW(set,&iface,detail,n,nullptr,nullptr))continue;
        const auto bus=CreateFileW(detail->DevicePath,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
        if(bus==INVALID_HANDLE_VALUE)continue;
        VIGEM_CHECK_VERSION version{};VIGEM_CHECK_VERSION_INIT(&version,VIGEM_COMMON_VERSION);
        if(!ioctl(bus,IOCTL_VIGEM_CHECK_VERSION,&version,sizeof(version)))return bus;
        CloseHandle(bus);
    }
    throw std::runtime_error("Compatible ViGEmBus unavailable. Install the verified bus driver before --virtual-xinput.");
}
}
struct VigemOutput::Impl {
    Handle bus{open_bus()};
    struct Target {
        ULONG serial=0,user_index=0; uint8_t wcid=0; Handle stop{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
        std::jthread thread; std::atomic_uint feedback=0; std::atomic_bool dirty=false;
        std::optional<XUSB_REPORT> pending;
        ~Target(){if(stop.value)SetEvent(stop.value);if(thread.joinable())thread.join();}
    };
    std::array<std::unique_ptr<Target>,16> targets;
    std::mutex error_mutex;std::string error;
    void publish(Target &target) {
        if(!target.pending)return;
        XUSB_SUBMIT_REPORT report{};XUSB_SUBMIT_REPORT_INIT(&report,target.serial);report.Report=*target.pending;
        const auto result=ioctl(bus.value,IOCTL_XUSB_SUBMIT_REPORT,&report,sizeof(report));
        // ViGEm returns STATUS_NO_MORE_ENTRIES when XUSB has no pending USB read.
        // The bus has not cached this report; keep the latest state for the next tick.
        if(result==ERROR_NO_MORE_ITEMS)return;
        checked(result,"Publish XInput report");target.pending.reset();
    }
    void remove(uint8_t wcid) {
        if(!wcid || wcid>16 || !targets[wcid-1])return;
        const auto serial=targets[wcid-1]->serial;targets[wcid-1].reset();
        VIGEM_UNPLUG_TARGET unplug{};VIGEM_UNPLUG_TARGET_INIT(&unplug,serial);
        checked(ioctl(bus.value,IOCTL_VIGEM_UNPLUG_TARGET,&unplug,sizeof(unplug)),"Remove virtual controller");
        std::cout<<"VIRTUAL DISCONNECTED WCID="<<unsigned(wcid)<<"\n";
    }
    ~Impl(){for(uint8_t w=1;w<=16;++w)try{remove(w);}catch(...){} }
};
VigemOutput::VigemOutput():impl_(std::make_unique<Impl>()) {}
VigemOutput::~VigemOutput()=default;
void VigemOutput::connect(uint8_t wcid) {
    if(!wcid || wcid>16)throw std::runtime_error("Invalid virtual WCID");
    if(impl_->targets[wcid-1])return;
    auto target=std::make_unique<Impl::Target>();target->wcid=wcid;
    if(!target->stop.value)checked(GetLastError(),"Create feedback stop event");
    DWORD error=ERROR_ALREADY_EXISTS;
    for(ULONG serial=1;serial<=65535;++serial) {
        VIGEM_PLUGIN_TARGET plug{};VIGEM_PLUGIN_TARGET_INIT(&plug,serial,Xbox360Wired);plug.VendorId=0x045e;plug.ProductId=0x028e;
        error=ioctl(impl_->bus.value,IOCTL_VIGEM_PLUGIN_TARGET,&plug,sizeof(plug));
        if(!error){target->serial=serial;break;}
        if(error!=ERROR_INVALID_PARAMETER && error!=ERROR_ALREADY_EXISTS)break;
    }
    checked(error,"Create virtual Xbox 360 controller");
    try {
        VIGEM_WAIT_DEVICE_READY ready{};VIGEM_WAIT_DEVICE_READY_INIT(&ready,target->serial);
        checked(ioctl(impl_->bus.value,IOCTL_VIGEM_WAIT_DEVICE_READY,&ready,sizeof(ready),nullptr,0,10000),"Wait virtual controller ready");
        XUSB_GET_USER_INDEX index{};XUSB_GET_USER_INDEX_INIT(&index,target->serial);
        checked(ioctl(impl_->bus.value,IOCTL_XUSB_GET_USER_INDEX,&index,sizeof(index),&index,sizeof(index)),"Read XInput slot");
        if(index.UserIndex>3)throw std::runtime_error("All four XInput slots occupied");
        target->user_index=index.UserIndex;
        std::cout<<"VIRTUAL CONNECTED WCID="<<unsigned(wcid)<<" XInput="<<index.UserIndex<<" serial="<<target->serial<<"\n";
        auto *state=target.get();auto *owner=impl_.get();
        target->thread=std::jthread([state,owner] {
            try {
                Handle event{CreateEventW(nullptr,TRUE,FALSE,nullptr)};if(!event.value)checked(GetLastError(),"Feedback event");
                while(WaitForSingleObject(state->stop.value,0)!=WAIT_OBJECT_0) {
                    XUSB_REQUEST_NOTIFICATION notification{};XUSB_REQUEST_NOTIFICATION_INIT(&notification,state->serial);
                    OVERLAPPED op{};op.hEvent=event.value;ResetEvent(event.value);DWORD transferred=0;
                    const bool immediate=DeviceIoControl(owner->bus.value,IOCTL_XUSB_REQUEST_NOTIFICATION,&notification,sizeof(notification),&notification,sizeof(notification),&transferred,&op)!=FALSE;
                    if(!immediate) {
                        checked(GetLastError()==ERROR_IO_PENDING?ERROR_SUCCESS:GetLastError(),"Read rumble feedback");
                        HANDLE waits[]={state->stop.value,event.value};const auto wait=WaitForMultipleObjects(2,waits,FALSE,INFINITE);
                        if(wait!=WAIT_OBJECT_0+1) {const auto wait_error=GetLastError();CancelIoEx(owner->bus.value,&op);GetOverlappedResult(owner->bus.value,&op,&transferred,TRUE);if(wait==WAIT_OBJECT_0)return;checked(wait_error,"Wait feedback");}
                        checked(GetOverlappedResult(owner->bus.value,&op,&transferred,FALSE)?ERROR_SUCCESS:GetLastError(),"Complete feedback");
                    }
                    state->feedback.store(unsigned(notification.LargeMotor)|(unsigned(notification.SmallMotor)<<8));state->dirty=true;
                }
            }catch(const std::exception &e){std::lock_guard lock(owner->error_mutex);if(owner->error.empty())owner->error=e.what();}
        });
    }catch(...) {VIGEM_UNPLUG_TARGET unplug{};VIGEM_UNPLUG_TARGET_INIT(&unplug,target->serial);ioctl(impl_->bus.value,IOCTL_VIGEM_UNPLUG_TARGET,&unplug,sizeof(unplug));throw;}
    impl_->targets[wcid-1]=std::move(target);
}
void VigemOutput::update(uint8_t wcid,const GamepadInput &input,bool guide) {
    if(!wcid || wcid>16 || !impl_->targets[wcid-1])return;
    const auto p=virtual_report(input,guide);auto &target=*impl_->targets[wcid-1];
    target.pending=XUSB_REPORT{p.buttons,p.left_trigger,p.right_trigger,p.left_x,p.left_y,p.right_x,p.right_y};
    impl_->publish(target);
}
void VigemOutput::disconnect(uint8_t wcid){impl_->remove(wcid);}
unsigned VigemOutput::slot(uint8_t wcid) const {if(!wcid || wcid>16 || !impl_->targets[wcid-1])throw std::runtime_error("Virtual controller not connected");return impl_->targets[wcid-1]->user_index;}
std::optional<Feedback> VigemOutput::feedback() {
    {std::lock_guard lock(impl_->error_mutex);if(!impl_->error.empty())throw std::runtime_error(impl_->error);}
    for(const auto &target:impl_->targets)if(target)impl_->publish(*target);
    for(const auto &target:impl_->targets)if(target && target->dirty.exchange(false)) {
        const auto value=target->feedback.load();return Feedback{target->wcid,static_cast<uint8_t>(value),static_cast<uint8_t>(value>>8)};
    }
    return {};
}
}
