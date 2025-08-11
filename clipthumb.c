// clipthumb.c
// Minimal .clip preview handler launcher for Wine/X11 capture
//
// Compile:
//   x86_64-w64-mingw32-gcc clipthumb.c -municode \
//       -lole32 -lshell32 -lgdi32 -luuid -ladvapi32 -lshlwapi -loleaut32 -o clipthumb.exe

#ifndef WINVER
#define WINVER 0x0500
#endif

#include <windows.h>
#include <initguid.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <objbase.h>
#include <stdio.h>

static const WCHAR* PREVIEW_WINDOW_CLASS = L"ClipThumbPreviewWindow";
static IPreviewHandler* g_previewHandler = NULL;

static void loge(const WCHAR* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(stderr, fmt, ap);
    va_end(ap);
}

LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static BOOL RegisterPreviewWindowClass(HINSTANCE hInstance) {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = PreviewWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = PREVIEW_WINDOW_CLASS;
    return RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

typedef struct {
    HWND host;
    HWND best;
    int bestArea;
} EnumCtx;

static BOOL CALLBACK find_largest_descendant(HWND child, LPARAM lParam) {
    EnumCtx* ctx = (EnumCtx*)lParam;
    if (!IsWindow(child) || !IsWindowVisible(child))
        return TRUE;

    RECT r;
    if (!GetWindowRect(child, &r))
        return TRUE;

    POINT pts[2] = { { r.left, r.top }, { r.right, r.bottom } };
    MapWindowPoints(NULL, ctx->host, pts, 2);
    int w = pts[1].x - pts[0].x;
    int h = pts[1].y - pts[0].y;
    if (w > 0 && h > 0) {
        int area = w * h;
        if (area > ctx->bestArea) {
            ctx->bestArea = area;
            ctx->best = child;
        }
    }

    EnumChildWindows(child, find_largest_descendant, lParam);
    return TRUE;
}

static BOOL ShowClipPreview(const WCHAR *inPath, const WCHAR *winTitle) {
    CLSID clsidPH;
    if (FAILED(CLSIDFromString(L"{9E6FA56B-AE86-49F9-AA1F-2517D81DE85C}", &clsidPH))) {
        loge(L"Invalid PreviewHandler CLSID\n");
        return FALSE;
    }

    IUnknown *unk = NULL;
    HRESULT hr = CoCreateInstance(&clsidPH, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IUnknown, (void**)&unk);
    if (FAILED(hr) || !unk) {
        loge(L"CoCreateInstance failed: 0x%08lX\n", hr);
        return FALSE;
    }

    IInitializeWithFile *initF = NULL;
    hr = unk->lpVtbl->QueryInterface(unk, &IID_IInitializeWithFile, (void**)&initF);
    if (FAILED(hr)) {
        loge(L"QI IInitializeWithFile failed: 0x%08lX\n", hr);
        unk->lpVtbl->Release(unk);
        return FALSE;
    }
    hr = initF->lpVtbl->Initialize(initF, (LPWSTR)inPath, STGM_READ);
    initF->lpVtbl->Release(initF);
    if (FAILED(hr)) {
        loge(L"InitializeWithFile failed: 0x%08lX\n", hr);
        unk->lpVtbl->Release(unk);
        return FALSE;
    }

    hr = unk->lpVtbl->QueryInterface(unk, &IID_IPreviewHandler, (void**)&g_previewHandler);
    unk->lpVtbl->Release(unk);
    if (FAILED(hr)) {
        loge(L"QI IPreviewHandler failed: 0x%08lX\n", hr);
        return FALSE;
    }

    HINSTANCE hInstance = GetModuleHandleW(NULL);
    if (!RegisterPreviewWindowClass(hInstance)) {
        loge(L"RegisterClass failed\n");
        g_previewHandler->lpVtbl->Release(g_previewHandler);
        g_previewHandler = NULL;
        return FALSE;
    }

    int winWidth = 800;
    int winHeight = 800;
    HWND hwndPreview = CreateWindowExW(
        0,
        PREVIEW_WINDOW_CLASS,
        winTitle ? winTitle : L"ClipThumb",
        WS_POPUP,
        100, 100,
        winWidth, winHeight,
        NULL, NULL, hInstance, NULL
    );
    if (!hwndPreview) {
        loge(L"CreateWindowEx(top) failed\n");
        g_previewHandler->lpVtbl->Release(g_previewHandler);
        g_previewHandler = NULL;
        return FALSE;
    }

    ShowWindow(hwndPreview, SW_SHOW);
    UpdateWindow(hwndPreview);

    HWND hwndHost = CreateWindowExW(
        0,
        PREVIEW_WINDOW_CLASS,
        L"Clip Preview Host",
        WS_CHILD | WS_VISIBLE,
        0, 0, winWidth, winHeight,
        hwndPreview, NULL, hInstance, NULL
    );
    if (!hwndHost) {
        loge(L"CreateWindowEx(host) failed\n");
        DestroyWindow(hwndPreview);
        g_previewHandler->lpVtbl->Release(g_previewHandler);
        g_previewHandler = NULL;
        return FALSE;
    }

    RECT rcHost = { 0, 0, winWidth, winHeight };
    hr = g_previewHandler->lpVtbl->SetWindow(g_previewHandler, hwndHost, &rcHost);
    if (FAILED(hr)) {
        loge(L"SetWindow failed: 0x%08lX\n", hr);
        DestroyWindow(hwndHost);
        DestroyWindow(hwndPreview);
        g_previewHandler->lpVtbl->Release(g_previewHandler);
        g_previewHandler = NULL;
        return FALSE;
    }

    hr = g_previewHandler->lpVtbl->DoPreview(g_previewHandler);
    if (FAILED(hr)) {
        loge(L"DoPreview failed: 0x%08lX\n", hr);
        DestroyWindow(hwndHost);
        DestroyWindow(hwndPreview);
        g_previewHandler->lpVtbl->Release(g_previewHandler);
        g_previewHandler = NULL;
        return FALSE;
    }

    // Give the handler a moment to instantiate child controls
    Sleep(200);

    // Try to size the top-level window to the largest visible child content
    EnumCtx ctx;
    ctx.host = hwndHost;
    ctx.best = NULL;
    ctx.bestArea = 0;

    EnumChildWindows(hwndHost, find_largest_descendant, (LPARAM)&ctx);

    if (ctx.best) {
        RECT cr;
        if (GetWindowRect(ctx.best, &cr)) {
            POINT pts[2] = { { cr.left, cr.top }, { cr.right, cr.bottom } };
            MapWindowPoints(NULL, hwndPreview, pts, 2);
            int childW = pts[1].x - pts[0].x;
            int childH = pts[1].y - pts[0].y;
            if (childW > 0 && childH > 0) {
                SetWindowPos(hwndHost, NULL, 0, 0, childW, childH, SWP_NOZORDER | SWP_NOACTIVATE);
                SetWindowPos(hwndPreview, NULL, 0, 0, childW, childH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                RECT newRc = { 0, 0, childW, childH };
                g_previewHandler->lpVtbl->SetWindow(g_previewHandler, hwndHost, &newRc);
                SendMessageW(hwndHost, WM_SIZE, 0, MAKELPARAM(childW, childH));
            }
        }
    }

    // Message loop until killed by the launcher
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_previewHandler->lpVtbl->Unload(g_previewHandler);
    g_previewHandler->lpVtbl->Release(g_previewHandler);
    g_previewHandler = NULL;
    return TRUE;
}

int wmain(int argc, WCHAR **argv) {
    if (argc < 3) {
        fwprintf(stderr, L"Usage: %s <file.clip> <window_title_token>\n", argv[0]);
        return 1;
    }

    const WCHAR* inPath = argv[1];
    const WCHAR* windowTitle = argv[2];

    // Optional sanity check (the launcher already enforces mimetype)
    if (!inPath || !*inPath || lstrcmpiW(PathFindExtensionW(inPath), L".clip") != 0) {
        fwprintf(stderr, L"Error: file must have .clip extension\n");
        return 1;
    }

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        fwprintf(stderr, L"CoInitializeEx failed: 0x%08lX\n", hr);
        return 1;
    }

    BOOL ok = ShowClipPreview(inPath, windowTitle);

    CoUninitialize();
    return ok ? 0 : 1;
}
