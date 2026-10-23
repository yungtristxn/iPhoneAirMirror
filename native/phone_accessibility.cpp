#include "phone_accessibility.h"
#include "phone_shell.h"
#include <algorithm>
#include <cmath>
#include <new>
#include <vector>
#include <future>
using Microsoft::WRL::ComPtr;
namespace {
HRESULT request(const std::shared_ptr<AccessibilityLifetime>& life,AccessibilityRequest& request){
    const HWND window=life->window.load();if(!window)return UIA_E_ELEMENTNOTAVAILABLE;
    if(GetWindowThreadProcessId(window,nullptr)==GetCurrentThreadId()){
        SendMessageW(window,PhoneShell::WM_ACCESSIBILITY,0,reinterpret_cast<LPARAM>(&request));return request.result;
    }
    // Cross-thread calls are bounded in time and count. The pending table owns
    // each message until dispatch or detach; timeout never leaves a stack pointer.
    auto call=std::make_shared<AccessibilityCall>();if(!call->done)return E_OUTOFMEMORY;call->request=request;
    {std::lock_guard<std::mutex> lock(life->mutex);if(life->window!=window)return UIA_E_ELEMENTNOTAVAILABLE;if(life->pending.size()>=8)return UIA_E_TIMEOUT;life->pending.push_back(call);}
    if(!PostMessageW(window,PhoneShell::WM_ACCESSIBILITY,1,reinterpret_cast<LPARAM>(call.get()))){
        std::lock_guard<std::mutex> lock(life->mutex);auto& pending=life->pending;pending.erase(std::remove(pending.begin(),pending.end(),call),pending.end());return UIA_E_ELEMENTNOTAVAILABLE;
    }
    if(WaitForSingleObject(call->done,2000)!=WAIT_OBJECT_0){call->canceled=true;return UIA_E_TIMEOUT;}
    request=std::move(call->request);return request.result;
}
std::vector<int> children(int parent,const AccessibilitySnapshot& state){
    if(parent==0){std::vector<int> ids{8};if(state.status>=3)ids.push_back(7);return ids;}
    if(parent==8)return state.collapsed?std::vector<int>{5}:std::vector<int>{1,6,2,3,4,5};
    return {};
}
std::wstring name(int id,const AccessibilitySnapshot& s){
    switch(id){case 0:return L"iPhone Air Mirror";case 1:return L"Mute audio";case 2:return L"Default size";
    case 3:return L"Fullscreen";case 4:return L"Close iPhone Air Mirror";case 5:return s.collapsed?L"Show controls":L"Hide controls";
    case 6:return L"Volume";case 7:return s.reason;case 8:return L"Quick controls";default:return L"";}
}
int toggle(int id,const AccessibilitySnapshot& s){return id==1?s.muted:id==3?s.fullscreen:!s.collapsed;}
class Provider final:public IRawElementProviderSimple,public IRawElementProviderFragment,public IRawElementProviderFragmentRoot,
    public IInvokeProvider,public IToggleProvider,public IRangeValueProvider {
    std::atomic<ULONG> references_{1};
    std::shared_ptr<AccessibilityLifetime> life_;
    ComPtr<IRawElementProviderFragmentRoot> root_provider_;
    int id_;
    HRESULT snapshot(AccessibilitySnapshot& result){AccessibilityRequest query;HRESULT hr=request(life_,query);if(SUCCEEDED(hr))result=std::move(query.snapshot);return hr;}
    Provider* make(int id){return new(std::nothrow) Provider(life_,id,id_==0?static_cast<IRawElementProviderFragmentRoot*>(this):root_provider_.Get());}
    HRESULT fragment(int id,IRawElementProviderFragment** result){*result=nullptr;if(id<0)return S_OK;if(id==0)return id_==0?QueryInterface(__uuidof(IRawElementProviderFragment),reinterpret_cast<void**>(result)):root_provider_->QueryInterface(__uuidof(IRawElementProviderFragment),reinterpret_cast<void**>(result));auto node=make(id);if(!node)return E_OUTOFMEMORY;*result=node;return S_OK;}
public:
    Provider(std::shared_ptr<AccessibilityLifetime> life,int id,IRawElementProviderFragmentRoot* root=nullptr):life_(std::move(life)),root_provider_(root),id_(id){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result)override{
        if(!result)return E_POINTER;*result=nullptr;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IRawElementProviderSimple))*result=static_cast<IRawElementProviderSimple*>(this);
        else if(iid==__uuidof(IRawElementProviderFragment))*result=static_cast<IRawElementProviderFragment*>(this);
        else if(iid==__uuidof(IRawElementProviderFragmentRoot)&&id_==0)*result=static_cast<IRawElementProviderFragmentRoot*>(this);
        else if(iid==__uuidof(IInvokeProvider)&&id_>=1&&id_<=5)*result=static_cast<IInvokeProvider*>(this);
        else if(iid==__uuidof(IToggleProvider)&&(id_==1||id_==3||id_==5))*result=static_cast<IToggleProvider*>(this);
        else if(iid==__uuidof(IRangeValueProvider)&&id_==6)*result=static_cast<IRangeValueProvider*>(this);
        if(!*result)return E_NOINTERFACE;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++references_;}
    ULONG STDMETHODCALLTYPE Release()override{ULONG count=--references_;if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* value)override{if(!value)return E_POINTER;*value=static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider|ProviderOptions_ProviderOwnsSetFocus);return S_OK;}
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern,IUnknown** value)override{
        if(!value)return E_POINTER;*value=nullptr;
        if(pattern==UIA_InvokePatternId&&id_>=1&&id_<=5)return QueryInterface(__uuidof(IInvokeProvider),reinterpret_cast<void**>(value));
        if(pattern==UIA_TogglePatternId&&(id_==1||id_==3||id_==5))return QueryInterface(__uuidof(IToggleProvider),reinterpret_cast<void**>(value));
        if(pattern==UIA_RangeValuePatternId&&id_==6)return QueryInterface(__uuidof(IRangeValueProvider),reinterpret_cast<void**>(value));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property,VARIANT* value)override{
        if(!value)return E_POINTER;VariantInit(value);AccessibilitySnapshot s;HRESULT hr=snapshot(s);if(FAILED(hr))return hr;
        auto boolean=[&](bool v){value->vt=VT_BOOL;value->boolVal=v?VARIANT_TRUE:VARIANT_FALSE;};
        auto integer=[&](LONG v){value->vt=VT_I4;value->lVal=v;};
        auto text=[&](const std::wstring& v){value->vt=VT_BSTR;value->bstrVal=SysAllocStringLen(v.data(),UINT(v.size()));return value->bstrVal?S_OK:E_OUTOFMEMORY;};
        switch(property){
        case UIA_NamePropertyId:return text(name(id_,s));
        case UIA_AutomationIdPropertyId:return text(L"AirMirrorLite.Control."+std::to_wstring(id_));
        case UIA_FrameworkIdPropertyId:return text(L"Win32");
        case UIA_ClassNamePropertyId:return text(L"AirMirrorLiteControl");
        case UIA_ControlTypePropertyId:integer(id_==0||id_==8?UIA_PaneControlTypeId:id_==6?UIA_SliderControlTypeId:id_==7?UIA_TextControlTypeId:UIA_ButtonControlTypeId);break;
        case UIA_IsKeyboardFocusablePropertyId:boolean(id_>=0&&id_<=6);break;
        case UIA_HasKeyboardFocusPropertyId:boolean(s.window_focused&&s.focus==id_);break;
        case UIA_IsEnabledPropertyId:boolean(true);break;
        case UIA_IsControlElementPropertyId:case UIA_IsContentElementPropertyId:boolean(true);break;
        case UIA_IsOffscreenPropertyId:boolean(!s.visible[id_]);break;
        case UIA_HelpTextPropertyId:return text(id_==0?L"On your iPhone, open Control Center, choose Screen Mirroring, then select iPhone Air Mirror. Both devices must be on the same local network.":id_==1?L"Toggle local playback mute. Arrow keys adjust volume.":id_==2?L"Restore the default window size. Keyboard shortcut: 0.":id_==6?L"Local playback volume, from 0 to 100 percent.":L"");
        case UIA_LiveSettingPropertyId:if(id_==7)integer(Polite);break;
        }return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** value)override{
        if(!value)return E_POINTER;*value=nullptr;AccessibilitySnapshot s;HRESULT hr=snapshot(s);if(FAILED(hr))return hr;
        HWND host=id_==0?s.window:id_==8?s.palette:id_==7?s.status_window:nullptr;
        return host?UiaHostProviderFromHwnd(host,value):S_OK;
    }
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction,IRawElementProviderFragment** value)override{
        if(!value)return E_POINTER;AccessibilitySnapshot s;HRESULT hr=snapshot(s);if(FAILED(hr)){*value=nullptr;return hr;}int target=-1;
        if(direction==NavigateDirection_FirstChild||direction==NavigateDirection_LastChild){auto ids=children(id_,s);if(!ids.empty())target=direction==NavigateDirection_FirstChild?ids.front():ids.back();}
        else if(id_!=0){const int parent=id_==8||id_==7?0:8;if(direction==NavigateDirection_Parent)target=parent;
            else{auto ids=children(parent,s);auto it=std::find(ids.begin(),ids.end(),id_);if(it!=ids.end()){if(direction==NavigateDirection_NextSibling&&it+1!=ids.end())target=*(it+1);if(direction==NavigateDirection_PreviousSibling&&it!=ids.begin())target=*(it-1);}}}
        return fragment(target,value);
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** value)override{
        if(!value)return E_POINTER;*value=nullptr;if(!life_->window.load())return UIA_E_ELEMENTNOTAVAILABLE;if(id_==0||id_==7||id_==8)return S_OK;
        int ids[]={UiaAppendRuntimeId,id_};*value=SafeArrayCreateVector(VT_I4,0,2);if(!*value)return E_OUTOFMEMORY;
        for(LONG i=0;i<2;++i)SafeArrayPutElement(*value,&i,&ids[i]);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* value)override{
        if(!value)return E_POINTER;AccessibilitySnapshot s;HRESULT hr=snapshot(s);if(FAILED(hr))return hr;auto r=s.bounds[id_];*value={double(r.left),double(r.top),double(r.right-r.left),double(r.bottom-r.top)};return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** value)override{if(!value)return E_POINTER;*value=nullptr;return life_->window.load()?S_OK:UIA_E_ELEMENTNOTAVAILABLE;}
    HRESULT STDMETHODCALLTYPE SetFocus()override{if(id_<0||id_>6)return UIA_E_INVALIDOPERATION;AccessibilityRequest r;r.operation=AccessibilityRequest::Focus;r.id=id_;return request(life_,r);}
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** value)override{
        if(!value)return E_POINTER;*value=nullptr;if(!life_->window.load())return UIA_E_ELEMENTNOTAVAILABLE;return id_==0?QueryInterface(__uuidof(IRawElementProviderFragmentRoot),reinterpret_cast<void**>(value)):root_provider_.CopyTo(value);
    }
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x,double y,IRawElementProviderFragment** value)override{
        if(!value)return E_POINTER;AccessibilitySnapshot s;HRESULT hr=snapshot(s);if(FAILED(hr)){*value=nullptr;return hr;}
        for(int id:{7,1,6,2,3,4,5,8}){const RECT r=s.bounds[id];if(s.visible[id]&&x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom)return fragment(id,value);}
        const RECT r=s.bounds[0];return fragment(x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom?0:-1,value);
    }
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** value)override{
        if(!value)return E_POINTER;AccessibilitySnapshot s;HRESULT hr=snapshot(s);if(FAILED(hr)){*value=nullptr;return hr;}return fragment(s.window_focused?s.focus:-1,value);
    }
    HRESULT STDMETHODCALLTYPE Invoke()override{if(id_<1||id_>5)return UIA_E_INVALIDOPERATION;AccessibilityRequest r;r.operation=AccessibilityRequest::Invoke;r.id=id_;return request(life_,r);}
    HRESULT STDMETHODCALLTYPE Toggle()override{return id_==1||id_==3||id_==5?Invoke():UIA_E_INVALIDOPERATION;}
    HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* value)override{if(!value)return E_POINTER;AccessibilitySnapshot s;HRESULT hr=snapshot(s);if(FAILED(hr))return hr;*value=toggle(id_,s)?ToggleState_On:ToggleState_Off;return S_OK;}
    HRESULT STDMETHODCALLTYPE SetValue(double value)override{if(!std::isfinite(value)||value<0||value>100)return E_INVALIDARG;AccessibilityRequest r;r.operation=AccessibilityRequest::Volume;r.id=id_;r.value=value;return request(life_,r);}
    HRESULT STDMETHODCALLTYPE get_Value(double* value)override{if(!value)return E_POINTER;AccessibilitySnapshot s;HRESULT hr=snapshot(s);if(SUCCEEDED(hr))*value=s.volume;return hr;}
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* value)override{if(!value)return E_POINTER;*value=FALSE;return S_OK;}
    HRESULT STDMETHODCALLTYPE get_Maximum(double* value)override{if(!value)return E_POINTER;*value=100;return S_OK;}
    HRESULT STDMETHODCALLTYPE get_Minimum(double* value)override{if(!value)return E_POINTER;*value=0;return S_OK;}
    HRESULT STDMETHODCALLTYPE get_LargeChange(double* value)override{if(!value)return E_POINTER;*value=10;return S_OK;}
    HRESULT STDMETHODCALLTYPE get_SmallChange(double* value)override{if(!value)return E_POINTER;*value=1;return S_OK;}
};
}
PhoneAccessibility::PhoneAccessibility(){root_.Attach(new Provider(lifetime_,0));}
PhoneAccessibility::~PhoneAccessibility(){detach();}
void PhoneAccessibility::attach(HWND window){lifetime_->window=window;known_=false;}
void PhoneAccessibility::detach(){
    bool attached=false;
    {std::lock_guard<std::mutex> lock(lifetime_->mutex);attached=lifetime_->window.exchange(nullptr)!=nullptr;
        for(auto& call:lifetime_->pending){call->canceled=true;call->request.result=UIA_E_ELEMENTNOTAVAILABLE;SetEvent(call->done);}lifetime_->pending.clear();}
    if(attached&&root_)UiaDisconnectProvider(root_.Get());
}
LRESULT PhoneAccessibility::dispatch_pending(void* token){
    std::shared_ptr<AccessibilityCall> call;
    {std::lock_guard<std::mutex> lock(lifetime_->mutex);auto& pending=lifetime_->pending;
        auto found=std::find_if(pending.begin(),pending.end(),[&](const auto& entry){return entry.get()==token;});
        if(found==pending.end())return 0;call=*found;pending.erase(found);}
    if(call->canceled)call->request.result=UIA_E_TIMEOUT;
    else if(const HWND window=lifetime_->window.load())SendMessageW(window,PhoneShell::WM_ACCESSIBILITY,0,reinterpret_cast<LPARAM>(&call->request));
    SetEvent(call->done);return 1;
}
LRESULT PhoneAccessibility::return_provider(HWND window,WPARAM wp,LPARAM lp){
    AccessibilityRequest query;if(FAILED(request(lifetime_,query)))return 0;
    const int id=window==query.snapshot.palette?8:window==query.snapshot.status_window?7:0;
    ComPtr<IRawElementProviderSimple> node;node.Attach(provider_for_test(id));return UiaReturnRawElementProvider(window,wp,lp,node.Get());
}
IRawElementProviderSimple* PhoneAccessibility::provider_for_test(int id){if(id<0||id>=9)return nullptr;if(id==0){root_->AddRef();return root_.Get();}ComPtr<IRawElementProviderFragmentRoot> root;root_.As(&root);return new(std::nothrow) Provider(lifetime_,id,root.Get());}
void PhoneAccessibility::invoked(int id){if(lifetime_->window.load()&&UiaClientsAreListening()){ComPtr<IRawElementProviderSimple> node;node.Attach(provider_for_test(id));if(node)UiaRaiseAutomationEvent(node.Get(),UIA_Invoke_InvokedEventId);}}
HRESULT PhoneAccessibility::transport_test(){
    const HWND window=lifetime_->window.load();if(!window||GetWindowThreadProcessId(window,nullptr)!=GetCurrentThreadId())return E_UNEXPECTED;
    auto query=[](ComPtr<IRawElementProviderSimple> node){VARIANT value{};HRESULT hr=node->GetPropertyValue(UIA_NamePropertyId,&value);if(SUCCEEDED(hr)&&(value.vt!=VT_BSTR||!SysStringLen(value.bstrVal)))hr=E_FAIL;VariantClear(&value);return hr;};
    ComPtr<IRawElementProviderSimple> node;node.Attach(provider_for_test(1));
    auto call=std::async(std::launch::async,query,node);const auto began=GetTickCount64();
    while(call.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready&&GetTickCount64()-began<3000){
        MSG message;while(PeekMessageW(&message,window,PhoneShell::WM_ACCESSIBILITY,PhoneShell::WM_ACCESSIBILITY,PM_REMOVE))DispatchMessageW(&message);Sleep(1);
    }
    if(FAILED(call.get()))return E_FAIL;
    // A deliberately unserviced separate token proves timed-out requests remain
    // owned, and detach releases both pending calls and waiting provider clients.
    PhoneAccessibility temporary;temporary.attach(window);node.Reset();node.Attach(temporary.provider_for_test(1));
    auto expired=std::async(std::launch::async,query,node);
    if(expired.get()!=HRESULT(UIA_E_TIMEOUT))return E_FAIL;
    auto waiting=std::async(std::launch::async,query,node);const auto waiting_at=GetTickCount64();bool pending=false;
    while(GetTickCount64()-waiting_at<1000){
        {std::lock_guard<std::mutex> lock(temporary.lifetime_->mutex);pending=temporary.lifetime_->pending.size()==2;}
        if(pending)break;Sleep(1);
    }
    temporary.detach();if(!pending||waiting.get()!=HRESULT(UIA_E_ELEMENTNOTAVAILABLE))return E_FAIL;
    if(query(node)!=HRESULT(UIA_E_ELEMENTNOTAVAILABLE))return E_FAIL;
    MSG message;while(PeekMessageW(&message,window,PhoneShell::WM_ACCESSIBILITY,PhoneShell::WM_ACCESSIBILITY,PM_REMOVE))DispatchMessageW(&message);
    return S_OK;
}
void PhoneAccessibility::changed(){
    if(notifying_||!lifetime_->window.load())return;notifying_=true;AccessibilityRequest query;
    if(SUCCEEDED(request(lifetime_,query))){auto& now=query.snapshot;
        if(known_&&UiaClientsAreListening()){
            if(now.collapsed!=previous_.collapsed){ComPtr<IRawElementProviderSimple> rail;rail.Attach(provider_for_test(8));UiaRaiseStructureChangedEvent(rail.Get(),StructureChangeType_ChildrenInvalidated,nullptr,0);}
            if((now.status>=3)!=(previous_.status>=3))UiaRaiseStructureChangedEvent(root_.Get(),StructureChangeType_ChildrenInvalidated,nullptr,0);
            for(int id:{1,3,5,6}){ComPtr<IRawElementProviderSimple> node;node.Attach(provider_for_test(id));VARIANT before{},after{};
                if(id==6){before.vt=after.vt=VT_R8;before.dblVal=previous_.volume;after.dblVal=now.volume;}
                else{before.vt=after.vt=VT_I4;before.lVal=toggle(id,previous_);after.lVal=toggle(id,now);}
                if(id==6?before.dblVal!=after.dblVal:before.lVal!=after.lVal)UiaRaiseAutomationPropertyChangedEvent(node.Get(),id==6?UIA_RangeValueValuePropertyId:UIA_ToggleToggleStatePropertyId,before,after);
            }
            if(now.window_focused&&(!previous_.window_focused||now.focus!=previous_.focus)){ComPtr<IRawElementProviderSimple> node;node.Attach(provider_for_test(now.focus));UiaRaiseAutomationEvent(node.Get(),UIA_AutomationFocusChangedEventId);}
            if(now.status>=3&&(now.status!=previous_.status||now.reason!=previous_.reason)){ComPtr<IRawElementProviderSimple> node;node.Attach(provider_for_test(7));UiaRaiseAutomationEvent(node.Get(),UIA_LiveRegionChangedEventId);}
        }
        previous_=std::move(now);known_=true;
    }
    notifying_=false;
}
