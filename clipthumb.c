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
#include <strsafe.h>
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

// Try to instantiate an IPreviewHandler for a given file extension by reading:
//   HKCR\<ext>\ShellEx\<handler-category-guid>\(Default) = <handler-clsid>
// Enumerate all subkeys under ShellEx and try each CLSID until one yields IPreviewHandler.
static HRESULT CreatePreviewHandlerForExtension(LPCWSTR ext, IPreviewHandler** outHandler) {
    if (!ext || !outHandler) return E_INVALIDARG;
    *outHandler = NULL;

    WCHAR keyPath[260];
    if (FAILED(StringCchPrintfW(keyPath, 260, L"%s\\ShellEx", ext))) {
        return E_FAIL;
    }

    HKEY hShellEx = NULL;
    LONG rc = RegOpenKeyExW(HKEY_CLASSES_ROOT, keyPath, 0, KEY_READ, &hShellEx);
    if (rc != ERROR_SUCCESS) {
        // Some setups register handlers on the ProgID instead of the extension.
        // Try resolving the default value of the extension to a ProgID and search there.
        WCHAR progid[260];
        DWORD type = 0, cb = sizeof(progid);
        rc = RegGetValueW(HKEY_CLASSES_ROOT, ext, NULL, RRF_RT_REG_SZ, &type, progid, &cb);
        if (rc == ERROR_SUCCESS && progid[0]) {
            if (FAILED(StringCchPrintfW(keyPath, 260, L"%s\\ShellEx", progid))) {
                return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
            }
            rc = RegOpenKeyExW(HKEY_CLASSES_ROOT, keyPath, 0, KEY_READ, &hShellEx);
        }
        if (rc != ERROR_SUCCESS) {
            return HRESULT_FROM_WIN32(rc);
        }
    }

    DWORD index = 0;
    WCHAR subkeyName[128];
    DWORD subkeyLen;
    FILETIME ft;
    HRESULT hrFound = REGDB_E_CLASSNOTREG;

    // Simple two-pass approach:
    // 1) Prefer the preview handler category {8895b1c6-b41f-4c1c-a562-0d564250836f} if present.
    // 2) Then try the rest.
    const WCHAR* kPreviewCategory = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";
    BOOL triedPreviewCategory = FALSE;

    for (int pass = 0; pass < 2 && !*outHandler; ++pass) {
        index = 0;
        while (TRUE) {
            subkeyLen = (DWORD)(sizeof(subkeyName) / sizeof(subkeyName[0]));
            LONG er = RegEnumKeyExW(hShellEx, index++, subkeyName, &subkeyLen, NULL, NULL, NULL, &ft);
            if (er == ERROR_NO_MORE_ITEMS)
                break;
            if (er != ERROR_SUCCESS)
                continue;

            if (pass == 0 && _wcsicmp(subkeyName, kPreviewCategory) != 0)
                continue; // First pass only try preview handler category

            if (pass == 1 && _wcsicmp(subkeyName, kPreviewCategory) == 0)
                continue; // Second pass: skip, already tried

            HKEY hSub = NULL;
            WCHAR subPath[260];
            if (FAILED(StringCchPrintfW(subPath, 260, L"%s\\%s", keyPath, subkeyName)))
                continue;

            if (RegOpenKeyExW(HKEY_CLASSES_ROOT, subPath, 0, KEY_READ, &hSub) != ERROR_SUCCESS)
                continue;

            WCHAR clsidStr[128] = {0};
            DWORD type = 0;
            DWORD cb = sizeof(clsidStr);
            LONG rv = RegGetValueW(hSub, NULL, NULL, RRF_RT_REG_SZ, &type, clsidStr, &cb);
            RegCloseKey(hSub);
            if (rv != ERROR_SUCCESS || !clsidStr[0])
                continue;

            CLSID clsidPH;
            if (FAILED(CLSIDFromString(clsidStr, &clsidPH)))
                continue;

            IUnknown* unk = NULL;
            HRESULT hr = CoCreateInstance(&clsidPH, NULL, CLSCTX_INPROC_SERVER, &IID_IUnknown, (void**)&unk);
            if (FAILED(hr) || !unk) {
                hrFound = hr; // remember last failure
                continue;
            }

            IPreviewHandler* ph = NULL;
            hr = unk->lpVtbl->QueryInterface(unk, &IID_IPreviewHandler, (void**)&ph);
            unk->lpVtbl->Release(unk);

            if (SUCCEEDED(hr) && ph) {
                *outHandler = ph;
                hrFound = S_OK;
                break;
            } else {
                hrFound = hr;
            }
        }
    }

    RegCloseKey(hShellEx);
    return hrFound;
}

static BOOL ShowClipPreview(const WCHAR *inPath, const WCHAR *winTitle) {
    // Discover a preview handler dynamically
    HRESULT hr = CreatePreviewHandlerForExtension(L".clip", &g_previewHandler);
    if (FAILED(hr) || !g_previewHandler) {
        loge(L"Failed to find a preview handler for .clip: 0x%08lX\n", hr);
        return FALSE;
    }

    // Initialize with file
    IInitializeWithFile *initF = NULL;
    hr = g_previewHandler->lpVtbl->QueryInterface(g_previewHandler, &IID_IInitializeWithFile, (void**)&initF);
    if (FAILED(hr) || !initF) {
        loge(L"Handler does not support IInitializeWithFile: 0x%08lX\n", hr);
        g_previewHandler->lpVtbl->Release(g_previewHandler);
        g_previewHandler = NULL;
        return FALSE;
    }
    hr = initF->lpVtbl->Initialize(initF, (LPWSTR)inPath, STGM_READ);
    initF->lpVtbl->Release(initF);
    if (FAILED(hr)) {
        loge(L"InitializeWithFile failed: 0x%08lX\n", hr);
        g_previewHandler->lpVtbl->Release(g_previewHandler);
        g_previewHandler = NULL;
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
