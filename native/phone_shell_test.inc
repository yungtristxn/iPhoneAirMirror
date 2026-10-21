// Hidden, in-process UI tests. No desktop input injection or phone capture.
HRESULT PhoneShell::self_test(const wchar_t* directory){return HRESULT(SendMessageW(window,WM_TEST,0,reinterpret_cast<LPARAM>(directory)));}
HRESULT PhoneShell::test_on_ui_thread(const wchar_t* directory){
    if(!hidden_)return E_ACCESSDENIED;
#define UI_REQUIRE(test) do{if(!(test))return MAKE_HRESULT(SEVERITY_ERROR,FACILITY_ITF,__LINE__);}while(0)
    auto key=[&](WPARAM k){main_proc(window,WM_KEYDOWN,k,0);};
    auto point=[&](int button){return MAKELPARAM(int((button==4?arrow_x_:rail_x_+38)*dpi_scale()),int((button==4?108:40+button*BUTTON_STEP)*dpi_scale()));};
    auto click=[&](int button){LPARAM p=point(button);palette_proc(palette_,WM_LBUTTONDOWN,MK_LBUTTON,p);palette_proc(palette_,WM_LBUTTONUP,0,p);};
    auto hover=[&](){palette_proc(palette_,WM_MOUSEMOVE,0,point(0));palette_proc(palette_,WM_MOUSEHOVER,0,point(0));};
    auto save=[&](const wchar_t* name,float scale=0){return directory&&*directory?paint_palette((std::wstring(directory)+L"/"+name).c_str(),scale):S_OK;};
    auto ratio=[&](){RECT r;GetWindowRect(window,&r);float rim=float(2*rim_pixels(dpi_scale()));return (r.right-r.left-rim)/(r.bottom-r.top-rim);};
    UI_REQUIRE(FindResourceW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_AIRMIRRORLITE),RT_GROUP_ICON));
    UI_REQUIRE(SendMessageW(window,WM_GETICON,ICON_SMALL,0)&&SendMessageW(window,WM_GETICON,ICON_BIG,0));
    for(int size:{16,32,48,256}){
        HICON icon=static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_AIRMIRRORLITE),IMAGE_ICON,size,size,LR_DEFAULTCOLOR));
        UI_REQUIRE(icon);ICONINFO info{};bool ok=GetIconInfo(icon,&info)!=FALSE;BITMAP bitmap{};
        if(ok)ok=GetObjectW(info.hbmColor,sizeof(bitmap),&bitmap)>0&&bitmap.bmWidth==size&&bitmap.bmHeight==size;
        if(info.hbmColor)DeleteObject(info.hbmColor);if(info.hbmMask)DeleteObject(info.hbmMask);DestroyIcon(icon);UI_REQUIRE(ok);
    }
    UI_REQUIRE((GetWindowLongPtrW(window,GWL_STYLE)&WS_CAPTION)==0);
    RECT client,outer;GetClientRect(window,&client);GetWindowRect(window,&outer);
    UI_REQUIRE(client.right==outer.right-outer.left&&client.bottom==outer.bottom-outer.top);
    set_volume(.65f);click(0);UI_REQUIRE(muted&&gain()==0);click(0);UI_REQUIRE(!muted&&std::abs(gain()-.65f)<.001f);
    // Passing over the speaker must not open the flyout; stale hover is ignored.
    palette_proc(palette_,WM_MOUSEMOVE,0,point(0));UI_REQUIRE(hover_pending_&&!popup_);
    palette_proc(palette_,WM_MOUSEMOVE,0,point(1));UI_REQUIRE(!hover_pending_&&!popup_);
    palette_proc(palette_,WM_MOUSEHOVER,0,point(0));UI_REQUIRE(!popup_);
    hover();UI_REQUIRE(popup_&&!hover_pending_);
    // Crossing below the flyout can keep the same hovered reset button. The
    // leave grace follows the volume zone, independently of button changes.
    const int rail_center=int((rail_x_+38)*dpi_scale());
    palette_proc(palette_,WM_MOUSEMOVE,0,MAKELPARAM(rail_center,int(80*dpi_scale())));
    UI_REQUIRE(hot_==1&&!hide_volume_pending_);
    palette_proc(palette_,WM_MOUSEMOVE,0,MAKELPARAM(rail_center,int(96*dpi_scale())));
    UI_REQUIRE(hot_==1&&hide_volume_pending_);
    // A queued timer from an earlier hover must not shorten the new grace time.
    palette_proc(palette_,WM_TIMER,HIDE_VOLUME,0);UI_REQUIRE(hide_volume_pending_&&popup_);
    palette_proc(palette_,WM_MOUSEMOVE,0,point(0));UI_REQUIRE(!hide_volume_pending_&&popup_);
    POINT slider{int((flyout_x_+106)*dpi_scale()),int(58*dpi_scale())};UI_REQUIRE(slider_hit(slider)&&volume_zone(slider));
    UI_REQUIRE(!slider_hit({int((flyout_x_+106)*dpi_scale()),int(24*dpi_scale())}));
    palette_proc(palette_,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(slider.x,slider.y));UI_REQUIRE(dragging_slider_&&std::abs(volume-.5f)<.02f);
    palette_proc(palette_,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(int((flyout_x_+188)*dpi_scale()),slider.y));UI_REQUIRE(volume==1);
    palette_proc(palette_,WM_LBUTTONUP,0,MAKELPARAM(slider.x,slider.y));UI_REQUIRE(!dragging_slider_);
    // An ordinary release preserves the open flyout. Losing capture to another
    // window cancels the interaction and removes the pressed state immediately.
    palette_proc(palette_,WM_CAPTURECHANGED,0,0);UI_REQUIRE(popup_);
    palette_proc(palette_,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(slider.x,slider.y));UI_REQUIRE(dragging_slider_);
    palette_proc(palette_,WM_CAPTURECHANGED,0,0);UI_REQUIRE(!dragging_slider_&&pressed_==-1&&!popup_&&!hover_pending_);
    hover();UI_REQUIRE(popup_);
    pointer_volume(slider.x);UI_REQUIRE(!muted&&std::abs(volume-.5f)<.02f);
    UI_REQUIRE(SUCCEEDED(save(L"volume.png")));UI_REQUIRE(SUCCEEDED(save(L"volume-125.png",1.25f)));UI_REQUIRE(SUCCEEDED(save(L"volume-200.png",2)));
    click(0);UI_REQUIRE(muted);UI_REQUIRE(SUCCEEDED(save(L"muted.png")));click(0);
    key(VK_ESCAPE);UI_REQUIRE(!popup_&&!disconnect);
    for(int i=0;i<25;++i)key(VK_DOWN);UI_REQUIRE(volume==0&&gain()==0);click(0);UI_REQUIRE(!muted&&volume>0);
    for(int i=0;i<25;++i)key(VK_UP);UI_REQUIRE(volume==1);
    hot_=-1;keyboard_focus_=-1;place_palette();
    UI_REQUIRE(SUCCEEDED(save(L"controls.png")));UI_REQUIRE(SUCCEEDED(save(L"controls-125.png",1.25f)));UI_REQUIRE(SUCCEEDED(save(L"controls-200.png",2)));
    click(4);UI_REQUIRE(controls_collapsed_&&!popup_);UI_REQUIRE(SUCCEEDED(save(L"collapsed-idle.png")));
    palette_proc(palette_,WM_MOUSEMOVE,0,point(4));UI_REQUIRE(hot_==4&&!hover_pending_);UI_REQUIRE(SUCCEEDED(save(L"collapsed-hover.png")));
    key(VK_TAB);UI_REQUIRE(keyboard_focus_==4);key(VK_RETURN);UI_REQUIRE(!controls_collapsed_);keyboard_focus_=-1;
    hover();click(4);UI_REQUIRE(controls_collapsed_&&!popup_);click(4);UI_REQUIRE(!controls_collapsed_);
    for(auto format:{std::pair<UINT,UINT>{498,1080},{1920,1080},{1080,1920},{300,600}}){
        main_proc(window,WM_FORMAT,format.first,format.second);key('0');
        UI_REQUIRE(std::abs(ratio()-float(format.first)/format.second)<.01f);
        GetClientRect(window,&client);int rim=2*rim_pixels(dpi_scale());
        UI_REQUIRE(client.right-rim<=LONG(format.first)&&client.bottom-rim<=LONG(format.second));
        if(format.first==300)UI_REQUIRE(client.right-rim==300&&client.bottom-rim==600);
        GetWindowRect(window,&outer);POINT grip{outer.right-int(18*dpi_scale()),outer.bottom-int(18*dpi_scale())};
        UI_REQUIRE(main_proc(window,WM_NCHITTEST,0,MAKELPARAM(grip.x,grip.y))==HTBOTTOMRIGHT);
        main_proc(window,WM_ENTERSIZEMOVE,0,0);RECT proposed=outer;proposed.right+=97;proposed.bottom+=45;
        main_proc(window,WM_SIZING,WMSZ_BOTTOMRIGHT,reinterpret_cast<LPARAM>(&proposed));
        UI_REQUIRE(user_sized_);SetWindowPos(window,nullptr,proposed.left,proposed.top,proposed.right-proposed.left,proposed.bottom-proposed.top,SWP_NOZORDER|SWP_NOACTIVATE);
        UI_REQUIRE(std::abs(ratio()-float(format.first)/format.second)<.01f);click(1);UI_REQUIRE(!user_sized_);
    }
    click(2);UI_REQUIRE(fullscreen);main_proc(window,WM_FORMAT,1920,1080);key(VK_ESCAPE);
    UI_REQUIRE(!fullscreen&&std::abs(ratio()-1920.0f/1080)<.01f);
    UI_REQUIRE((GetWindowLongPtrW(window,GWL_STYLE)&WS_CAPTION)==0);
    RECT work=work_area(window);
    for(POINT origin:{POINT{work.left,work.top},POINT{work.right-400,work.top},POINT{work.right-400,work.bottom-200}}){
        dismiss_volume();SetWindowPos(window,nullptr,origin.x,origin.y,328,188,SWP_NOZORDER|SWP_NOACTIVATE);
        RECT before,after;GetWindowRect(palette_,&before);LONG speaker_x=before.left+int((rail_x_+38)*dpi_scale()),speaker_y=before.top+int(40*dpi_scale());
        hover();GetWindowRect(palette_,&after);UI_REQUIRE(popup_);
        UI_REQUIRE(speaker_x==after.left+int((rail_x_+38)*dpi_scale())&&speaker_y==after.top+int(40*dpi_scale()));
        UI_REQUIRE(after.left>=work.left&&after.right<=work.right&&after.top>=work.top&&after.bottom<=work.bottom);
        UI_REQUIRE(volume_zone({int((rail_x_+38)*dpi_scale()),int(40*dpi_scale())}));
        UI_REQUIRE(!volume_zone({int((rail_x_+38)*dpi_scale()),int(172*dpi_scale())}));
        bool side=dock_right_;click(4);UI_REQUIRE(controls_collapsed_&&dock_right_==side&&!popup_);
        click(4);UI_REQUIRE(!controls_collapsed_&&dock_right_==side);
    }
    dismiss_volume();main_proc(window,WM_FORMAT,498,1080);default_size(true);
    UI_REQUIRE(GetWindow(palette_,GW_OWNER)==window&&(GetWindowLongPtrW(palette_,GWL_EXSTYLE)&WS_EX_TOOLWINDOW));
    keyboard_focus_=-1;key(VK_TAB);UI_REQUIRE(keyboard_focus_==0);key(VK_LEFT);UI_REQUIRE(popup_&&volume<1);key(VK_ESCAPE);UI_REQUIRE(!popup_&&!disconnect);
    key(VK_SPACE);UI_REQUIRE(muted);
    main_proc(window,WM_KEYDOWN,VK_SPACE,1LL<<30);UI_REQUIRE(muted);
    key(VK_SPACE);UI_REQUIRE(!muted);
    float before_repeat=volume;main_proc(window,WM_KEYDOWN,VK_LEFT,1LL<<30);UI_REQUIRE(volume<before_repeat);dismiss_volume();
    // Repositioning unchanged artwork uses the existing layered surface. Hover
    // and volume changes repaint it without allocating another bitmap or DC.
    hot_=keyboard_focus_=-1;place_palette();UI_REQUIRE(palette_surface_->valid);
    const auto draws=palette_surface_->draws,allocations=palette_surface_->allocations;
    DWORD objects=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    GetWindowRect(window,&outer);
    for(int i=0;i<100;++i)SetWindowPos(window,nullptr,outer.left+(i%2),outer.top,outer.right-outer.left,outer.bottom-outer.top,SWP_NOZORDER|SWP_NOACTIVATE);
    UI_REQUIRE(palette_surface_->draws==draws&&palette_surface_->allocations==allocations);
    UI_REQUIRE(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==objects);
    palette_proc(palette_,WM_MOUSEMOVE,0,point(1));UI_REQUIRE(palette_surface_->draws==draws+1&&palette_surface_->allocations==allocations);
    set_volume(.42f);UI_REQUIRE(palette_surface_->draws==draws+2&&palette_surface_->allocations==allocations);
    key('D');UI_REQUIRE(disconnect.exchange(false));click(3);UI_REQUIRE(quit.exchange(false));
    UI_REQUIRE(!IsWindowVisible(window)&&!IsWindowVisible(palette_));
    hot_=keyboard_focus_=-1;dismiss_volume();set_volume(1);muted=false;place_palette();return S_OK;
#undef UI_REQUIRE
}
