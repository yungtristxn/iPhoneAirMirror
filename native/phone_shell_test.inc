// In-process diagnostics for the hidden test window; never drives another app.
HRESULT PhoneShell::self_test(const wchar_t* directory){
    return HRESULT(SendMessageW(window,WM_TEST,0,reinterpret_cast<LPARAM>(directory)));
}
HRESULT PhoneShell::test_on_ui_thread(const wchar_t* directory){
    if(!hidden_)return E_ACCESSDENIED;
#define UI_REQUIRE(test) do{if(!(test))return MAKE_HRESULT(SEVERITY_ERROR,FACILITY_ITF,__LINE__);}while(0)
    auto key=[&](WPARAM k){main_proc(window,WM_KEYDOWN,k,0);};
    auto ratio=[&](){RECT r;GetWindowRect(window,&r);float border=2*std::round(4*dpi_scale());return (r.right-r.left-border)/(r.bottom-r.top-border);};
    auto click=[&](int index){float s=dpi_scale();LPARAM point=MAKELPARAM(int(36*s),int((popup_height_+40+index*BUTTON_STEP)*s));
        palette_proc(palette_,WM_LBUTTONDOWN,MK_LBUTTON,point);palette_proc(palette_,WM_LBUTTONUP,0,point);};
    auto save=[&](const wchar_t* name){return directory&&*directory?paint_palette((std::wstring(directory)+L"/"+name).c_str()):S_OK;};
    UI_REQUIRE((GetWindowLongPtrW(window,GWL_STYLE)&WS_CAPTION)==0);
    RECT client,outer;GetClientRect(window,&client);GetWindowRect(window,&outer);
    UI_REQUIRE(client.right==outer.right-outer.left&&client.bottom==outer.bottom-outer.top);
    set_volume(.65f);click(0);UI_REQUIRE(muted&&gain()==0);
    click(0);UI_REQUIRE(!muted&&std::abs(gain()-.65f)<.001f);
    palette_proc(palette_,WM_MOUSEMOVE,0,MAKELPARAM(int(36*dpi_scale()),int(40*dpi_scale())));
    UI_REQUIRE(popup_&&popup_height_>=70);
    pointer_volume(int((46+popup_height_+2)*.5f*dpi_scale()));UI_REQUIRE(!muted&&std::abs(volume-.5f)<.02f);
    UI_REQUIRE(SUCCEEDED(save(L"volume.png")));
    click(0);UI_REQUIRE(muted);UI_REQUIRE(SUCCEEDED(save(L"muted.png")));click(0);
    for(int i=0;i<25;++i)key(VK_DOWN);UI_REQUIRE(volume==0&&gain()==0);
    click(0);UI_REQUIRE(!muted&&volume>0);
    for(int i=0;i<25;++i)key(VK_UP);UI_REQUIRE(volume==1);
    popup_=false;hot_=-1;place_palette();UI_REQUIRE(SUCCEEDED(save(L"controls.png")));
    for(auto format:{std::pair<UINT,UINT>{498,1080},{1920,1080},{1080,1920}}){
        main_proc(window,WM_FORMAT,format.first,format.second);key('0');
        UI_REQUIRE(std::abs(ratio()-float(format.first)/format.second)<.01f);
        GetWindowRect(window,&outer);POINT point{outer.right-int(18*dpi_scale()),outer.bottom-int(18*dpi_scale())};
        UI_REQUIRE(main_proc(window,WM_NCHITTEST,0,MAKELPARAM(point.x,point.y))==HTBOTTOMRIGHT);
        main_proc(window,WM_ENTERSIZEMOVE,0,0);RECT proposed=outer;proposed.right+=97;proposed.bottom+=45;
        main_proc(window,WM_SIZING,WMSZ_BOTTOMRIGHT,reinterpret_cast<LPARAM>(&proposed));
        UI_REQUIRE(user_sized_);SetWindowPos(window,nullptr,proposed.left,proposed.top,proposed.right-proposed.left,proposed.bottom-proposed.top,SWP_NOZORDER|SWP_NOACTIVATE);
        UI_REQUIRE(std::abs(ratio()-float(format.first)/format.second)<.01f);
        click(1);UI_REQUIRE(!user_sized_);
    }
    click(2);UI_REQUIRE(fullscreen);main_proc(window,WM_FORMAT,1920,1080);key(VK_ESCAPE);
    UI_REQUIRE(!fullscreen&&std::abs(ratio()-1920.0f/1080)<.01f);
    UI_REQUIRE((GetWindowLongPtrW(window,GWL_STYLE)&WS_CAPTION)==0);
    main_proc(window,WM_FORMAT,498,1080);key('0');
    RECT phone,palette;GetWindowRect(window,&phone);GetWindowRect(palette_,&palette);
    UI_REQUIRE(palette.left>=phone.right||palette.right<=phone.left);
    UI_REQUIRE(GetWindow(palette_,GW_OWNER)==window);
    UI_REQUIRE((GetWindowLongPtrW(palette_,GWL_EXSTYLE)&WS_EX_TOOLWINDOW)!=0);
    // Opening the volume popup at the monitor's top edge must not move its button.
    main_proc(window,WM_FORMAT,1920,1080);
    RECT work=work_area(window);SetWindowPos(window,nullptr,phone.left,work.top,328,188,SWP_NOZORDER|SWP_NOACTIVATE);
    GetWindowRect(palette_,&palette);LONG speaker_y=palette.top+int(40*dpi_scale());
    palette_proc(palette_,WM_MOUSEMOVE,0,MAKELPARAM(int(36*dpi_scale()),int(40*dpi_scale())));
    GetWindowRect(palette_,&palette);UI_REQUIRE(speaker_y==palette.top+int((popup_height_+40)*dpi_scale()));
    popup_=false;main_proc(window,WM_FORMAT,498,1080);default_size(true);
    key('D');UI_REQUIRE(disconnect.exchange(false));
    click(3);UI_REQUIRE(quit.exchange(false));
    UI_REQUIRE(!IsWindowVisible(window)&&!IsWindowVisible(palette_));
    hot_=keyboard_focus_=-1;popup_=false;set_volume(1);muted=false;KillTimer(palette_,HIDE_VOLUME);place_palette();
    return S_OK;
#undef UI_REQUIRE
}
