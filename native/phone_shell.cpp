#include "phone_shell.h"
#include <windowsx.h>
#include <gdiplus.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <future>
#include "control_artwork.h"
#include "resources.h"
#include "phone_accessibility.h"
using namespace Gdiplus;
namespace {
constexpr DWORD WINDOW_STYLE=WS_POPUP|WS_THICKFRAME|WS_SYSMENU|WS_MINIMIZEBOX;
constexpr int PALETTE_WIDTH=control_layout::rail_width, BODY_HEIGHT=control_layout::height-20, FLYOUT_WIDTH=212, FLYOUT_SPACE=224;
constexpr UINT_PTR HIDE_VOLUME=1;
constexpr int RAIL_GAP=0,WORK_MARGIN=12;
int rail_clearance(float s){return int(std::ceil((PALETTE_WIDTH+RAIL_GAP+WORK_MARGIN)*s));}
bool toggles_fullscreen(WPARAM command,bool fullscreen,bool minimized){
    const WPARAM action=command&0xfff0;
    // Taskbar restore must restore a minimized fullscreen window first; the
    // ordinary fullscreen Restore command returns to the saved window bounds.
    return action==SC_MAXIMIZE||(action==SC_RESTORE&&fullscreen&&!minimized);
}
struct StatusCardSize {int width,height;bool compact;};
StatusCardSize status_card_size(RECT phone,RECT work,float scale){
    // Keep a narrow phone's status card clear of its external control rail.
    const int width=std::min({int(340*scale),std::max(1,int(phone.right-phone.left)-int(12*scale)),int(work.right-work.left)});
    const bool compact=width<240*scale;
    return {width,std::min(int((compact?244:218)*scale),int(work.bottom-work.top)),compact};
}
struct PalettePlacement {int x,y,width,height,rail,arrow,flyout;bool right,popup;};
PalettePlacement palette_placement(RECT phone,RECT work,float s,bool collapsed,bool popup,bool fullscreen){
    const int base=collapsed?36:PALETTE_WIDTH,base_px=int(base*s),gap=int(std::ceil(RAIL_GAP*s));
    const int expanded=int(std::ceil(PALETTE_WIDTH*s))+gap;
    const int left_space=phone.left-work.left,right_space=work.right-phone.right;
    bool right=fullscreen||right_space>=expanded||(left_space<expanded&&right_space>=left_space);
    int x=fullscreen?phone.right-base_px-int(WORK_MARGIN*s):right?phone.right+gap:phone.left-base_px-gap;
    x=std::clamp<LONG>(x,work.left,std::max(work.left,work.right-base_px));
    const int height=int((BODY_HEIGHT+20)*s);
    // Anchor near the phone's shoulder; short landscape windows stay centered.
    const int inset=std::min(int(80*s),int(phone.bottom-phone.top-height)/2);
    const int y=std::clamp<LONG>(phone.top+inset,work.top,std::max(work.top,work.bottom-height));
    popup=popup&&int((base+FLYOUT_SPACE)*s)<=work.right-work.left;
    bool extend_right=x+int((base+FLYOUT_SPACE)*s)<=work.right;
    int offset=popup&&!extend_right?FLYOUT_SPACE:0,width=int((base+(popup?FLYOUT_SPACE:0))*s);
    int panel_x=std::clamp<LONG>(x-int(offset*s),work.left,std::max(work.left,work.right-width));
    // Only the neither-side-fits fallback may move the rail when opening a flyout.
    return {panel_x,y,width,height,offset+(right?0:20),offset+(collapsed?18:right?84:12),extend_right?PALETTE_WIDTH+2:10,right,popup};
}
constexpr wchar_t const* labels[]={L"Mute / unmute (M) — hover for volume",L"Restore default size (0)",L"Fullscreen (F11)",L"Close AirMirrorLite (Alt+F4)",L"Hide controls (C)"};
RECT work_area(HWND hwnd){MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&mi);return mi.rcWork;}
}
// One bounded backing bitmap, owned and released on the UI thread. Moving the
// owned palette does not change its pixels and must not allocate or repaint.
class PaletteSurface {
public:
    HDC memory=nullptr;
    HBITMAP bitmap=nullptr;
    HGDIOBJ previous=nullptr;
    void* pixels=nullptr;
    int width=0,height=0;
    uint64_t allocations=0,draws=0;
    bool valid=false;
    ControlArtwork artwork{};
    ~PaletteSurface(){release();}
    void release(){
        if(memory&&previous)SelectObject(memory,previous);
        if(bitmap)DeleteObject(bitmap);
        if(memory)DeleteDC(memory);
        memory=nullptr;bitmap=nullptr;previous=nullptr;pixels=nullptr;width=height=0;valid=false;
    }
    HRESULT resize(int w,int h){
        if(w<1||h<1||w>2048||h>2048)return E_INVALIDARG;
        if(memory&&w==width&&h==height)return S_OK;
        HDC next_memory=CreateCompatibleDC(nullptr);
        if(!next_memory)return E_OUTOFMEMORY;
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        void* next_pixels=nullptr;HBITMAP next_bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&next_pixels,nullptr,0);
        if(!next_bitmap||!next_pixels){if(next_bitmap)DeleteObject(next_bitmap);DeleteDC(next_memory);return E_OUTOFMEMORY;}
        HGDIOBJ next_previous=SelectObject(next_memory,next_bitmap);
        if(!next_previous||next_previous==HGDI_ERROR){DeleteObject(next_bitmap);DeleteDC(next_memory);return E_FAIL;}
        release();memory=next_memory;bitmap=next_bitmap;previous=next_previous;pixels=next_pixels;width=w;height=h;++allocations;
        return S_OK;
    }
};
PhoneShell::PhoneShell()=default;
PhoneShell::~PhoneShell(){shutdown();}
float PhoneShell::corner_radius(UINT width,UINT height,float s){return std::clamp(float(std::min(width,height))*.135f,24*s,58*s);}
float PhoneShell::dpi_scale() const {return window?GetDpiForWindow(window)/96.0f:1.0f;}
void PhoneShell::signal(){redraw=true;chrome_dirty=true;if(wake_)SetEvent(wake_);}
HRESULT PhoneShell::create(HANDLE wake,bool hidden){
    wake_=wake;hidden_=hidden;std::promise<HWND> created;auto ready=created.get_future();
    thread_=std::thread([this,created=std::move(created)]() mutable {
        if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED))){created.set_value(nullptr);return;}
        GdiplusStartupInput startup;if(GdiplusStartup(&gdiplus_,&startup,nullptr)!=Ok){CoUninitialize();created.set_value(nullptr);return;}
        painter_=std::make_unique<ControlPainter>();
        palette_surface_=std::make_unique<PaletteSurface>();
        status_surface_=std::make_unique<PaletteSurface>();
        accessibility_=std::make_unique<PhoneAccessibility>();
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
        WNDCLASSW wc{};wc.lpfnWndProc=main_proc;wc.hInstance=GetModuleHandleW(nullptr);wc.hIcon=LoadIconW(wc.hInstance,MAKEINTRESOURCEW(IDI_AIRMIRRORLITE));wc.style=CS_DBLCLKS;wc.lpszClassName=L"AirMirrorLiteWindow";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&wc);
        WNDCLASSW pc=wc;pc.lpfnWndProc=palette_proc;pc.lpszClassName=L"AirMirrorLiteControls";RegisterClassW(&pc);
        WNDCLASSW sc=wc;sc.lpfnWndProc=status_proc;sc.lpszClassName=L"AirMirrorLiteStatus";RegisterClassW(&sc);
        window=CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP,wc.lpszClassName,L"AirMirrorLite",WINDOW_STYLE,0,0,360,760,nullptr,nullptr,wc.hInstance,this);
        if(window){
            update_icons();
            BOOL dark=TRUE;DwmSetWindowAttribute(window,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
            DWORD border=DWMWA_COLOR_NONE;DwmSetWindowAttribute(window,DWMWA_BORDER_COLOR,&border,sizeof(border));
            palette_=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,pc.lpszClassName,L"AirMirrorLite quick controls",WS_POPUP,0,0,72,220,window,nullptr,wc.hInstance,this);
            status_panel_=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT,sc.lpszClassName,L"AirMirrorLite status",WS_POPUP,0,0,320,160,window,nullptr,wc.hInstance,this);
            accessibility_->attach(window);
            tooltip_=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,palette_,nullptr,wc.hInstance,nullptr);
            SendMessageW(tooltip_,TTM_SETDELAYTIME,TTDT_INITIAL,450);SendMessageW(tooltip_,TTM_SETMAXTIPWIDTH,0,250);
            for(UINT_PTR i=0;i<5;++i){TOOLINFOW t{sizeof(t)};t.uFlags=TTF_SUBCLASS;t.hwnd=palette_;t.uId=i+1;t.lpszText=const_cast<LPWSTR>(labels[i]);SendMessageW(tooltip_,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&t));}
            POINT cursor;GetCursorPos(&cursor);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST),&mi);
            SetWindowPos(window,nullptr,mi.rcWork.left+40,mi.rcWork.top+40,360,760,SWP_NOZORDER|SWP_NOACTIVATE);
            default_size(true);
            initial_paint_=paint_palette();
        }
        created.set_value(window);
        if(window){MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}}
        painter_.reset();
        palette_surface_.reset();
        status_surface_.reset();
        accessibility_.reset();
        if(small_icon_)DestroyIcon(small_icon_);if(large_icon_)DestroyIcon(large_icon_);small_icon_=large_icon_=nullptr;
        if(gdiplus_){GdiplusShutdown(gdiplus_);gdiplus_=0;}
        CoUninitialize();
    });
    HWND result=ready.get();return result&&palette_&&tooltip_&&status_panel_?initial_paint_:E_FAIL;
}
void PhoneShell::show(){PostMessageW(window,WM_SHOW,0,0);}
void PhoneShell::update_icons(){
    UINT dpi=GetDpiForWindow(window);auto instance=GetModuleHandleW(nullptr);
    auto load=[&](int metric){int size=GetSystemMetricsForDpi(metric,dpi);return static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(IDI_AIRMIRRORLITE),IMAGE_ICON,size,size,LR_DEFAULTCOLOR));};
    HICON next_small=load(SM_CXSMICON),next_large=load(SM_CXICON);
    if(next_small){SendMessageW(window,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(next_small));if(small_icon_)DestroyIcon(small_icon_);small_icon_=next_small;}
    if(next_large){SendMessageW(window,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(next_large));if(large_icon_)DestroyIcon(large_icon_);large_icon_=next_large;}
}
void PhoneShell::shutdown(){if(thread_.joinable()){if(window)PostMessageW(window,WM_SHUTDOWN,0,0);thread_.join();}window=nullptr;}
void PhoneShell::set_title(const std::wstring& title){SetWindowTextW(window,title.c_str());}
std::wstring PhoneShell::status_message() const {std::lock_guard<std::mutex> lock(status_mutex_);return status_reason_;}
void PhoneShell::set_status(int kind,const std::wstring& message){
    if(kind<0||kind>6)return;
    {std::lock_guard<std::mutex> lock(status_mutex_);
        const int current=status;
        if(kind==6){status=has_video?2:0;status_reason_.clear();}
        else if((current==5&&kind<3)||(current==4&&kind<2))return;
        else{status=kind;status_reason_.clear();for(wchar_t c:message){if(status_reason_.size()==256)break;status_reason_.push_back(c>=32&&c!=127?c:L' ');}if(kind==4||kind==5)error_generation_=source_generation_;}
    }
    signal();PostMessageW(window,WM_REFRESH,0,0);
}
bool PhoneShell::dismiss_error(){
    {std::lock_guard<std::mutex> lock(status_mutex_);if(status!=4)return false;status=has_video?2:0;status_reason_.clear();}
    status_dismissed=true;signal();PostMessageW(window,WM_REFRESH,0,0);return true;
}
void PhoneShell::video_size(UINT w,UINT h){
    if(!w||!h||w>3840||h>3840)return;
    if(!has_video.exchange(true)){
        {std::lock_guard<std::mutex> lock(status_mutex_);if(status<3||(status==4&&source_generation_>error_generation_)){status=2;status_reason_.clear();}}
        chrome_dirty=true;signal();PostMessageW(window,WM_REFRESH,0,0);
    }
    if(published_w_!=w||published_h_!=h){published_w_=w;published_h_=h;PostMessageW(window,WM_SOURCE_FORMAT,source_generation_,MAKELPARAM(w,h));}
}
void PhoneShell::reset_video(){
    has_video=false;{std::lock_guard<std::mutex> lock(status_mutex_);if(status<3){status=0;status_reason_.clear();}}published_w_=published_h_=0;++source_generation_;
    PostMessageW(window,WM_SOURCE_RESET,source_generation_,0);signal();PostMessageW(window,WM_REFRESH,0,0);
}
void PhoneShell::shape_window(){
    if(!window)return;
    if(fullscreen){SetWindowRgn(window,nullptr,TRUE);return;}
    RECT r;GetClientRect(window,&r);float s=dpi_scale();int radius=int(corner_radius(r.right,r.bottom,s));
    HRGN region=CreateRoundRectRgn(0,0,r.right+1,r.bottom+1,std::max(2,2*radius-4),std::max(2,2*radius-4));
    if(region && !SetWindowRgn(window,region,TRUE))DeleteObject(region);
}
void PhoneShell::default_size(bool center){
    if(fullscreen)toggle_fullscreen();
    RECT area=work_area(window),old{};GetWindowRect(window,&old);float s=dpi_scale();int border=rim_pixels(s);
    // Source pixels are physical pixels. DPI scales the controls, not the video.
    float source_h=display_h_?float(display_h_):760*s;
    float source_w=display_w_?float(display_w_):source_h*aspect_;
    float fit=std::min({1.0f,std::max(1.0f,float(area.right-area.left)-WORK_MARGIN*s-rail_clearance(s)-2*border)/source_w,std::max(1.0f,float(area.bottom-area.top)-48*s-2*border)/source_h});
    int w=int(std::round(source_w*fit))+2*border,h=int(std::round(source_h*fit))+2*border;
    int x=center?area.left+((area.right-area.left)-w-int(84*s))/2:old.left+(old.right-old.left-w)/2;
    int y=center?area.top+((area.bottom-area.top)-h)/2:old.top+(old.bottom-old.top-h)/2;
    x=std::clamp<LONG>(x,area.left+int(WORK_MARGIN*s),std::max(area.left+int(WORK_MARGIN*s),area.right-w-rail_clearance(s)));
    y=std::clamp<LONG>(y,area.top+int(12*s),std::max(area.top+int(12*s),area.bottom-h-int(12*s)));
    user_sized_=false;SetWindowPos(window,nullptr,x,y,w,h,SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);signal();
}
RECT PhoneShell::fitted_rect(RECT original) const {
    const float s=dpi_scale(),border=float(2*rim_pixels(s));
    RECT work=work_area(window);
    float area=std::max(1.0f,float(original.right-original.left)-border)*std::max(1.0f,float(original.bottom-original.top)-border);
    float h=std::sqrt(area/aspect_),w=h*aspect_;
    float factor=std::min({1.0f,std::max(1.0f,float(work.right-work.left)-WORK_MARGIN*s-rail_clearance(s)-border)/w,std::max(1.0f,float(work.bottom-work.top)-24*s-border)/h});
    int width=int(w*factor+border),height=int(h*factor+border);
    int x=(original.left+original.right-width)/2,y=(original.top+original.bottom-height)/2;
    x=std::clamp<LONG>(x,work.left+int(WORK_MARGIN*s),std::max(work.left+int(WORK_MARGIN*s),work.right-width-rail_clearance(s)));
    y=std::clamp<LONG>(y,work.top+int(12*s),std::max(work.top+int(12*s),work.bottom-height-int(12*s)));
    return {x,y,x+width,y+height};
}
void PhoneShell::toggle_fullscreen(){
    dismiss_volume();keyboard_focus_=-1;
    if(!fullscreen){
        GetWindowRect(window,&restore_);fullscreen=true;
        MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&mi);
        SetWindowLongPtrW(window,GWL_STYLE,WS_POPUP|WS_SYSMENU|(hidden_?0:WS_VISIBLE));
        SetWindowRgn(window,nullptr,TRUE);
        SetWindowPos(window,HWND_TOP,mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top,SWP_FRAMECHANGED);
    }else{
        fullscreen=false;restore_=fitted_rect(restore_);SetWindowLongPtrW(window,GWL_STYLE,WINDOW_STYLE|(hidden_?0:WS_VISIBLE));
        SetWindowPos(window,nullptr,restore_.left,restore_.top,restore_.right-restore_.left,restore_.bottom-restore_.top,SWP_NOZORDER|SWP_FRAMECHANGED);
    }
    shape_window();place_palette();signal();
}
void PhoneShell::set_volume(float level){
    level=std::clamp(level,0.0f,1.0f);volume=level;if(level>0){last_nonzero_=level;muted=false;}
    paint_palette();if(wake_)SetEvent(wake_);
}
void PhoneShell::toggle_mute(){
    if(muted || volume<=0.0f){muted=false;if(volume<=0.0f)volume=last_nonzero_;}else muted=true;
    paint_palette();if(wake_)SetEvent(wake_);
}
void PhoneShell::invoke(int button){
    if(button==0)toggle_mute();else if(button==1)default_size();else if(button==2)toggle_fullscreen();else if(button==3){quit=true;if(wake_)SetEvent(wake_);}else if(button==4)toggle_controls();
    if(accessibility_&&button>=0&&button<=4)accessibility_->invoked(button+1);
}
void PhoneShell::toggle_controls(){
    dismiss_volume();controls_collapsed_=!controls_collapsed_;hot_=-1;
    if(keyboard_focus_>=0)keyboard_focus_=controls_collapsed_?4:0;
    place_palette();
}
void PhoneShell::dismiss_volume(){
    // Escape, format changes and deactivation can all dismiss an active drag.
    // Clear before releasing capture because WM_CAPTURECHANGED is synchronous.
    dragging_slider_=false;pressed_=-1;
    if(GetCapture()==palette_)ReleaseCapture();
    update_volume_timer(true);TRACKMOUSEEVENT cancel{sizeof(cancel),TME_CANCEL|TME_HOVER,palette_,0};TrackMouseEvent(&cancel);
    hover_pending_=false;if(keyboard_focus_==5)keyboard_focus_=0;if(popup_){popup_=false;place_palette();}else paint_palette();
}
void PhoneShell::update_volume_timer(bool in_zone){
    if(!popup_||in_zone||dragging_slider_){
        if(hide_volume_pending_)KillTimer(palette_,HIDE_VOLUME);
        hide_volume_pending_=false;hide_volume_deadline_=0;
    }else if(!hide_volume_pending_){
        hide_volume_pending_=SetTimer(palette_,HIDE_VOLUME,350,nullptr)!=0;
        hide_volume_deadline_=hide_volume_pending_?GetTickCount64()+350:0;
    }
}
void PhoneShell::place_palette(){
    if(!palette_)return;
    RECT r;GetWindowRect(window,&r);float s=dpi_scale();auto placement=palette_placement(r,work_area(window),s,controls_collapsed_,popup_,fullscreen);
    dock_right_=placement.right;popup_=placement.popup;rail_x_=placement.rail;arrow_x_=placement.arrow;flyout_x_=placement.flyout;
    palette_width_=(controls_collapsed_?36:PALETTE_WIDTH)+(popup_?FLYOUT_SPACE:0);
    SetWindowPos(palette_,nullptr,placement.x,placement.y,placement.width,placement.height,SWP_NOZORDER|SWP_NOACTIVATE);
    paint_palette();
    if(!hidden_&&IsWindowVisible(window)&&!IsIconic(window))ShowWindow(palette_,SW_SHOWNOACTIVATE);else ShowWindow(palette_,SW_HIDE);
    paint_status_panel();
}
void PhoneShell::paint_status_panel(){
    if(!status_panel_||!status_surface_)return;
    const int kind=status;const auto reason=status_message();
    if(kind<3){ShowWindow(status_panel_,SW_HIDE);accessibility_changed();return;}
    const float s=dpi_scale();RECT phone,area=work_area(window);GetWindowRect(window,&phone);
    const auto card=status_card_size(phone,area,s);const int width=card.width,height=card.height;
    const int x=std::clamp<LONG>((phone.left+phone.right-width)/2,area.left,std::max(area.left,area.right-width));
    const int y=std::clamp<LONG>((phone.top+phone.bottom-height)/2,area.top,std::max(area.top,area.bottom-height));
    auto& surface=*status_surface_;const bool changed=surface.width!=width||surface.height!=height||painted_status_!=reason||painted_status_kind_!=kind;
    if(changed&&SUCCEEDED(surface.resize(width,height))){
        RECT bounds{0,0,width,height};HBRUSH brush=CreateSolidBrush(RGB(30,32,38));FillRect(surface.memory,&bounds,brush);DeleteObject(brush);
        SetBkMode(surface.memory,TRANSPARENT);
        auto label=[&](const std::wstring& value,int top,int bottom,int size,int weight,COLORREF color){
            HFONT font=CreateFontW(-int(size*s),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
            auto old=SelectObject(surface.memory,font);SetTextColor(surface.memory,color);RECT text{int(18*s),int(top*s),width-int(18*s),int(bottom*s)};
            DrawTextW(surface.memory,value.c_str(),int(value.size()),&text,DT_WORDBREAK|DT_NOPREFIX|DT_END_ELLIPSIS);SelectObject(surface.memory,old);DeleteObject(font);
        };
        label(kind==3?L"Restoring playback":kind==5?L"Receiver unavailable":L"Connection needs attention",18,card.compact?66:48,card.compact?15:17,FW_SEMIBOLD,RGB(244,245,248));
        label(reason.empty()?L"Playback could not continue.":reason,card.compact?76:54,card.compact?176:158,12,FW_NORMAL,RGB(219,223,232));
        label(kind==3?L"Please wait. Your connection will recover if possible.":kind==5?L"Close and reopen AirMirrorLite to retry.":L"Press Escape to dismiss, then reconnect your iPhone.",card.compact?186:168,card.compact?234:208,11,FW_NORMAL,RGB(161,179,207));
        auto pixels=static_cast<uint32_t*>(surface.pixels);for(int i=0;i<width*height;++i)pixels[i]|=0xff000000;
        SIZE size{width,height};POINT zero{};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        if(UpdateLayeredWindow(status_panel_,nullptr,nullptr,&size,surface.memory,&zero,0,&blend,ULW_ALPHA)){painted_status_=reason;painted_status_kind_=kind;}
        HRGN region=CreateRoundRectRgn(0,0,width+1,height+1,int(24*s),int(24*s));if(region&&!SetWindowRgn(status_panel_,region,FALSE))DeleteObject(region);
    }
    SetWindowPos(status_panel_,HWND_TOP,x,y,width,height,SWP_NOACTIVATE);
    ShowWindow(status_panel_,!hidden_&&IsWindowVisible(window)&&!IsIconic(window)?SW_SHOWNOACTIVATE:SW_HIDE);accessibility_changed();
}
int PhoneShell::button_at(POINT p) const {
    float s=dpi_scale(),x=p.x/s-rail_x_,y=p.y/s;
    float ax=p.x/s-arrow_x_;
    if(std::abs(ax)<=control_layout::arrow_half_width && (controls_collapsed_?(y>=control_layout::panel_top&&y<=control_layout::panel_bottom):(y>=control_layout::arrow_top&&y<=control_layout::arrow_bottom)))return 4;
    if(controls_collapsed_||x<control_layout::button_left||x>control_layout::button_right)return -1;
    for(int i=0;i<4;++i)if(y>=control_layout::row_top(i)&&y<control_layout::row_bottom(i))return i;
    return -1;
}
bool PhoneShell::volume_zone(POINT p) const {
    if(button_at(p)==0)return true;if(!popup_)return false;
    float s=dpi_scale(),x=p.x/s,y=p.y/s;
    return y>=10&&y<=86&&x>=std::min(rail_x_+16,flyout_x_)&&x<=std::max(rail_x_+60,flyout_x_+FLYOUT_WIDTH);
}
bool PhoneShell::slider_hit(POINT p) const {
    float x=p.x/dpi_scale()-flyout_x_,y=p.y/dpi_scale();return popup_&&x>=12&&x<=200&&y>=42&&y<=74;
}
void PhoneShell::pointer_volume(int x){
    float logical=x/dpi_scale()-flyout_x_;set_volume(std::round(std::clamp((logical-24)/164,0.0f,1.0f)*100)/100);
}
void PhoneShell::update_tooltips(){
    if(!tooltip_)return;float s=dpi_scale();
    for(UINT_PTR i=0;i<4;++i){TOOLINFOW t{sizeof(t)};t.hwnd=palette_;t.uId=i+1;
        if(!controls_collapsed_)t.rect={int((rail_x_+control_layout::button_left)*s),int(control_layout::row_top(int(i))*s),int((rail_x_+control_layout::button_right)*s),int(control_layout::row_bottom(int(i))*s)};
        SendMessageW(tooltip_,TTM_NEWTOOLRECTW,0,reinterpret_cast<LPARAM>(&t));}
    TOOLINFOW arrow{sizeof(arrow)};arrow.hwnd=palette_;arrow.uId=5;
    arrow.rect={int((arrow_x_-control_layout::arrow_half_width)*s),int((controls_collapsed_?control_layout::panel_top:control_layout::arrow_top)*s),int((arrow_x_+control_layout::arrow_half_width)*s),int((controls_collapsed_?control_layout::panel_bottom:control_layout::arrow_bottom)*s)};
    SendMessageW(tooltip_,TTM_NEWTOOLRECTW,0,reinterpret_cast<LPARAM>(&arrow));
    arrow.lpszText=const_cast<LPWSTR>(controls_collapsed_?L"Show controls (C)":L"Hide controls (C)");SendMessageW(tooltip_,TTM_UPDATETIPTEXTW,0,reinterpret_cast<LPARAM>(&arrow));
    SendMessageW(tooltip_,TTM_ACTIVATE,popup_||hover_pending_?FALSE:TRUE,0);
}
HRESULT PhoneShell::paint_palette(const wchar_t* diagnostic_path,float diagnostic_scale){
    if(!palette_||!gdiplus_||!painter_||!palette_surface_)return E_UNEXPECTED;
    float s=diagnostic_scale>0?diagnostic_scale:dpi_scale();int width=int(palette_width_*s),height=int((BODY_HEIGHT+20)*s);
    if(width<1||height<1||width>2048||height>2048)return E_INVALIDARG;
    ControlArtwork artwork{s,float(rail_x_),float(flyout_x_),volume,float(arrow_x_),width,height,hot_,pressed_,keyboard_focus_,popup_,fullscreen,muted,controls_collapsed_,controls_collapsed_?!dock_right_:dock_right_};
    auto& surface=*palette_surface_;
    if(!diagnostic_path&&surface.valid&&surface.artwork==artwork){update_tooltips();accessibility_changed();return S_OK;}
    HRESULT result=surface.resize(width,height);if(FAILED(result))return result;
    surface.valid=false;result=painter_->draw(surface.memory,artwork);
    if(SUCCEEDED(result))++surface.draws;
    if(SUCCEEDED(result)&&diagnostic_path){
        static const CLSID png={0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};
        Bitmap bitmap(width,height,width*4,PixelFormat32bppPARGB,static_cast<BYTE*>(surface.pixels));if(bitmap.Save(diagnostic_path,&png,nullptr)!=Ok)result=E_FAIL;
    }
    if(SUCCEEDED(result)&&!diagnostic_path){SIZE size{width,height};POINT source{};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        if(!UpdateLayeredWindow(palette_,nullptr,nullptr,&size,surface.memory,&source,0,&blend,ULW_ALPHA))result=HRESULT_FROM_WIN32(GetLastError());
        else{surface.artwork=artwork;surface.valid=true;}}
    update_tooltips();accessibility_changed();return result;
}
void PhoneShell::menu(POINT screen){
    HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,1,L"Default size\t0");AppendMenuW(menu,MF_STRING,2,L"Fullscreen\tF11");AppendMenuW(menu,MF_STRING,3,L"Disconnect\tD");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,4,L"Close\tAlt+F4");
    UINT action=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,screen.x,screen.y,0,window,nullptr);DestroyMenu(menu);
    if(action==1)default_size();if(action==2)toggle_fullscreen();if(action==3){disconnect=true;signal();}if(action==4){quit=true;signal();}
}
void PhoneShell::accessibility_changed(){if(accessibility_)accessibility_->changed();}
LRESULT PhoneShell::accessibility_request(AccessibilityRequest& request){
    if(request.id<0||request.id>=9){request.result=E_INVALIDARG;return 0;}
    if(request.operation==AccessibilityRequest::Snapshot){
        auto& s=request.snapshot;s.window=window;s.palette=palette_;s.status_window=status_panel_;s.collapsed=controls_collapsed_;s.muted=muted;s.fullscreen=fullscreen;s.volume=volume*100;
        s.focus=keyboard_focus_>=0?keyboard_focus_+1:0;s.window_focused=GetFocus()==window;s.status=status;s.reason=status_message();
        GetWindowRect(window,&s.bounds[0]);RECT palette{};if(palette_)GetWindowRect(palette_,&palette);const float scale=dpi_scale();
        const bool shown=!hidden_&&IsWindowVisible(window)&&!IsIconic(window);s.visible[0]=shown;s.bounds[8]=palette;s.visible[8]=shown;
        for(int id=1;id<=4;++id){s.bounds[id]={palette.left+int((rail_x_+control_layout::button_left)*scale),palette.top+int(control_layout::row_top(id-1)*scale),palette.left+int((rail_x_+control_layout::button_right)*scale),palette.top+int(control_layout::row_bottom(id-1)*scale)};s.visible[id]=shown&&!controls_collapsed_;}
        s.bounds[5]={palette.left+int((arrow_x_-control_layout::arrow_half_width)*scale),palette.top+int(control_layout::arrow_top*scale),palette.left+int((arrow_x_+control_layout::arrow_half_width)*scale),palette.top+int(control_layout::arrow_bottom*scale)};s.visible[5]=shown;
        s.bounds[6]={palette.left+int((flyout_x_+12)*scale),palette.top+int(42*scale),palette.left+int((flyout_x_+200)*scale),palette.top+int(74*scale)};s.visible[6]=shown&&popup_&&!controls_collapsed_;
        if(status_panel_)GetWindowRect(status_panel_,&s.bounds[7]);s.visible[7]=shown&&s.status>=3;
        if(shown)UnionRect(&s.bounds[0],&s.bounds[0],&palette);if(s.visible[7])UnionRect(&s.bounds[0],&s.bounds[0],&s.bounds[7]);
    }else if(request.operation==AccessibilityRequest::Invoke){
        if(request.id<1||request.id>5||(controls_collapsed_&&request.id!=5)){request.result=UIA_E_ELEMENTNOTAVAILABLE;return 0;}
        invoke(request.id-1);
    }else if(request.operation==AccessibilityRequest::Volume){
        if(request.id!=6||!std::isfinite(request.value)||request.value<0||request.value>100){request.result=E_INVALIDARG;return 0;}
        set_volume(float(request.value/100));
    }else if(request.operation==AccessibilityRequest::Focus){
        if(request.id<0||request.id>6){request.result=UIA_E_INVALIDOPERATION;return 0;}
        if(request.id==0)dismiss_volume();else if(controls_collapsed_&&request.id!=5)toggle_controls();
        keyboard_focus_=request.id-1;if(request.id==6)popup_=true;if(!hidden_)SetFocus(window);place_palette();
    }
    request.result=S_OK;return 1;
}
LRESULT CALLBACK PhoneShell::main_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
    auto a=reinterpret_cast<PhoneShell*>(GetWindowLongPtrW(h,GWLP_USERDATA));
    if(msg==WM_NCCREATE){a=static_cast<PhoneShell*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(a));}
    if(!a)return DefWindowProcW(h,msg,wp,lp);
    switch(msg){
    case WM_NCCALCSIZE:return 0;
    case WM_TEST:return a->test_on_ui_thread(reinterpret_cast<const wchar_t*>(lp));
    case WM_SHUTDOWN:if(a->accessibility_)a->accessibility_->detach();if(a->tooltip_)DestroyWindow(a->tooltip_);if(a->palette_)DestroyWindow(a->palette_);if(a->status_panel_)DestroyWindow(a->status_panel_);DestroyWindow(h);PostQuitMessage(0);return 0;
    case WM_GETOBJECT:if(lp==UiaRootObjectId&&a->accessibility_)return a->accessibility_->return_provider(h,wp,lp);break;
    case WM_ACCESSIBILITY:if(wp==1)return a->accessibility_?a->accessibility_->dispatch_pending(reinterpret_cast<void*>(lp)):0;return lp?a->accessibility_request(*reinterpret_cast<AccessibilityRequest*>(lp)):0;
    case WM_SETFOCUS:case WM_KILLFOCUS:a->accessibility_changed();break;
    case WM_SHOW:a->hidden_=false;ShowWindow(h,SW_SHOW);a->place_palette();return 0;
    case WM_ACTIVATEAPP:if(!wp)a->dismiss_volume();break;
    case WM_CLOSE:a->quit=true;a->signal();return 0;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{PAINTSTRUCT paint;BeginPaint(h,&paint);EndPaint(h,&paint);a->signal();return 0;}
    case WM_SIZE:a->shape_window();a->place_palette();a->signal();return 0;
    case WM_MOVE:a->place_palette();return 0;
    case WM_DPICHANGED:{auto r=reinterpret_cast<RECT*>(lp);SetWindowPos(h,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);a->update_icons();a->shape_window();a->place_palette();a->signal();return 0;}
    case WM_NCHITTEST:{POINT pt{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(h,&pt);RECT r;GetClientRect(h,&r);float s=a->dpi_scale();
        if(!a->fullscreen && pt.x>r.right-int(36*s)&&pt.y>r.bottom-int(36*s))return HTBOTTOMRIGHT;
        return a->fullscreen?HTCLIENT:HTCAPTION;}
    case WM_ENTERSIZEMOVE:GetWindowRect(h,&a->size_start_);a->dismiss_volume();return 0;
    case WM_SIZING:{if(a->fullscreen)break;auto r=reinterpret_cast<RECT*>(lp);float s=a->dpi_scale(),border=float(2*rim_pixels(s));
        float w=float(r->right-r->left),height=float(r->bottom-r->top);
        float dw=std::abs(w-(a->size_start_.right-a->size_start_.left))/std::max(1L,a->size_start_.right-a->size_start_.left);
        float dh=std::abs(height-(a->size_start_.bottom-a->size_start_.top))/std::max(1L,a->size_start_.bottom-a->size_start_.top);
        if(dw>dh)height=(w-border)/a->aspect_+border;else w=(height-border)*a->aspect_+border;
        float min_h=a->aspect_<1?360*s:180*s;float max_h=std::min(3900.0f,float(work_area(h).bottom-work_area(h).top-16*s));
        height=std::clamp(height,std::min(min_h,max_h),max_h);w=(height-border)*a->aspect_+border;
        float max_w=std::min(3900.0f,float(work_area(h).right-work_area(h).left)-WORK_MARGIN*s-rail_clearance(s));
        if(w>max_w){w=max_w;height=(w-border)/a->aspect_+border;}
        r->right=r->left+LONG(w);r->bottom=r->top+LONG(height);a->user_sized_=true;return TRUE;}
    case WM_GETMINMAXINFO:{auto m=reinterpret_cast<MINMAXINFO*>(lp);float s=a->dpi_scale();float height=a->aspect_<1?360*s:180*s;m->ptMinTrackSize={LONG((height-2*rim_pixels(s))*a->aspect_+2*rim_pixels(s)),LONG(height)};m->ptMaxTrackSize={4096,4096};return 0;}
    case WM_SOURCE_RESET:
        if(wp>a->ui_generation_){a->ui_generation_=wp;a->session_shape_=0;}
        return 0;
    case WM_SOURCE_FORMAT:{
        if(wp<a->ui_generation_)return 0;
        if(wp>a->ui_generation_){a->ui_generation_=wp;a->session_shape_=0;}
        const UINT width=LOWORD(lp),height=HIWORD(lp);const float next=float(width)/float(height);
        if(!width||!height||width>3840||height>3840||next<=.2f||next>=5.0f)return 0;
        const float shape=std::min(next,1.0f/next);
        if(a->session_shape_==0)a->session_shape_=shape;
        // AirPlay Photos/video may replace screen-shaped output with media of a
        // different aspect. Keep the established shell and let the GPU fit it.
        // Matching ratios can still change resolution or rotate the phone view.
        if(std::abs(shape-a->session_shape_)>a->session_shape_*.01f)return 0;
        return main_proc(h,WM_FORMAT,width,height);}
    // UI geometry application, also used directly by hidden artwork diagnostics.
    case WM_FORMAT:{float next=float(wp)/float(lp);if(std::isfinite(next)&&next>.2f&&next<5.0f&&wp<=3840&&lp<=3840){
        bool changed=a->display_w_!=wp||a->display_h_!=UINT(lp);RECT r;GetWindowRect(h,&r);
        a->aspect_=next;a->display_w_=UINT(wp);a->display_h_=UINT(lp);
        if(changed){a->dismiss_volume();if(!a->fullscreen){if(!a->user_sized_)a->default_size();else{r=a->fitted_rect(r);SetWindowPos(h,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);}}}
        }a->signal();a->paint_palette();return 0;}
    case WM_REFRESH:a->paint_palette();a->paint_status_panel();return 0;
    case WM_NCLBUTTONDBLCLK:if(wp==HTCAPTION){a->toggle_fullscreen();return 0;}break;
    case WM_LBUTTONDBLCLK:a->toggle_fullscreen();return 0;
    case WM_SYSCOMMAND:if(toggles_fullscreen(wp,a->fullscreen,IsIconic(h)!=FALSE)){a->toggle_fullscreen();return 0;}break;
    case WM_NCRBUTTONUP:case WM_RBUTTONUP:{POINT p;GetCursorPos(&p);a->menu(p);return 0;}
    case WM_KEYDOWN:
        if(wp==VK_F11 && !(lp&(1LL<<30))){a->toggle_fullscreen();return 0;}
        if(wp=='M' && !(lp&(1LL<<30))){a->toggle_mute();return 0;}
        if(wp=='C'&&!(lp&(1LL<<30))){a->toggle_controls();return 0;}
        if(wp=='0'||wp==VK_NUMPAD0){a->default_size();return 0;}
        if(wp==VK_UP||wp==VK_DOWN){const float step=a->keyboard_focus_==5?.01f:.05f;a->set_volume(a->volume+(wp==VK_UP?step:-step));return 0;}
        if(a->keyboard_focus_==5&&(wp==VK_HOME||wp==VK_END||wp==VK_PRIOR||wp==VK_NEXT)){a->set_volume(wp==VK_HOME?0:wp==VK_END?1:a->volume+(wp==VK_PRIOR?.1f:-.1f));return 0;}
        if(wp==VK_TAB){int count=a->popup_?6:5;a->keyboard_focus_=a->controls_collapsed_?4:a->keyboard_focus_<0?(GetKeyState(VK_SHIFT)<0?count-1:0):(a->keyboard_focus_+(GetKeyState(VK_SHIFT)<0?count-1:1))%count;a->paint_palette();return 0;}
        if((wp==VK_RETURN||wp==VK_SPACE)&&a->keyboard_focus_>=0){if(a->keyboard_focus_<5&&!(lp&(1LL<<30)))a->invoke(a->keyboard_focus_);return 0;}
        if(wp==VK_ESCAPE){if(a->dismiss_error())return 0;if(a->popup_||a->hover_pending_){a->dismiss_volume();return 0;}if(a->fullscreen)a->toggle_fullscreen();else{a->disconnect=true;a->signal();}return 0;}
        if((wp==VK_LEFT||wp==VK_RIGHT)&&(a->keyboard_focus_==0||a->keyboard_focus_==5)&&!a->controls_collapsed_){a->popup_=true;a->place_palette();a->set_volume(a->volume+(wp==VK_RIGHT?.01f:-.01f));return 0;}
        if(wp=='D'){a->dismiss_error();a->disconnect=true;a->signal();return 0;}break;
    }
    return DefWindowProcW(h,msg,wp,lp);
}
LRESULT CALLBACK PhoneShell::palette_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
    auto a=reinterpret_cast<PhoneShell*>(GetWindowLongPtrW(h,GWLP_USERDATA));
    if(msg==WM_NCCREATE){a=static_cast<PhoneShell*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(a));}
    if(!a)return DefWindowProcW(h,msg,wp,lp);
    switch(msg){
    case WM_GETOBJECT:if(lp==UiaRootObjectId&&a->accessibility_)return a->accessibility_->return_provider(h,wp,lp);break;
    case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
    case WM_SETCURSOR:SetCursor(LoadCursorW(nullptr,IDC_HAND));return TRUE;
    case WM_MOUSEMOVE:{POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        if(!a->tracking_){TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,h,0};TrackMouseEvent(&track);a->tracking_=true;}
        if(a->dragging_slider_){a->pointer_volume(point.x);return 0;}
        int hot=a->button_at(point);bool changed=hot!=a->hot_;a->hot_=hot;
        a->update_volume_timer(a->volume_zone(point));
        if(hot==0&&!a->popup_&&!a->hover_pending_&&a->pressed_<0){
            UINT delay=400;SystemParametersInfoW(SPI_GETMOUSEHOVERTIME,0,&delay,0);TRACKMOUSEEVENT hover{sizeof(hover),TME_HOVER,h,std::clamp(delay,200u,1000u)};
            TrackMouseEvent(&hover);a->hover_pending_=true;a->update_tooltips();
        }else if(hot!=0&&a->hover_pending_){TRACKMOUSEEVENT cancel{sizeof(cancel),TME_CANCEL|TME_HOVER,h,0};TrackMouseEvent(&cancel);a->hover_pending_=false;}
        if(changed)a->paint_palette();return 0;}
    case WM_MOUSEHOVER:{bool pending=a->hover_pending_;a->hover_pending_=false;POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        if(pending&&a->button_at(p)==0&&a->pressed_<0&&!a->dragging_slider_){a->popup_=true;a->place_palette();}return 0;}
    case WM_MOUSELEAVE:a->tracking_=false;a->hover_pending_=false;a->hot_=-1;a->update_volume_timer(false);a->paint_palette();return 0;
    case WM_TIMER:if(wp==HIDE_VOLUME&&a->hide_volume_pending_&&GetTickCount64()>=a->hide_volume_deadline_){KillTimer(h,HIDE_VOLUME);a->hide_volume_pending_=false;a->hide_volume_deadline_=0;POINT p;GetCursorPos(&p);ScreenToClient(h,&p);if(!a->volume_zone(p)&&!a->dragging_slider_)a->dismiss_volume();}return 0;
    case WM_LBUTTONDOWN:{POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};a->pressed_=a->button_at(p);
        TRACKMOUSEEVENT cancel{sizeof(cancel),TME_CANCEL|TME_HOVER,h,0};TrackMouseEvent(&cancel);a->hover_pending_=false;
        a->dragging_slider_=a->slider_hit(p);if(a->pressed_>=0||a->dragging_slider_){if(!a->hidden_)SetCapture(h);}
        if(a->dragging_slider_)a->pointer_volume(p.x);a->paint_palette();return 0;}
    case WM_LBUTTONUP:{POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};int hit=a->button_at(p);bool slider=a->dragging_slider_;int pressed=a->pressed_;a->dragging_slider_=false;a->pressed_=-1;if(GetCapture()==h)ReleaseCapture();if(!slider&&hit==pressed&&hit>=0)a->invoke(hit);a->paint_palette();return 0;}
    case WM_CAPTURECHANGED:if(a->dragging_slider_||a->pressed_>=0)a->dismiss_volume();return 0;
    case WM_CANCELMODE:a->dismiss_volume();return 0;
    case WM_MOUSEWHEEL:{
        // High-resolution wheels send fractions of WHEEL_DELTA. Accumulate them
        // instead of treating every tiny (or zero) delta as a full volume step.
        a->wheel_delta_+=GET_WHEEL_DELTA_WPARAM(wp);
        const int steps=a->wheel_delta_/WHEEL_DELTA;a->wheel_delta_%=WHEEL_DELTA;
        if(steps)a->set_volume(a->volume+steps*.05f);return 0;}
    case WM_CLOSE:a->quit=true;a->signal();return 0;
    }
    return DefWindowProcW(h,msg,wp,lp);
}

LRESULT CALLBACK PhoneShell::status_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
    auto shell=reinterpret_cast<PhoneShell*>(GetWindowLongPtrW(h,GWLP_USERDATA));
    if(msg==WM_NCCREATE){shell=static_cast<PhoneShell*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(shell));}
    if(msg==WM_GETOBJECT&&lp==UiaRootObjectId&&shell&&shell->accessibility_)return shell->accessibility_->return_provider(h,wp,lp);
    return DefWindowProcW(h,msg,wp,lp);
}

#include "phone_shell_test.inc"
