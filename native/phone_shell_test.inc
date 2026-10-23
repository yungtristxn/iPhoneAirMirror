// Hidden, in-process UI tests. No desktop input injection or phone capture.
HRESULT PhoneShell::self_test(const wchar_t* directory){return HRESULT(SendMessageW(window,WM_TEST,0,reinterpret_cast<LPARAM>(directory)));}
HRESULT PhoneShell::test_on_ui_thread(const wchar_t* directory){
    if(!hidden_)return E_ACCESSDENIED;
#define UI_REQUIRE(test) do{if(!(test))return MAKE_HRESULT(SEVERITY_ERROR,FACILITY_ITF,__LINE__);}while(0)
    auto key=[&](WPARAM k){main_proc(window,WM_KEYDOWN,k,0);};
    auto point=[&](int button){return MAKELPARAM(int((button==4?arrow_x_:rail_x_+38)*dpi_scale()),int((button==4?control_layout::arrow_y:control_layout::row_center(button))*dpi_scale()));};
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
    for(float scale:{1.0f,1.25f,2.0f}){
        RECT area{0,0,3840,2160},phone{300,200,300+LONG(166*scale),200+LONG(600*scale)};
        auto card=status_card_size(phone,area,scale);const int card_left=phone.left+(phone.right-phone.left-card.width)/2;
        auto rail=palette_placement(phone,area,scale,false,false,false);
        UI_REQUIRE(card.compact&&card.width<phone.right-phone.left&&card.height==int(244*scale));
        UI_REQUIRE(card_left>=phone.left&&card_left+card.width<=phone.right&&card_left+card.width<rail.x);
        phone.right=phone.left+LONG(400*scale);card=status_card_size(phone,area,scale);
        UI_REQUIRE(!card.compact&&card.width==int(340*scale)&&card.height==int(218*scale));
    }
    // The rail remains inside each work area even when neither external side
    // fits (the original 1830-wide window at the left edge is included).
    for(float scale:{1.0f,1.25f,2.0f})for(bool collapsed:{false,true})for(bool popup:{false,true}){
        RECT area{0,0,1920,1080};const int width=1920-int(90*scale);
        for(POINT origin:{POINT{0,0},POINT{-100,-100},POINT{1800,900}}){
            RECT phone{origin.x,origin.y,origin.x+width,origin.y+400};auto p=palette_placement(phone,area,scale,collapsed,popup&&!collapsed,false);
            UI_REQUIRE(p.x>=area.left&&p.x+p.width<=area.right&&p.y>=area.top&&p.y+p.height<=area.bottom);
            UI_REQUIRE(rail_clearance(scale)>=int((PALETTE_WIDTH+RAIL_GAP)*scale));
        }
    }
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
    // Dismissing a flyout during a drag also ends that interaction. Late pointer
    // movement/release must neither adjust the hidden slider nor click a button.
    for(bool cancel_message:{false,true}){
        hover();slider={int((flyout_x_+106)*dpi_scale()),int(58*dpi_scale())};
        palette_proc(palette_,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(slider.x,slider.y));UI_REQUIRE(dragging_slider_);
        if(cancel_message)palette_proc(palette_,WM_CANCELMODE,0,0);else key(VK_ESCAPE);
        UI_REQUIRE(!dragging_slider_&&pressed_==-1&&!popup_&&!disconnect);
        const float canceled_volume=volume;
        palette_proc(palette_,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(0,0));
        palette_proc(palette_,WM_LBUTTONUP,0,point(0));
        UI_REQUIRE(volume==canceled_volume&&!muted&&!quit);
    }
    // A wheel notch remains 5%, including fractional and reversed input.
    set_volume(.5f);wheel_delta_=0;
    auto wheel=[&](short delta){palette_proc(palette_,WM_MOUSEWHEEL,MAKEWPARAM(0,delta),0);};
    wheel(0);wheel(30);wheel(30);wheel(30);UI_REQUIRE(volume==.5f);
    wheel(30);UI_REQUIRE(std::abs(volume-.55f)<.001f&&wheel_delta_==0);
    wheel(-60);wheel(60);UI_REQUIRE(std::abs(volume-.55f)<.001f&&wheel_delta_==0);
    wheel(-240);UI_REQUIRE(std::abs(volume-.45f)<.001f&&wheel_delta_==0);
    hover();UI_REQUIRE(popup_);
    slider={int((flyout_x_+106)*dpi_scale()),int(58*dpi_scale())};
    pointer_volume(slider.x);UI_REQUIRE(!muted&&std::abs(volume-.5f)<.02f);
    UI_REQUIRE(SUCCEEDED(save(L"volume.png")));UI_REQUIRE(SUCCEEDED(save(L"volume-125.png",1.25f)));UI_REQUIRE(SUCCEEDED(save(L"volume-200.png",2)));
    click(0);UI_REQUIRE(muted);UI_REQUIRE(SUCCEEDED(save(L"muted.png")));click(0);
    key(VK_ESCAPE);UI_REQUIRE(!popup_&&!disconnect);
    for(int i=0;i<25;++i)key(VK_DOWN);UI_REQUIRE(volume==0&&gain()==0);click(0);UI_REQUIRE(!muted&&volume>0);
    for(int i=0;i<25;++i)key(VK_UP);UI_REQUIRE(volume==1);
    hot_=-1;keyboard_focus_=-1;place_palette();
    UI_REQUIRE(SUCCEEDED(save(L"controls.png")));UI_REQUIRE(SUCCEEDED(save(L"controls-125.png",1.25f)));UI_REQUIRE(SUCCEEDED(save(L"controls-200.png",2)));
    hot_=3;UI_REQUIRE(SUCCEEDED(save(L"close-hover.png")));
    hot_=-1;keyboard_focus_=2;UI_REQUIRE(SUCCEEDED(save(L"keyboard-focus.png")));keyboard_focus_=-1;
    // Accessibility and mouse input agree after the side tab moved to row one.
    AccessibilityRequest control_snapshot;accessibility_request(control_snapshot);
    UI_REQUIRE(SUCCEEDED(control_snapshot.result));RECT rail_bounds;GetWindowRect(palette_,&rail_bounds);
    for(int id=1;id<=5;++id){const RECT b=control_snapshot.snapshot.bounds[id];
        POINT center{(b.left+b.right)/2-rail_bounds.left,(b.top+b.bottom)/2-rail_bounds.top};
        UI_REQUIRE(button_at(center)==id-1);
    }
    UI_REQUIRE(button_at({int(arrow_x_*dpi_scale()),int(108*dpi_scale())})==-1);
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
    click(2);UI_REQUIRE(fullscreen);GetWindowRect(window,&outer);MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor);UI_REQUIRE(EqualRect(&outer,&monitor.rcMonitor));main_proc(window,WM_FORMAT,1920,1080);key(VK_ESCAPE);
    UI_REQUIRE(!fullscreen&&std::abs(ratio()-1920.0f/1080)<.01f);
    UI_REQUIRE((GetWindowLongPtrW(window,GWL_STYLE)&WS_CAPTION)==0);
    UI_REQUIRE(toggles_fullscreen(SC_MAXIMIZE,false,false));
    UI_REQUIRE(toggles_fullscreen(SC_RESTORE,true,false));
    UI_REQUIRE(!toggles_fullscreen(SC_RESTORE,true,true));
    UI_REQUIRE(!toggles_fullscreen(SC_RESTORE,false,true));
    click(2);UI_REQUIRE(fullscreen);main_proc(window,WM_SYSCOMMAND,SC_RESTORE,0);
    UI_REQUIRE(!fullscreen&&!IsWindowVisible(window));
    // Rapid Photos-style format changes must restore the final portrait shape
    // without pressing Default size, including the user-sized/fullscreen paths.
    for(bool manual:{false,true}){
        main_proc(window,WM_FORMAT,994,2160);default_size();user_sized_=manual;
        for(auto format:{std::pair<UINT,UINT>{2160,994},{994,2160},{2160,994},{994,2160}}){
            main_proc(window,WM_FORMAT,format.first,format.second);
            UI_REQUIRE(std::abs(ratio()-float(format.first)/format.second)<.01f);
            UI_REQUIRE(user_sized_==manual);
        }
        click(2);UI_REQUIRE(fullscreen);main_proc(window,WM_FORMAT,2160,994);main_proc(window,WM_FORMAT,994,2160);key(VK_ESCAPE);
        UI_REQUIRE(!fullscreen&&std::abs(ratio()-994.0f/2160)<.01f&&user_sized_==manual);
        // Exercise a normal uniformly scaled suggestion without changing the
        // desktop DPI. This checks handler geometry, not multi-monitor evidence.
        RECT suggestion;GetWindowRect(window,&suggestion);
        suggestion.right=suggestion.left+(suggestion.right-suggestion.left)*4/5;
        suggestion.bottom=suggestion.top+(suggestion.bottom-suggestion.top)*4/5;
        UINT dpi=GetDpiForWindow(window);main_proc(window,WM_DPICHANGED,MAKELONG(dpi,dpi),reinterpret_cast<LPARAM>(&suggestion));
        GetClientRect(window,&client);const int rim=2*rim_pixels(dpi_scale());
        UI_REQUIRE(client.right<client.bottom&&std::abs((client.right-rim)-994.0f/2160*(client.bottom-rim))<=2.0f&&user_sized_==manual);
    }
    {
        // Production source messages preserve an established phone frame when
        // Photos supplies wide media. Only this hidden test uses synthetic epochs.
        const UINT_PTR saved_generation=ui_generation_;const float saved_shape=session_shape_;
        UINT_PTR generation=saved_generation;
        auto source=[&](UINT w,UINT h,UINT_PTR epoch){main_proc(window,WM_SOURCE_FORMAT,epoch,MAKELPARAM(w,h));};
        for(bool manual:{false,true}){
            main_proc(window,WM_SOURCE_RESET,++generation,0);source(994,2160,generation);default_size();user_sized_=manual;
            RECT phone;GetWindowRect(window,&phone);source(3840,2160,generation);GetWindowRect(window,&outer);
            UI_REQUIRE(EqualRect(&phone,&outer)&&display_w_==994&&display_h_==2160&&user_sized_==manual);
            source(2160,994,generation);UI_REQUIRE(std::abs(ratio()-2160.0f/994)<.01f);
            source(498,1080,generation);UI_REQUIRE(display_w_==498&&display_h_==1080&&std::abs(ratio()-498.0f/1080)<.01f);
            source(1920,1080,generation);key('0');
            UI_REQUIRE(display_w_==498&&display_h_==1080&&std::abs(ratio()-498.0f/1080)<.01f);
        }
        const UINT_PTR old=generation;main_proc(window,WM_SOURCE_RESET,++generation,0);
        source(1920,1080,old);UI_REQUIRE(display_w_==498&&display_h_==1080&&session_shape_==0);
        source(300,600,generation);UI_REQUIRE(display_w_==300&&display_h_==600);
        // A new session's frame may arrive before its reset is dispatched. A
        // subsequent equal/older reset must not erase the newly learned shape.
        source(1920,1080,++generation);main_proc(window,WM_SOURCE_RESET,generation,0);main_proc(window,WM_SOURCE_RESET,old,0);
        source(994,2160,generation);UI_REQUIRE(display_w_==1920&&display_h_==1080&&std::abs(session_shape_-1080.0f/1920)<.001f);
        ui_generation_=saved_generation;session_shape_=saved_shape;
    }
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
    // Explicit failures stay visible through cleanup; a dead listener must not
    // become Ready merely because its error is dismissed or a session ends.
    set_status(4,L"Synthetic decoder failure");reset_video();set_status(0,L"Ready");paint_status_panel();
    UI_REQUIRE(status==4&&status_message()==L"Synthetic decoder failure"&&painted_status_kind_==4&&status_surface_->width>0);
    key(VK_ESCAPE);UI_REQUIRE(status<3&&status_dismissed.exchange(false));
    set_status(5,L"Listener unavailable");set_status(0,L"Ready");UI_REQUIRE(!dismiss_error()&&status==5);
    set_status(3,L"Restoring Windows playback");UI_REQUIRE(status==3);set_status(6,L"");UI_REQUIRE(status<3);
    set_status(4,std::wstring(300,L'x')+L"\n");UI_REQUIRE(status_message().size()==256);set_status(6,L"");
    // Query and operate the real native providers directly in this hidden
    // window; no external UI Automation client or desktop input is involved.
    {
        ComPtr<IRawElementProviderSimple> root;root.Attach(accessibility_->provider_for_test(0));
        ComPtr<IRawElementProviderFragment> fragment;UI_REQUIRE(SUCCEEDED(root.As(&fragment)));
        VARIANT focusable{};UI_REQUIRE(SUCCEEDED(root->GetPropertyValue(UIA_IsKeyboardFocusablePropertyId,&focusable))&&focusable.vt==VT_BOOL&&focusable.boolVal==VARIANT_TRUE);VariantClear(&focusable);
        keyboard_focus_=0;UI_REQUIRE(SUCCEEDED(fragment->SetFocus())&&keyboard_focus_==-1);
        ComPtr<IRawElementProviderFragment> rail;UI_REQUIRE(SUCCEEDED(fragment->Navigate(NavigateDirection_FirstChild,&rail))&&rail);
        ComPtr<IRawElementProviderFragment> child;UI_REQUIRE(SUCCEEDED(rail->Navigate(NavigateDirection_FirstChild,&child))&&child);
        ComPtr<IRawElementProviderSimple> rail_provider;UI_REQUIRE(SUCCEEDED(rail.As(&rail_provider)));ComPtr<IRawElementProviderSimple> host;UI_REQUIRE(SUCCEEDED(rail_provider->get_HostRawElementProvider(&host))&&host);
        ComPtr<IRawElementProviderFragment> parent;UI_REQUIRE(SUCCEEDED(rail->Navigate(NavigateDirection_Parent,&parent))&&parent);
        ComPtr<IUnknown> root_identity,parent_identity;root.As(&root_identity);parent.As(&parent_identity);UI_REQUIRE(root_identity.Get()==parent_identity.Get());
        for(int id=1;id<=6;++id){ComPtr<IRawElementProviderSimple> node;node.Attach(accessibility_->provider_for_test(id));VARIANT value{};
            UI_REQUIRE(SUCCEEDED(node->GetPropertyValue(UIA_NamePropertyId,&value))&&value.vt==VT_BSTR&&SysStringLen(value.bstrVal)>0);VariantClear(&value);
            UI_REQUIRE(SUCCEEDED(node->GetPropertyValue(UIA_ControlTypePropertyId,&value))&&value.vt==VT_I4&&value.lVal==(id==6?UIA_SliderControlTypeId:UIA_ButtonControlTypeId));VariantClear(&value);
        }
        ComPtr<IRawElementProviderSimple> speaker; speaker.Attach(accessibility_->provider_for_test(1));ComPtr<IToggleProvider> toggle;
        UI_REQUIRE(SUCCEEDED(speaker.As(&toggle)));ToggleState before,after;UI_REQUIRE(SUCCEEDED(toggle->get_ToggleState(&before))&&SUCCEEDED(toggle->Toggle())&&SUCCEEDED(toggle->get_ToggleState(&after))&&before!=after);toggle->Toggle();
        ComPtr<IRawElementProviderSimple> slider_provider;slider_provider.Attach(accessibility_->provider_for_test(6));ComPtr<IRangeValueProvider> range;UI_REQUIRE(SUCCEEDED(slider_provider.As(&range)));
        double value=0;UI_REQUIRE(SUCCEEDED(range->SetValue(37))&&SUCCEEDED(range->get_Value(&value))&&std::abs(value-37)<.01);
        UI_REQUIRE(range->SetValue(101)==E_INVALIDARG&&range->SetValue(std::nan(""))==E_INVALIDARG);
        ComPtr<IRawElementProviderFragment> slider_fragment;UI_REQUIRE(SUCCEEDED(slider_provider.As(&slider_fragment))&&SUCCEEDED(slider_fragment->SetFocus())&&keyboard_focus_==5&&popup_);
        key(VK_HOME);UI_REQUIRE(volume==0);key(VK_END);UI_REQUIRE(volume==1);key(VK_NEXT);UI_REQUIRE(std::abs(volume-.9f)<.001f);key(VK_UP);UI_REQUIRE(std::abs(volume-.91f)<.001f);key(VK_SPACE);UI_REQUIRE(!quit&&!controls_collapsed_);dismiss_volume();
        ComPtr<IRawElementProviderSimple> collapse;collapse.Attach(accessibility_->provider_for_test(5));ComPtr<IInvokeProvider> action;UI_REQUIRE(SUCCEEDED(collapse.As(&action))&&SUCCEEDED(action->Invoke())&&controls_collapsed_);
        child.Reset();UI_REQUIRE(SUCCEEDED(rail->Navigate(NavigateDirection_FirstChild,&child))&&child);ComPtr<IRawElementProviderSimple> first;UI_REQUIRE(SUCCEEDED(child.As(&first)));
        VARIANT name{};UI_REQUIRE(SUCCEEDED(first->GetPropertyValue(UIA_NamePropertyId,&name))&&std::wstring(name.bstrVal)==L"Show controls");VariantClear(&name);action->Invoke();
        ComPtr<IRawElementProviderSimple> close;close.Attach(accessibility_->provider_for_test(4));action.Reset();UI_REQUIRE(SUCCEEDED(close.As(&action))&&SUCCEEDED(action->Invoke())&&quit.exchange(false));
        PhoneAccessibility detached;ComPtr<IRawElementProviderSimple> expired;expired.Attach(detached.provider_for_test(1));VARIANT unavailable{};UI_REQUIRE(expired->GetPropertyValue(UIA_NamePropertyId,&unavailable)==UIA_E_ELEMENTNOTAVAILABLE);
        UI_REQUIRE(SUCCEEDED(accessibility_->transport_test()));
    }
    key('D');UI_REQUIRE(disconnect.exchange(false));click(3);UI_REQUIRE(quit.exchange(false));
    UI_REQUIRE(!IsWindowVisible(window)&&!IsWindowVisible(palette_));
    hot_=keyboard_focus_=-1;dismiss_volume();set_volume(1);muted=false;place_palette();return S_OK;
#undef UI_REQUIRE
}
