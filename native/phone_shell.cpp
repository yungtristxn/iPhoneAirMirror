#include "phone_shell.h"
#include <windowsx.h>
#include <gdiplus.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <future>
using namespace Gdiplus;
namespace {
constexpr DWORD WINDOW_STYLE=WS_POPUP|WS_THICKFRAME|WS_SYSMENU|WS_MINIMIZEBOX;
constexpr int PALETTE_WIDTH=72, BODY_WIDTH=52, BODY_HEIGHT=200, BUTTON_STEP=44;
constexpr UINT_PTR HIDE_VOLUME=1;
constexpr wchar_t const* labels[]={L"Mute / unmute (M)",L"Default size (0)",L"Fullscreen (F11)",L"Close AirMirrorLite (Alt+F4)"};
void rounded(GraphicsPath& path,RectF r,float radius){
    float d=radius*2;path.AddArc(r.X,r.Y,d,d,180,90);path.AddArc(r.GetRight()-d,r.Y,d,d,270,90);
    path.AddArc(r.GetRight()-d,r.GetBottom()-d,d,d,0,90);path.AddArc(r.X,r.GetBottom()-d,d,d,90,90);path.CloseFigure();
}
void line(Graphics& g,Pen& pen,float x1,float y1,float x2,float y2){g.DrawLine(&pen,x1,y1,x2,y2);}
void icon(Graphics& g,int which,float x,float y,bool active,bool silent){
    Pen p(Color(245,232,235,240),1.7f);p.SetStartCap(LineCapRound);p.SetEndCap(LineCapRound);p.SetLineJoin(LineJoinRound);
    if(which==0){
        GraphicsPath speaker;PointF points[]={{x-10,y-4},{x-6,y-4},{x,y-9},{x,y+9},{x-6,y+4},{x-10,y+4}};
        speaker.AddPolygon(points,6);g.DrawPath(&p,&speaker);
        if(silent){line(g,p,x+5,y-4,x+11,y+4);line(g,p,x+11,y-4,x+5,y+4);}
        else {g.DrawArc(&p,x-1,y-6,12.0f,12.0f,-55.0f,110.0f);g.DrawArc(&p,x-1,y-10,19.0f,20.0f,-52.0f,104.0f);}
    }else if(which==1){
        g.DrawArc(&p,x-9,y-9,18.0f,18.0f,-65.0f,285.0f);line(g,p,x+8,y-10,x+8,y-4);line(g,p,x+8,y-4,x+2,y-5);
        Pen inner(Color(190,232,235,240),1.3f);g.DrawRectangle(&inner,x-3.5f,y-4,7.0f,8.0f);
    }else if(which==2){
        float k=active?4.0f:9.0f, e=active?9.0f:4.0f;
        for(float sx:{-1.0f,1.0f})for(float sy:{-1.0f,1.0f}){
            line(g,p,x+sx*k,y+sy*e,x+sx*k,y+sy*k);line(g,p,x+sx*k,y+sy*k,x+sx*e,y+sy*k);
        }
    }else{line(g,p,x-6,y-6,x+6,y+6);line(g,p,x+6,y-6,x-6,y+6);}
}
RECT work_area(HWND hwnd){MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&mi);return mi.rcWork;}
}
float PhoneShell::dpi_scale() const {return window?GetDpiForWindow(window)/96.0f:1.0f;}
void PhoneShell::signal(){redraw=true;chrome_dirty=true;if(wake_)SetEvent(wake_);}
HRESULT PhoneShell::create(HANDLE wake,bool hidden){
    wake_=wake;hidden_=hidden;std::promise<HWND> created;auto ready=created.get_future();
    thread_=std::thread([this,created=std::move(created)]() mutable {
        GdiplusStartupInput startup;if(GdiplusStartup(&gdiplus_,&startup,nullptr)!=Ok){created.set_value(nullptr);return;}
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
        WNDCLASSW wc{};wc.lpfnWndProc=main_proc;wc.hInstance=GetModuleHandleW(nullptr);wc.style=CS_DBLCLKS;wc.lpszClassName=L"AirMirrorLiteWindow";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&wc);
        WNDCLASSW pc=wc;pc.lpfnWndProc=palette_proc;pc.lpszClassName=L"AirMirrorLiteControls";RegisterClassW(&pc);
        window=CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP,wc.lpszClassName,L"AirMirrorLite",WINDOW_STYLE,0,0,360,760,nullptr,nullptr,wc.hInstance,this);
        if(window){
            BOOL dark=TRUE;DwmSetWindowAttribute(window,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
            DWORD border=DWMWA_COLOR_NONE;DwmSetWindowAttribute(window,DWMWA_BORDER_COLOR,&border,sizeof(border));
            palette_=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,pc.lpszClassName,L"AirMirrorLite quick controls",WS_POPUP,0,0,72,220,window,nullptr,wc.hInstance,this);
            tooltip_=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,palette_,nullptr,wc.hInstance,nullptr);
            SendMessageW(tooltip_,TTM_SETDELAYTIME,TTDT_INITIAL,450);SendMessageW(tooltip_,TTM_SETMAXTIPWIDTH,0,250);
            for(UINT_PTR i=0;i<4;++i){TOOLINFOW t{sizeof(t)};t.uFlags=TTF_SUBCLASS;t.hwnd=palette_;t.uId=i+1;t.lpszText=const_cast<LPWSTR>(labels[i]);SendMessageW(tooltip_,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&t));}
            POINT cursor;GetCursorPos(&cursor);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST),&mi);
            SetWindowPos(window,nullptr,mi.rcWork.left+40,mi.rcWork.top+40,360,760,SWP_NOZORDER|SWP_NOACTIVATE);
            default_size(true);
        }
        created.set_value(window);
        if(window){MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}}
        if(gdiplus_){GdiplusShutdown(gdiplus_);gdiplus_=0;}
    });
    HWND result=ready.get();return result&&palette_&&tooltip_?S_OK:E_FAIL;
}
void PhoneShell::show(){PostMessageW(window,WM_SHOW,0,0);}
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
    RECT r;GetClientRect(window,&r);float s=dpi_scale();int radius=int(std::clamp(float(std::min(r.right,r.bottom))*.115f,22*s,42*s));
    HRGN region=CreateRoundRectRgn(0,0,r.right+1,r.bottom+1,std::max(2,2*radius-4),std::max(2,2*radius-4));
    if(region && !SetWindowRgn(window,region,TRUE))DeleteObject(region);
}
void PhoneShell::default_size(bool center){
    if(fullscreen)toggle_fullscreen();
    RECT area=work_area(window),old{};GetWindowRect(window,&old);float s=dpi_scale();int border=int(std::round(4*s));
    float long_side=std::min(760*s,float(aspect_<1?area.bottom-area.top:area.right-area.left)*.78f);
    int h=int(aspect_<1?long_side:long_side/aspect_)+2*border;
    int w=int(aspect_<1?long_side*aspect_:long_side)+2*border;
    int x=center?area.left+((area.right-area.left)-w-int(84*s))/2:old.left+(old.right-old.left-w)/2;
    int y=center?area.top+((area.bottom-area.top)-h)/2:old.top+(old.bottom-old.top-h)/2;
    x=std::clamp<LONG>(x,area.left+int(12*s),std::max(area.left+int(12*s),area.right-w-int(84*s)));
    y=std::clamp<LONG>(y,area.top+int(12*s),std::max(area.top+int(12*s),area.bottom-h-int(12*s)));
    user_sized_=false;SetWindowPos(window,nullptr,x,y,w,h,SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);signal();
}
RECT PhoneShell::fitted_rect(RECT original) const {
    const float s=dpi_scale(),border=2*std::round(4*s);
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
    popup_=false;keyboard_focus_=-1;
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
    if(button==0)toggle_mute();else if(button==1)default_size();else if(button==2)toggle_fullscreen();else if(button==3){quit=true;if(wake_)SetEvent(wake_);}
}
void PhoneShell::place_palette(){
    if(!palette_)return;
    RECT r;GetWindowRect(window,&r);RECT area=work_area(window);float s=dpi_scale();int w=int(PALETTE_WIDTH*s),base=int((BODY_HEIGHT+20)*s);
    int x=fullscreen?r.right-w-int(12*s):r.right+int(4*s);
    if(x+w>area.right && !fullscreen)x=r.left-w-int(4*s);
    int y=r.top+(r.bottom-r.top-base)/2;
    // Reserve slider headroom even while collapsed, so opening it never moves
    // the speaker away from the pointer when the phone is near the screen edge.
    LONG top=area.top+std::min<LONG>(int(140*s),std::max(0L,area.bottom-area.top-base));
    y=std::clamp<LONG>(y,top,std::max(top,area.bottom-base));
    popup_height_=popup_?140:0;
    int extra=int(popup_height_*s);
    SetWindowPos(palette_,nullptr,x,y-extra,w,base+extra,SWP_NOZORDER|SWP_NOACTIVATE);
    paint_palette();
    if(!hidden_ && IsWindowVisible(window) && !IsIconic(window))ShowWindow(palette_,SW_SHOWNOACTIVATE);else ShowWindow(palette_,SW_HIDE);
}
int PhoneShell::button_at(POINT point) const {
    float s=dpi_scale(),x=point.x/s,y=point.y/s-popup_height_;
    if(x<14||x>58)return -1;
    for(int i=0;i<4;++i){float top=20.0f+i*BUTTON_STEP;if(y>=top&&y<top+40)return i;}
    return -1;
}
void PhoneShell::pointer_volume(int y){
    float logical=y/dpi_scale();float top=46,bottom=float(popup_height_)+2;
    if(bottom<=top)return;set_volume(std::round(std::clamp((bottom-logical)/(bottom-top),0.0f,1.0f)*100)/100);
}
void PhoneShell::update_tooltips(){
    if(!tooltip_)return;float s=dpi_scale();
    for(UINT_PTR i=0;i<4;++i){TOOLINFOW t{sizeof(t)};t.hwnd=palette_;t.uId=i+1;
        t.rect={int(14*s),int((popup_height_+20+i*BUTTON_STEP)*s),int(58*s),int((popup_height_+60+i*BUTTON_STEP)*s)};SendMessageW(tooltip_,TTM_NEWTOOLRECTW,0,reinterpret_cast<LPARAM>(&t));}
    SendMessageW(tooltip_,TTM_ACTIVATE,popup_?FALSE:TRUE,0);
}
HRESULT PhoneShell::paint_palette(const wchar_t* diagnostic_path){
    if(!palette_||!gdiplus_)return E_UNEXPECTED;
    float s=dpi_scale();int width=int(PALETTE_WIDTH*s),height=int((BODY_HEIGHT+20+popup_height_)*s);
    if(width<1||height<1||width>1024||height>2048)return E_INVALIDARG;
    HDC screen=GetDC(nullptr),memory=CreateCompatibleDC(screen);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr;HBITMAP dib=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!dib||!bits){if(dib)DeleteObject(dib);DeleteDC(memory);ReleaseDC(nullptr,screen);return E_OUTOFMEMORY;}
    HGDIOBJ old=SelectObject(memory,dib);
    HRESULT result=S_OK;
    {
        Bitmap bitmap(width,height,width*4,PixelFormat32bppPARGB,static_cast<BYTE*>(bits));Graphics g(&bitmap);
        g.Clear(Color(0,0,0,0));g.ScaleTransform(s,s);g.SetSmoothingMode(SmoothingModeAntiAlias);g.SetPixelOffsetMode(PixelOffsetModeHighQuality);g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        RectF body(10,10,52,float(BODY_HEIGHT+popup_height_));
        for(int spread=7;spread>=1;--spread){GraphicsPath shadow;rounded(shadow,RectF(body.X-spread*.65f,body.Y+2-spread*.35f,body.Width+spread*1.3f,body.Height+spread*.7f),26+spread*.65f);SolidBrush shade(Color(5,0,0,0));g.FillPath(&shade,&shadow);}
        GraphicsPath shape;rounded(shape,body,26);LinearGradientBrush fill(body,Color(245,37,39,44),Color(245,24,26,30),LinearGradientModeVertical);g.FillPath(&fill,&shape);
        Pen edge(Color(35,255,255,255),1);g.DrawPath(&edge,&shape);
        for(int i=0;i<4;++i){float y=popup_height_+40.0f+i*BUTTON_STEP;
            if(hot_==i||keyboard_focus_==i||pressed_==i){SolidBrush highlight(i==3?Color(190,171,49,63):Color(pressed_==i?44:25,255,255,255));GraphicsPath cell;rounded(cell,RectF(15,y-20,42,40),16);g.FillPath(&highlight,&cell);if(keyboard_focus_==i){Pen focus(Color(190,150,195,255),1.2f);g.DrawPath(&focus,&cell);}}
            icon(g,i,36,y,fullscreen,muted||volume<=0.0f);
        }
        Pen separator(Color(28,255,255,255),1);line(g,separator,25,float(popup_height_+151),47,float(popup_height_+151));
        if(popup_){
            FontFamily family(L"Segoe UI");Font font(&family,10,FontStyleRegular,UnitPixel);StringFormat format;format.SetAlignment(StringAlignmentCenter);format.SetLineAlignment(StringAlignmentCenter);
            wchar_t value[16];if(muted)wcscpy_s(value,L"Muted");else swprintf_s(value,L"%d",int(std::round(volume*100)));
            SolidBrush text(Color(230,220,224,232));g.DrawString(value,-1,&font,RectF(12,17,48,18),&format,&text);
            float top=46,bottom=float(popup_height_)+2,thumb=bottom-volume*(bottom-top);
            Pen track(Color(255,65,69,77),4);track.SetStartCap(LineCapRound);track.SetEndCap(LineCapRound);line(g,track,36,top,36,bottom);
            Pen level(muted?Color(255,128,133,143):Color(255,220,228,239),4);level.SetStartCap(LineCapRound);level.SetEndCap(LineCapRound);line(g,level,36,thumb,36,bottom);
            SolidBrush knob(Color(255,247,248,250));g.FillEllipse(&knob,29.0f,thumb-7,14.0f,14.0f);
        }
        g.Flush(FlushIntentionSync);
        if(diagnostic_path){
            // Only the explicit no-network diagnostic invokes this on UI artwork.
            static const CLSID png={0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};
            if(bitmap.Save(diagnostic_path,&png,nullptr)!=Ok)result=E_FAIL;
        }
    }
    SIZE size{width,height};POINT source{};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    UpdateLayeredWindow(palette_,screen,nullptr,&size,memory,&source,0,&blend,ULW_ALPHA);
    SelectObject(memory,old);DeleteObject(dib);DeleteDC(memory);ReleaseDC(nullptr,screen);update_tooltips();
    return result;
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
    case WM_CLOSE:a->quit=true;a->signal();return 0;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{PAINTSTRUCT paint;BeginPaint(h,&paint);EndPaint(h,&paint);a->signal();return 0;}
    case WM_SIZE:a->shape_window();a->place_palette();a->signal();return 0;
    case WM_MOVE:a->place_palette();return 0;
    case WM_DPICHANGED:{auto r=reinterpret_cast<RECT*>(lp);SetWindowPos(h,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);a->shape_window();a->place_palette();a->signal();return 0;}
    case WM_NCHITTEST:{POINT pt{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(h,&pt);RECT r;GetClientRect(h,&r);float s=a->dpi_scale();
        if(!a->fullscreen && pt.x>r.right-int(36*s)&&pt.y>r.bottom-int(36*s))return HTBOTTOMRIGHT;
        return a->fullscreen?HTCLIENT:HTCAPTION;}
    case WM_ENTERSIZEMOVE:GetWindowRect(h,&a->size_start_);a->popup_=false;a->place_palette();return 0;
    case WM_SIZING:{if(a->fullscreen)break;auto r=reinterpret_cast<RECT*>(lp);float s=a->dpi_scale(),border=2*std::round(4*s);
        float w=float(r->right-r->left),height=float(r->bottom-r->top);
        float dw=std::abs(w-(a->size_start_.right-a->size_start_.left))/std::max(1L,a->size_start_.right-a->size_start_.left);
        float dh=std::abs(height-(a->size_start_.bottom-a->size_start_.top))/std::max(1L,a->size_start_.bottom-a->size_start_.top);
        if(dw>dh)height=(w-border)/a->aspect_+border;else w=(height-border)*a->aspect_+border;
        float min_h=a->aspect_<1?360*s:180*s;float max_h=std::min(3900.0f,float(work_area(h).bottom-work_area(h).top-16*s));
        height=std::clamp(height,std::min(min_h,max_h),max_h);w=(height-border)*a->aspect_+border;
        float max_w=std::min(3900.0f,float(work_area(h).right-work_area(h).left)-90*s);
        if(w>max_w){w=max_w;height=(w-border)/a->aspect_+border;}
        r->right=r->left+LONG(w);r->bottom=r->top+LONG(height);a->user_sized_=true;return TRUE;}
    case WM_GETMINMAXINFO:{auto m=reinterpret_cast<MINMAXINFO*>(lp);float s=a->dpi_scale();float height=a->aspect_<1?360*s:180*s;m->ptMinTrackSize={LONG((height-8*s)*a->aspect_+8*s),LONG(height)};m->ptMaxTrackSize={4096,4096};return 0;}
    case WM_FORMAT:{float next=float(wp)/float(lp);if(std::isfinite(next)&&next>.2f&&next<5.0f&&std::abs(next-a->aspect_)>.01f){
        RECT r;GetWindowRect(h,&r);a->aspect_=next;
        if(!a->fullscreen){if(!a->user_sized_)a->default_size();else{r=a->fitted_rect(r);SetWindowPos(h,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);}}
        }a->signal();a->paint_palette();return 0;}
    case WM_REFRESH:a->paint_palette();return 0;
    case WM_NCLBUTTONDBLCLK:if(wp==HTCAPTION){a->toggle_fullscreen();return 0;}break;
    case WM_LBUTTONDBLCLK:a->toggle_fullscreen();return 0;
    case WM_SYSCOMMAND:if((wp&0xfff0)==SC_MAXIMIZE || ((wp&0xfff0)==SC_RESTORE && a->fullscreen)){a->toggle_fullscreen();return 0;}break;
    case WM_NCRBUTTONUP:case WM_RBUTTONUP:{POINT p;GetCursorPos(&p);a->menu(p);return 0;}
    case WM_KEYDOWN:
        if(wp==VK_F11 && !(lp&(1LL<<30))){a->toggle_fullscreen();return 0;}
        if(wp=='M' && !(lp&(1LL<<30))){a->toggle_mute();return 0;}
        if(wp=='0'||wp==VK_NUMPAD0){a->default_size();return 0;}
        if(wp==VK_UP||wp==VK_DOWN){a->set_volume(a->volume+(wp==VK_UP?.05f:-.05f));return 0;}
        if(wp==VK_TAB){a->keyboard_focus_=a->keyboard_focus_<0?(GetKeyState(VK_SHIFT)<0?3:0):(a->keyboard_focus_+(GetKeyState(VK_SHIFT)<0?3:1))%4;a->paint_palette();return 0;}
        if((wp==VK_RETURN||wp==VK_SPACE)&&a->keyboard_focus_>=0){a->invoke(a->keyboard_focus_);return 0;}
        if(wp==VK_ESCAPE){if(a->fullscreen)a->toggle_fullscreen();else{a->disconnect=true;a->signal();}return 0;}
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
        KillTimer(h,HIDE_VOLUME);if(a->dragging_slider_){a->pointer_volume(point.y);return 0;}
        int hot=a->button_at(point);bool changed=hot!=a->hot_;a->hot_=hot;
        if(hot==0&&!a->popup_){a->popup_=true;a->place_palette();}else if(changed)a->paint_palette();return 0;}
    case WM_MOUSELEAVE:a->tracking_=false;a->hot_=-1;if(!a->dragging_slider_)SetTimer(h,HIDE_VOLUME,300,nullptr);a->paint_palette();return 0;
    case WM_TIMER:if(wp==HIDE_VOLUME){POINT p;RECT r;GetCursorPos(&p);GetWindowRect(h,&r);if(!PtInRect(&r,p)&&!a->dragging_slider_){a->popup_=false;a->place_palette();}KillTimer(h,HIDE_VOLUME);}return 0;
    case WM_LBUTTONDOWN:{POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};a->pressed_=a->button_at(p);if(!a->hidden_)SetCapture(h);
        if(a->popup_ && p.y<a->popup_height_*a->dpi_scale()){a->dragging_slider_=true;a->pointer_volume(p.y);}a->paint_palette();return 0;}
    case WM_LBUTTONUP:{POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};int hit=a->button_at(p);bool slider=a->dragging_slider_;int pressed=a->pressed_;a->dragging_slider_=false;if(GetCapture()==h)ReleaseCapture();a->pressed_=-1;if(!slider&&hit==pressed&&hit>=0)a->invoke(hit);a->paint_palette();return 0;}
    case WM_CAPTURECHANGED:a->dragging_slider_=false;a->pressed_=-1;return 0;
    case WM_MOUSEWHEEL:a->set_volume(a->volume+(GET_WHEEL_DELTA_WPARAM(wp)>0?.05f:-.05f));return 0;
    case WM_CLOSE:a->quit=true;a->signal();return 0;
    }
    return DefWindowProcW(h,msg,wp,lp);
}

#include "phone_shell_test.inc"
