#pragma once
#include <windows.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct AccessibilitySnapshot {
    HWND window=nullptr,palette=nullptr,status_window=nullptr;
    std::array<RECT,9> bounds{};
    std::array<bool,9> visible{};
    bool collapsed=false,muted=false,fullscreen=false,window_focused=false;
    double volume=100;
    int focus=0,status=0;
    std::wstring reason;
};
struct AccessibilityRequest {
    enum Operation {Snapshot,Invoke,Focus,Volume};
    Operation operation=Snapshot;
    int id=0;
    double value=0;
    AccessibilitySnapshot snapshot;
    HRESULT result=UIA_E_ELEMENTNOTAVAILABLE;
};
struct AccessibilityCall {
    AccessibilityRequest request;
    HANDLE done=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::atomic<bool> canceled{false};
    ~AccessibilityCall(){if(done)CloseHandle(done);}
};
struct AccessibilityLifetime {
    std::atomic<HWND> window{nullptr};
    std::mutex mutex;
    std::vector<std::shared_ptr<AccessibilityCall>> pending; // at most eight calls
};
class PhoneAccessibility {
    std::shared_ptr<AccessibilityLifetime> lifetime_=std::make_shared<AccessibilityLifetime>();
    Microsoft::WRL::ComPtr<IRawElementProviderSimple> root_;
    AccessibilitySnapshot previous_;
    bool known_=false,notifying_=false;
public:
    PhoneAccessibility();
    ~PhoneAccessibility();
    void attach(HWND window);
    void detach();
    LRESULT return_provider(HWND window,WPARAM wp,LPARAM lp);
    void changed();
    void invoked(int id);
    LRESULT dispatch_pending(void* token);
    // Hidden in-process tests query the same providers exposed by WM_GETOBJECT.
    IRawElementProviderSimple* provider_for_test(int id);
    HRESULT transport_test();
};
