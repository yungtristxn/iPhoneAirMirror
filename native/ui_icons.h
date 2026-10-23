#pragma once
#include <dwrite.h>
#include <wrl/client.h>

// Use the installed Windows icon set, not approximated paths or bundled fonts.
// These stable glyphs are also present in Windows 10's Segoe MDL2 Assets.
inline HRESULT create_ui_icon_format(IDWriteFactory* writing,float size,IDWriteTextFormat** out){
    Microsoft::WRL::ComPtr<IDWriteFontCollection> fonts;
    HRESULT hr=writing->GetSystemFontCollection(&fonts);if(FAILED(hr))return hr;
    UINT32 index=0;BOOL exists=FALSE;
    const wchar_t* family=L"Segoe Fluent Icons";
    hr=fonts->FindFamilyName(family,&index,&exists);if(FAILED(hr))return hr;
    if(!exists){family=L"Segoe MDL2 Assets";hr=fonts->FindFamilyName(family,&index,&exists);if(FAILED(hr))return hr;}
    if(!exists)return DWRITE_E_NOFONT;
    hr=writing->CreateTextFormat(family,nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,size,L"en-us",out);
    if(SUCCEEDED(hr)){
        (*out)->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return hr;
}
