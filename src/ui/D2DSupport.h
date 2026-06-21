// SPDX-License-Identifier: GPL-3.0-or-later
// Shared Direct2D/DirectWrite helpers: process-lifetime factories (created once,
// like the themeBrush cache) plus a COLORREF->D2D color and a SafeRelease. The
// factories are device-independent and immortal, so all D2D controls share them.
#pragma once

#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>

namespace sqlterm {

// Single shared ID2D1Factory (single-threaded; all controls live on the UI
// thread). Returns nullptr only if creation fails.
inline ID2D1Factory* d2dFactory() {
    static ID2D1Factory* f = []() -> ID2D1Factory* {
        ID2D1Factory* p = nullptr;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &p);
        return p;
    }();
    return f;
}

// Single shared IDWriteFactory.
inline IDWriteFactory* dwriteFactory() {
    static IDWriteFactory* f = []() -> IDWriteFactory* {
        IDWriteFactory* p = nullptr;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(&p));
        return p;
    }();
    return f;
}

inline D2D1_COLOR_F colorToD2D(COLORREF c) {
    return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, 1.0f);
}

template <class T>
inline void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

}  // namespace sqlterm
