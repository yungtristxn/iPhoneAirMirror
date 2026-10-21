#include "phone_shell.h"
#include <windowsx.h>
#include <gdiplus.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <future>
#include "control_artwork.h"
#include "resources.h"
using namespace Gdiplus;
namespace {
constexpr DWORD WINDOW_STYLE=WS_POPUP|WS_THICKFRAME|WS_SYSMENU|WS_MINIMIZEBOX;
constexpr int PALETTE_WIDTH=96, BODY_HEIGHT=196, BUTTON_STEP=44, FLYOUT_WIDTH=212, FLYOUT_SPACE=224;
constexpr UINT_PTR HIDE_VOLUME=1;
constexpr wchar_t const* labels[]={L"Mute / unmute (M) — hover for volume",L"Fit at source resolution (0)",L"Fullscreen (F11)",L"Close AirMirrorLite (Alt+F4)",L"Hide controls (C)"};
RECT work_area(HWND hwnd){MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&mi);return mi.rcWork;}
}
PhoneShell::PhoneShell()=default;
PhoneShell::~PhoneShell(){shutdown();}
float PhoneShell::corner_radius(UINT width,UINT height,float s){return std::clamp(float(std::min(width,height))*.105f,20*s,36*s);}
float PhoneShell::dpi_scale() const {return window?GetDpiForWindow(window)/96.0f:1.0f;}
void PhoneShell::signal(){redraw=true;chrome_dirty=true;if(wake_)SetEvent(wake_);}
HRESULT PhoneShell::create(HANDLE wake,bool hidden){
    wake_=wake;hidden_=hidden;std::promise<HWND> created;auto ready=created.get_future();
    thread_=std::thread([this,created=std::move(created)]() mutable {
        GdiplusStartupInput startup;if(GdiplusStartup(&gdiplus_,&startup,nullptr)!=Ok){created.set_value(nullptr);return;}
        painter_=std::make_unique<ControlPainter>();
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
        WNDCLASSW wc{};wc.lpfnWndProc=main_proc;wc.hInstance=GetModuleHandleW(nullptr);wc.hIcon=LoadIconW(wc.hInstance,MAKEINTRESOURCEW(IDI_AIRMIRRORLITE));wc.style=CS_DBLCLKS;wc.lpszClassName=L"AirMirrorLiteWindow";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&wc);
        WNDCLASSW pc=wc;pc.lpfnWndProc=palette_proc;pc.lpszClassName=L"AirMirrorLiteControls";RegisterClassW(&pc);
        window=CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP,wc.lpszClassName,L"AirMirrorLite",WINDOW_STYLE,0,0,360,760,nullptr,nullptr,wc.hInstance,this);
        if(window){
            update_icons();
            BOOL dark=TRUE;DwmSetWindowAttribute(window,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
            DWORD border=DWMWA_COLOR_NONE;DwmSetWindowAttribute(window,DWMWA_BORDER_COLOR,&border,sizeof(border));
            palette_=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,pc.lpszClassName,L"AirMirrorLite quick controls",WS_POPUP,0,0,72,220,window,nullptr,wc.hInstance,this);
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
        if(small_icon_)DestroyIcon(small_icon_);if(large_icon_)DestroyIcon(large_icon_);small_icon_=large_icon_=nullptr;
        if(gdiplus_){GdiplusShutdown(gdiplus_);gdiplus_=0;}
    });
    HWND result=ready.get();return result&&palette_&&tooltip_?initial_paint_:E_FAIL;
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
void PhoneShell::set_status(const std::wstring& title){
    SetWindowTextW(window,title.c_str());
    int state=has_video?2:title.find(L"Connect")!=std::wstring::npos?1:0;
    if(status.exchange(state)!=state){chrome_dirty=true;PostMessageW(window,WM_REFRESH,0,0);if(wake_)SetEvent(wake_);}
}
void PhoneShell::video_size(UINT w,UINT h){
    if(!w||!h)return;
    if(!has_video.exchange(true)){status=2;chrome_dirty=true;signal();}
    if(published_w_!=w||published_h_!=h){published_w_=w;published_h_=h;PostMessageW(window,WM_FORMAT,w,h);}
}
void PhoneShell::reset_video(){has_video=false;status=0;published_w_=published_h_=0;signal();PostMessageW(window,WM_REFRESH,0,0);}
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
    float fit=std::min({1.0f,std::max(1.0f,float(area.right-area.left)-110*s-2*border)/source_w,std::max(1.0f,float(area.bottom-area.top)-48*s-2*border)/source_h});
    int w=int(std::round(source_w*fit))+2*border,h=int(std::round(source_h*fit))+2*border;
    int x=center?area.left+((area.right-area.left)-w-int(84*s))/2:old.left+(old.right-old.left-w)/2;
    int y=center?area.top+((area.bottom-area.top)-h)/2:old.top+(old.bottom-old.top-h)/2;
    x=std::clamp<LONG>(x,area.left+int(12*s),std::max(area.left+int(12*s),area.right-w-int(84*s)));
    y=std::clamp<LONG>(y,area.top+int(12*s),std::max(area.top+int(12*s),area.bottom-h-int(12*s)));
    user_sized_=false;SetWindowPos(window,nullptr,x,y,w,h,SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);signal();
}
RECT PhoneShell::fitted_rect(RECT original) const {
    const float s=dpi_scale(),border=float(2*rim_pixels(s));
    RECT work=work_area(window);
    float area=std::max(1.0f,float(original.right-original.left)-border)*std::max(1.0f,float(original.bottom-original.top)-border);
    float h=std::sqrt(area/aspect_),w=h*aspect_;
    float factor=std::min({1.0f,std::max(1.0f,float(work.right-work.left)-100*s-border)/w,std::max(1.0f,float(work.bottom-work.top)-24*s-border)/h});
    int width=int(w*factor+border),height=int(h*factor+border);
    int x=(original.left+original.right-width)/2,y=(original.top+original.bottom-height)/2;
    x=std::clamp<LONG>(x,work.left+int(12*s),std::max(work.left+int(12*s),work.right-width-int(84*s)));
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
}
void PhoneShell::toggle_controls(){
    dismiss_volume();controls_collapsed_=!controls_collapsed_;hot_=-1;pressed_=-1;
    if(keyboard_focus_>=0)keyboard_focus_=controls_collapsed_?4:0;
    place_palette();
}
void PhoneShell::dismiss_volume(){
    KillTimer(palette_,HIDE_VOLUME);TRACKMOUSEEVENT cancel{sizeof(cancel),TME_CANCEL|TME_HOVER,palette_,0};TrackMouseEvent(&cancel);
    hover_pending_=false;if(popup_){popup_=false;place_palette();}else update_tooltips();
}
void PhoneShell::place_palette(){
    if(!palette_)return;
    RECT r;GetWindowRect(window,&r);RECT area=work_area(window);float s=dpi_scale();int base_width=controls_collapsed_?36:PALETTE_WIDTH;int base_w=int(base_width*s),height=int((BODY_HEIGHT+20)*s);
    int x=fullscreen?r.right-base_w-int(12*s):r.right+int(4*s);
    // Choose the dock side using expanded width, so collapsing never flips sides.
    bool right=true;if(r.right+int((PALETTE_WIDTH+4)*s)>area.right&&!fullscreen){x=r.left-base_w-int(4*s);right=false;}
    dock_right_=right;int y=std::clamp<LONG>(r.top+(r.bottom-r.top-height)/2,area.top,std::max(area.top,area.bottom-height));
    bool extend_right=right;
    if(x+int((base_width+FLYOUT_SPACE)*s)>area.right)extend_right=false;
    if(x-int(FLYOUT_SPACE*s)<area.left)extend_right=true;
    int offset=popup_&&!extend_right?FLYOUT_SPACE:0;
    rail_x_=offset+(right?0:20);arrow_x_=offset+(controls_collapsed_?18:right?84:12);
    flyout_x_=extend_right?PALETTE_WIDTH+2:10;
    palette_width_=base_width+(popup_?FLYOUT_SPACE:0);
    SetWindowPos(palette_,nullptr,x-int(offset*s),y,int(palette_width_*s),height,SWP_NOZORDER|SWP_NOACTIVATE);
    paint_palette();
    if(!hidden_&&IsWindowVisible(window)&&!IsIconic(window))ShowWindow(palette_,SW_SHOWNOACTIVATE);else ShowWindow(palette_,SW_HIDE);
}
int PhoneShell::button_at(POINT p) const {
    float s=dpi_scale(),x=p.x/s-rail_x_,y=p.y/s;
    float ax=p.x/s-arrow_x_;
    if(std::abs(ax)<=12 && (controls_collapsed_?(y>=10&&y<=206):(y>=90&&y<=126)))return 4;
    if(controls_collapsed_||x<16||x>60)return -1;
    for(int i=0;i<4;++i)if(y>=18+i*BUTTON_STEP&&y<62+i*BUTTON_STEP)return i;
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
        if(!controls_collapsed_)t.rect={int((rail_x_+16)*s),int((18+i*BUTTON_STEP)*s),int((rail_x_+60)*s),int((62+i*BUTTON_STEP)*s)};
        SendMessageW(tooltip_,TTM_NEWTOOLRECTW,0,reinterpret_cast<LPARAM>(&t));}
    TOOLINFOW arrow{sizeof(arrow)};arrow.hwnd=palette_;arrow.uId=5;
    arrow.rect={int((arrow_x_-12)*s),int((controls_collapsed_?10:90)*s),int((arrow_x_+12)*s),int((controls_collapsed_?206:126)*s)};
    SendMessageW(tooltip_,TTM_NEWTOOLRECTW,0,reinterpret_cast<LPARAM>(&arrow));
    arrow.lpszText=const_cast<LPWSTR>(controls_collapsed_?L"Show controls (C)":L"Hide controls (C)");SendMessageW(tooltip_,TTM_UPDATETIPTEXTW,0,reinterpret_cast<LPARAM>(&arrow));
    SendMessageW(tooltip_,TTM_ACTIVATE,popup_||hover_pending_?FALSE:TRUE,0);
}
HRESULT PhoneShell::paint_palette(const wchar_t* diagnostic_path,float diagnostic_scale){
    if(!palette_||!gdiplus_)return E_UNEXPECTED;
    float s=diagnostic_scale>0?diagnostic_scale:dpi_scale();int width=int(palette_width_*s),height=int((BODY_HEIGHT+20)*s);
    if(width<1||height<1||width>2048||height>2048)return E_INVALIDARG;
    HDC screen=GetDC(nullptr),memory=CreateCompatibleDC(screen);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr;HBITMAP dib=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!dib||!bits){if(dib)DeleteObject(dib);DeleteDC(memory);ReleaseDC(nullptr,screen);return E_OUTOFMEMORY;}
    HGDIOBJ old=SelectObject(memory,dib);
    ControlArtwork artwork{s,float(rail_x_),float(flyout_x_),volume,float(arrow_x_),width,height,hot_,pressed_,keyboard_focus_,popup_,fullscreen,muted,controls_collapsed_,controls_collapsed_?!dock_right_:dock_right_};
    HRESULT result=painter_->draw(memory,artwork);
    if(SUCCEEDED(result)&&diagnostic_path){
        static const CLSID png={0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};
        Bitmap bitmap(width,height,width*4,PixelFormat32bppPARGB,static_cast<BYTE*>(bits));if(bitmap.Save(diagnostic_path,&png,nullptr)!=Ok)result=E_FAIL;
    }
    if(SUCCEEDED(result)&&!diagnostic_path){SIZE size{width,height};POINT source{};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        if(!UpdateLayeredWindow(palette_,screen,nullptr,&size,memory,&source,0,&blend,ULW_ALPHA))result=HRESULT_FROM_WIN32(GetLastError());}
    SelectObject(memory,old);DeleteObject(dib);DeleteDC(memory);ReleaseDC(nullptr,screen);update_tooltips();return result;
}
void PhoneShell::menu(POINT screen){
    HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,1,L"Default size\t0");AppendMenuW(menu,MF_STRING,2,L"Fullscreen\tF11");AppendMenuW(menu,MF_STRING,3,L"Disconnect\tD");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,4,L"Close\tAlt+F4");
    UINT action=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,screen.x,screen.y,0,window,nullptr);DestroyMenu(menu);
    if(action==1)default_size();if(action==2)toggle_fullscreen();if(action==3){disconnect=true;signal();}if(action==4){quit=true;signal();}
}
LRESULT CALLBACK PhoneShell::main_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
    auto a=reinterpret_cast<PhoneShell*>(GetWindowLongPtrW(h,GWLP_USERDATA));
    if(msg==WM_NCCREATE){a=static_cast<PhoneShell*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(a));}
    if(!a)return DefWindowProcW(h,msg,wp,lp);
    switch(msg){
    case WM_NCCALCSIZE:return 0;
    case WM_TEST:return a->test_on_ui_thread(reinterpret_cast<const wchar_t*>(lp));
    case WM_SHUTDOWN:if(a->tooltip_)DestroyWindow(a->tooltip_);if(a->palette_)DestroyWindow(a->palette_);DestroyWindow(h);PostQuitMessage(0);return 0;
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
        float max_w=std::min(3900.0f,float(work_area(h).right-work_area(h).left)-90*s);
        if(w>max_w){w=max_w;height=(w-border)/a->aspect_+border;}
        r->right=r->left+LONG(w);r->bottom=r->top+LONG(height);a->user_sized_=true;return TRUE;}
    case WM_GETMINMAXINFO:{auto m=reinterpret_cast<MINMAXINFO*>(lp);float s=a->dpi_scale();float height=a->aspect_<1?360*s:180*s;m->ptMinTrackSize={LONG((height-2*rim_pixels(s))*a->aspect_+2*rim_pixels(s)),LONG(height)};m->ptMaxTrackSize={4096,4096};return 0;}
    case WM_FORMAT:{float next=float(wp)/float(lp);if(std::isfinite(next)&&next>.2f&&next<5.0f&&wp<=3840&&lp<=3840){
        bool changed=a->display_w_!=wp||a->display_h_!=UINT(lp);RECT r;GetWindowRect(h,&r);
        a->aspect_=next;a->display_w_=UINT(wp);a->display_h_=UINT(lp);
        if(changed){a->dismiss_volume();if(!a->fullscreen){if(!a->user_sized_)a->default_size();else{r=a->fitted_rect(r);SetWindowPos(h,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);}}}
        }a->signal();a->paint_palette();return 0;}
    case WM_REFRESH:a->paint_palette();return 0;
    case WM_NCLBUTTONDBLCLK:if(wp==HTCAPTION){a->toggle_fullscreen();return 0;}break;
    case WM_LBUTTONDBLCLK:a->toggle_fullscreen();return 0;
    case WM_SYSCOMMAND:if((wp&0xfff0)==SC_MAXIMIZE || ((wp&0xfff0)==SC_RESTORE && a->fullscreen)){a->toggle_fullscreen();return 0;}break;
    case WM_NCRBUTTONUP:case WM_RBUTTONUP:{POINT p;GetCursorPos(&p);a->menu(p);return 0;}
    case WM_KEYDOWN:
        if(wp==VK_F11 && !(lp&(1LL<<30))){a->toggle_fullscreen();return 0;}
        if(wp=='M' && !(lp&(1LL<<30))){a->toggle_mute();return 0;}
        if(wp=='C'&&!(lp&(1LL<<30))){a->toggle_controls();return 0;}
        if(wp=='0'||wp==VK_NUMPAD0){a->default_size();return 0;}
        if(wp==VK_UP||wp==VK_DOWN){a->set_volume(a->volume+(wp==VK_UP?.05f:-.05f));return 0;}
        if(wp==VK_TAB){a->keyboard_focus_=a->controls_collapsed_?4:a->keyboard_focus_<0?(GetKeyState(VK_SHIFT)<0?4:0):(a->keyboard_focus_+(GetKeyState(VK_SHIFT)<0?4:1))%5;a->paint_palette();return 0;}
        if((wp==VK_RETURN||wp==VK_SPACE)&&a->keyboard_focus_>=0){a->invoke(a->keyboard_focus_);return 0;}
        if(wp==VK_ESCAPE){if(a->popup_||a->hover_pending_){a->dismiss_volume();return 0;}if(a->fullscreen)a->toggle_fullscreen();else{a->disconnect=true;a->signal();}return 0;}
        if((wp==VK_LEFT||wp==VK_RIGHT)&&a->keyboard_focus_==0&&!a->controls_collapsed_){a->popup_=true;a->place_palette();a->set_volume(a->volume+(wp==VK_RIGHT?.01f:-.01f));return 0;}
        if(wp=='D'){a->disconnect=true;a->signal();return 0;}break;
    }
    return DefWindowProcW(h,msg,wp,lp);
}
LRESULT CALLBACK PhoneShell::palette_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
    auto a=reinterpret_cast<PhoneShell*>(GetWindowLongPtrW(h,GWLP_USERDATA));
    if(msg==WM_NCCREATE){a=static_cast<PhoneShell*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(a));}
    if(!a)return DefWindowProcW(h,msg,wp,lp);
    switch(msg){
    case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
    case WM_SETCURSOR:SetCursor(LoadCursorW(nullptr,IDC_HAND));return TRUE;
    case WM_MOUSEMOVE:{POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        if(!a->tracking_){TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,h,0};TrackMouseEvent(&track);a->tracking_=true;}
        if(a->dragging_slider_){a->pointer_volume(point.x);return 0;}
        int hot=a->button_at(point);bool changed=hot!=a->hot_;a->hot_=hot;
        if(a->volume_zone(point))KillTimer(h,HIDE_VOLUME);else if(a->popup_&&changed)SetTimer(h,HIDE_VOLUME,350,nullptr);
        if(hot==0&&!a->popup_&&!a->hover_pending_&&a->pressed_<0){
            UINT delay=400;SystemParametersInfoW(SPI_GETMOUSEHOVERTIME,0,&delay,0);TRACKMOUSEEVENT hover{sizeof(hover),TME_HOVER,h,std::clamp(delay,200u,1000u)};
            TrackMouseEvent(&hover);a->hover_pending_=true;a->update_tooltips();
        }else if(hot!=0&&a->hover_pending_){TRACKMOUSEEVENT cancel{sizeof(cancel),TME_CANCEL|TME_HOVER,h,0};TrackMouseEvent(&cancel);a->hover_pending_=false;}
        if(changed)a->paint_palette();return 0;}
    case WM_MOUSEHOVER:{bool pending=a->hover_pending_;a->hover_pending_=false;POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        if(pending&&a->button_at(p)==0&&a->pressed_<0&&!a->dragging_slider_){a->popup_=true;a->place_palette();}return 0;}
    case WM_MOUSELEAVE:a->tracking_=false;a->hover_pending_=false;a->hot_=-1;if(!a->dragging_slider_)SetTimer(h,HIDE_VOLUME,350,nullptr);a->paint_palette();return 0;
    case WM_TIMER:if(wp==HIDE_VOLUME){POINT p;GetCursorPos(&p);ScreenToClient(h,&p);if(!a->volume_zone(p)&&!a->dragging_slider_)a->dismiss_volume();KillTimer(h,HIDE_VOLUME);}return 0;
    case WM_LBUTTONDOWN:{POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};a->pressed_=a->button_at(p);
        TRACKMOUSEEVENT cancel{sizeof(cancel),TME_CANCEL|TME_HOVER,h,0};TrackMouseEvent(&cancel);a->hover_pending_=false;
        a->dragging_slider_=a->slider_hit(p);if(a->pressed_>=0||a->dragging_slider_){if(!a->hidden_)SetCapture(h);}
        if(a->dragging_slider_)a->pointer_volume(p.x);a->paint_palette();return 0;}
    case WM_LBUTTONUP:{POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};int hit=a->button_at(p);bool slider=a->dragging_slider_;int pressed=a->pressed_;a->dragging_slider_=false;if(GetCapture()==h)ReleaseCapture();a->pressed_=-1;if(!slider&&hit==pressed&&hit>=0)a->invoke(hit);a->paint_palette();return 0;}
    case WM_CAPTURECHANGED:a->dragging_slider_=false;a->pressed_=-1;return 0;
    case WM_MOUSEWHEEL:a->set_volume(a->volume+(GET_WHEEL_DELTA_WPARAM(wp)>0?.05f:-.05f));return 0;
    case WM_CLOSE:a->quit=true;a->signal();return 0;
    }
    return DefWindowProcW(h,msg,wp,lp);
}

#include "phone_shell_test.inc"
