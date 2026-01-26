#include <windows.h>
#include <shobjidl.h>
#include <shobjidl_core.h>
#include <shlwapi.h>
#include <propvarutil.h>
#include <propkey.h>
#include <gdiplus.h>
#include <vector>
#include <string>
#include <random>
#include <atomic>
#include <shlobj.h>
#include <thread>
#include <wmp.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdiplus.lib")
using namespace Gdiplus;
const int winWidth = 450;

HWND g_hwndPosSlider = nullptr;
HWND g_hwndCurTime = nullptr;
HWND g_hwndTotalTime = nullptr;
HWND g_hwndVolLabel = nullptr;
HWND g_hwndAuthor = nullptr;
HWND g_hwndVolume = nullptr;
HWND g_hwndTitle = nullptr;
HWND g_hwndAlbumArt = nullptr;

WMPPlayState g_prevState = wmppsUndefined;
IWMPPlayer4* g_core = nullptr;
IWMPControls* g_controls = nullptr;
std::vector<std::wstring> g_mp3s;
std::atomic<bool> g_running{ true };
HBITMAP g_hAlbumArt = nullptr;
ULONG_PTR g_gdiplusToken = 0;
int CenterX(int controlWidth, int windowWidth = winWidth) {
    return (windowWidth - controlWidth) / 2;
}
std::wstring open_folder_dialog() {
    BROWSEINFOW bi{};
    wchar_t buffer[MAX_PATH] = L"";
    bi.hwndOwner = nullptr;
    bi.pszDisplayName = buffer;
    bi.lpszTitle = L"Select Folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl)
        return L"";
    wchar_t path[MAX_PATH];
    if (SHGetPathFromIDListW(pidl, path)) {
        CoTaskMemFree(pidl);
        return path;
    }
    CoTaskMemFree(pidl);
    return L"";
}
std::wstring open_mp3_dialog() {
    OPENFILENAMEW ofn{};
    wchar_t filePath[MAX_PATH] = L"";
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = L"MP3 Files\0*.mp3\0All Files\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn))
        return filePath;
    return L"";
}
std::wstring FormatTime(double sec) {
    int s = (int)sec;
    int m = s / 60;
    s %= 60;
    wchar_t buf[16];
    swprintf(buf, 16, L"%d:%02d", m, s);
    return buf;
}
std::vector<BYTE> ExtractAPICv23(const std::wstring& path) {
    std::vector<BYTE> result;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) {
        return result;
    }
    if (!f) return result;
    unsigned char header[10];
    if (fread(header, 1, 10, f) != 10) {
        fclose(f);
        return result;
    }
    if (header[0] != 'I' || header[1] != 'D' || header[2] != '3') {
        fclose(f);
        return result;
    }
    if (header[3] != 3) {
        fclose(f);
        return result;
    }
    int tagSize =
        ((header[6] & 0x7F) << 21) |
        ((header[7] & 0x7F) << 14) |
        ((header[8] & 0x7F) << 7) |
        (header[9] & 0x7F);
    int bytesRead = 0;
    while (bytesRead < tagSize) {
        unsigned char frameHeader[10];
        if (fread(frameHeader, 1, 10, f) != 10)
            break;
        bytesRead += 10;
        if (frameHeader[0] == 0)
            break;
        char frameID[5] = {};
        memcpy(frameID, frameHeader, 4);
        int frameSize =
            (frameHeader[4] << 24) |
            (frameHeader[5] << 16) |
            (frameHeader[6] << 8) |
            (frameHeader[7]);
        if (strcmp(frameID, "APIC") == 0) {
            std::vector<BYTE> frameData(frameSize);
            fread(frameData.data(), 1, frameSize, f);
            size_t pos = 0;
            pos++;
            while (pos < frameData.size() && frameData[pos] != 0)
                pos++;
            pos += 2;
            while (pos < frameData.size() && frameData[pos] != 0)
                pos++;
            pos++;
            if (pos < frameData.size()) {
                result.insert(result.end(),
                    frameData.begin() + pos,
                    frameData.end());
            }
            fclose(f);
            return result;
        }
        else {
            fseek(f, frameSize, SEEK_CUR);
            bytesRead += frameSize;
        }
    }
    fclose(f);
    return result;
}
HBITMAP CreateRuntimeBitmap256() {
    const int w = 256;
    const int h = 256;
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = w;
    bi.bV5Height = -h;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP bmp = CreateDIBSection(
        hdc,
        (BITMAPINFO*)&bi,
        DIB_RGB_COLORS,
        &bits,
        nullptr,
        0
    );
    ReleaseDC(nullptr, hdc);
    if (!bmp || !bits)
        return nullptr;
    DWORD* px = (DWORD*)bits;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            BYTE r = (BYTE)(x);
            BYTE g = (BYTE)(y);
            BYTE b = (BYTE)(x ^ y);
            px[y * w + x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }
    return bmp;
}
void LoadDefaultAlbumArt() {
    if (g_hAlbumArt) {
        DeleteObject(g_hAlbumArt);
        g_hAlbumArt = nullptr;
    }
    g_hAlbumArt = CreateRuntimeBitmap256();
    if (!g_hAlbumArt)
        return;
    BITMAP bm;
    GetObject(g_hAlbumArt, sizeof(bm), &bm);
    int x = CenterX(bm.bmWidth, winWidth);
    int y = 50;
    SetWindowPos(g_hwndAlbumArt, nullptr, x, y, bm.bmWidth, bm.bmHeight,
        SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessage(g_hwndAlbumArt, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)g_hAlbumArt);
}
void LoadAlbumArtToStatic(const std::wstring& path) {
    std::vector<BYTE> img = ExtractAPICv23(path);
    if (img.empty()) {
        LoadDefaultAlbumArt();
        return;
    }
    IStream* stream = SHCreateMemStream(img.data(), (UINT)img.size());
    if (!stream) {
        LoadDefaultAlbumArt();
        return;
    }
    Bitmap* bmp = Bitmap::FromStream(stream);
    stream->Release();
    if (!bmp || bmp->GetLastStatus() != Ok) {
        delete bmp;
        LoadDefaultAlbumArt();
        return;
    }
    UINT w = bmp->GetWidth();
    UINT h = bmp->GetHeight();
    const int maxSize = 256;
    double scale = min((double)maxSize / w, (double)maxSize / h);
    int dstW = (int)(w * scale);
    int dstH = (int)(h * scale);
    Bitmap resized(dstW, dstH, PixelFormat32bppARGB);
    Graphics g(&resized);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.DrawImage(bmp, 0, 0, dstW, dstH);
    delete bmp;
    HBITMAP hbm = nullptr;
    Color bg(0, 0, 0);
    if (resized.GetHBITMAP(bg, &hbm) != Ok || !hbm) {
        LoadDefaultAlbumArt();
        return;
    }
    g_hAlbumArt = hbm;
    int x = CenterX(dstW, winWidth);
    int y = 50;
    SetWindowPos(g_hwndAlbumArt, nullptr, x, y, dstW, dstH,
        SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessage(g_hwndAlbumArt, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)g_hAlbumArt);
}
bool HasID3v23Title(const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
        return false;
    unsigned char header[10];
    if (fread(header, 1, 10, f) != 10) {
        fclose(f);
        return false;
    }
    if (header[0] != 'I' || header[1] != 'D' || header[2] != '3' || header[3] != 3) {
        fclose(f);
        return false;
    }
    int tagSize =
        ((header[6] & 0x7F) << 21) |
        ((header[7] & 0x7F) << 14) |
        ((header[8] & 0x7F) << 7) |
        (header[9] & 0x7F);
    int bytesRead = 0;
    while (bytesRead < tagSize) {
        unsigned char frameHeader[10];
        if (fread(frameHeader, 1, 10, f) != 10)
            break;
        bytesRead += 10;
        if (frameHeader[0] == 0)
            break;
        char id[5] = {};
        memcpy(id, frameHeader, 4);
        int size =
            (frameHeader[4] << 24) |
            (frameHeader[5] << 16) |
            (frameHeader[6] << 8) |
            (frameHeader[7]);
        if (strcmp(id, "TIT2") == 0) {
            fclose(f);
            return true;
        }
        fseek(f, size, SEEK_CUR);
        bytesRead += size;
    }
    fclose(f);
    return false;
}
void LoadMetadata(const std::wstring& path) {
    IWMPMedia* media = nullptr;
    BSTR bpath = SysAllocString(path.c_str());
    HRESULT hr = g_core->newMedia(bpath, &media);
    SysFreeString(bpath);
    if (FAILED(hr) || !media)
        return;
    BSTR artist = nullptr;
    media->getItemInfo(SysAllocString(L"Author"), &artist);
    if (!artist || wcslen(artist) == 0) {
        SetWindowTextW(g_hwndAuthor, L"Unknown Artist");
    }
    else {
        SetWindowTextW(g_hwndAuthor, artist);
    }
    BSTR title = nullptr;
    media->getItemInfo(SysAllocString(L"Title"), &title);
    std::wstring stem;
    size_t slash = path.find_last_of(L"/\\");
    size_t start = (slash == std::wstring::npos) ? 0 : slash + 1;
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || dot < start)
        stem = path.substr(start);
    stem = path.substr(start, dot - start);
    bool hasRealTitle = HasID3v23Title(path);
    bool missingTitle = false;
    if (!title || wcslen(title) == 0) {
        missingTitle = true;
    }
    else {
        if (_wcsicmp(title, stem.c_str()) == 0 && !hasRealTitle) {
            missingTitle = true;
        }
    }
    if (missingTitle) {
        std::wstring filename;
        size_t slash = path.find_last_of(L"/\\");
        if (slash == std::wstring::npos)
            filename = path;
        filename = path.substr(slash + 1);
        std::wstring fallback = L"Unknown Title (" + filename + L")";

        SetWindowTextW(g_hwndTitle, fallback.c_str());
    }
    else {
        SetWindowTextW(g_hwndTitle, title);
    }
    LoadAlbumArtToStatic(path);
    if (artist) SysFreeString(artist);
    if (title)  SysFreeString(title);
    media->Release();
}
std::vector<std::wstring> history;
void PlayFile(std::wstring file) {
    if (!file.empty()) {
        BSTR b = SysAllocString(file.c_str());
        g_core->put_URL(b);
        SysFreeString(b);
        LoadMetadata(file);
    }
}
void PlayRandom() {
    if (g_mp3s.empty())
        return;
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<> dist(0, (int)g_mp3s.size() - 1);
    const std::wstring& file = g_mp3s[dist(rng)];
    history.push_back(file);
    PlayFile(file);
}
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wParam) == 1) { // Pause/Play
            WMPPlayState state;
            g_core->get_playState(&state);
            if (state == wmppsPlaying)
                g_controls->pause();
            else
                g_controls->play();
        }
        else if (LOWORD(wParam) == 4) { // Open File
            {
                PlayFile(open_mp3_dialog());
            }
        }
        else if (LOWORD(wParam) == 2) {
            PlayRandom();
        }
        else if (LOWORD(wParam) == 6) { // Previous
            if (history.size() != 1) {
                history.pop_back();
                PlayFile(history.back());
            }
        }
        break;
    case WM_DROPFILES:
    {
        HDROP hDrop = (HDROP)wParam;
        if (DragQueryFile(hDrop, 0xFFFFFFFF, NULL, 0) == 1) {
            wchar_t path[MAX_PATH];
            DragQueryFileW(hDrop, 0, path, MAX_PATH);
            std::wstring file = path;
            std::wstring lower;
            lower.resize(file.size());
            std::transform(file.begin(), file.end(), lower.begin(), ::towlower);
            if (lower.ends_with(L".mp3")) {
                history.push_back(file);
                PlayFile(file);
            }

        }
        DragFinish(hDrop);
        return 0;
    }
    case WM_HSCROLL:
    {
        HWND src = (HWND)lParam;
        if (src == g_hwndVolume) {
            int pos = (int)SendMessage(g_hwndVolume, TBM_GETPOS, 0, 0);
            IWMPSettings* settings = nullptr;
            if (SUCCEEDED(g_core->get_settings(&settings))) {
                settings->put_volume(pos);
                settings->Release();
            }
            wchar_t buf[16];
            swprintf(buf, 16, L"%d%%", pos);
            SetWindowTextW(g_hwndVolLabel, buf);
        }
        else if (src == g_hwndPosSlider) {
            IWMPMedia* media = nullptr;
            if (SUCCEEDED(g_core->get_currentMedia(&media)) && media) {
                double duration = 0;
                media->get_duration(&duration);
                int sliderPos = (int)SendMessage(g_hwndPosSlider, TBM_GETPOS, 0, 0);
                double newTime = (sliderPos / 1000.0) * duration;
                g_controls->put_currentPosition(newTime);
                media->Release();
            }
        }
    }
    break;
    case WM_TIMER:
        if (wParam == 100) {
            WMPPlayState state;
            g_core->get_playState(&state);
            if (g_prevState == wmppsPlaying &&
                (state == wmppsStopped || state == wmppsMediaEnded)) {
                PlayRandom();
            }
            g_prevState = state;
            if (state == wmppsPlaying) {
                IWMPMedia* media = nullptr;
                if (SUCCEEDED(g_core->get_currentMedia(&media)) && media) {
                    double duration = 0;
                    media->get_duration(&duration);
                    double pos = 0;
                    g_controls->get_currentPosition(&pos);
                    if (duration > 0) {
                        int sliderPos = (int)((pos / duration) * 1000);
                        SendMessage(g_hwndPosSlider, TBM_SETPOS, TRUE, sliderPos);
                    }
                    SetWindowTextW(g_hwndCurTime, FormatTime(pos).c_str());
                    SetWindowTextW(g_hwndTotalTime, FormatTime(duration).c_str());
                    media->Release();
                }
            }
        }
        break;
    case WM_DESTROY:
        g_running = false;
        if (g_hAlbumArt) {
            DeleteObject(g_hAlbumArt);
            g_hAlbumArt = nullptr;
        }
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
HICON CreateRuntimeIcon() {
    const int w = 32;
    const int h = 32;
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = w;
    bi.bV5Height = -h;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP colorBmp = CreateDIBSection(hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    DWORD* px = (DWORD*)bits;
    const int diameter = 32;
    const float radius = diameter * 0.5f;
    const float cx = w * 0.5f;
    const float cy = h * 0.5f;
    const DWORD brightBlue = 0xFFFFFFFF;
    const DWORD darkBlue = 0xFF0000FF;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float dx = x - cx;
            float dy = y - cy;
            float dist = sqrtf(dx * dx + dy * dy);
            if (dist > radius) {
                px[y * w + x] = 0x00000000;
                continue;
            }
            float t = (float(x) / (w - 1) + float(y) / (h - 1)) * 0.5f;
            t = max(0.f, min(1.f, t));
            BYTE r1 = (brightBlue >> 16) & 0xFF;
            BYTE g1 = (brightBlue >> 8) & 0xFF;
            BYTE b1 = (brightBlue) & 0xFF;
            BYTE r2 = (darkBlue >> 16) & 0xFF;
            BYTE g2 = (darkBlue >> 8) & 0xFF;
            BYTE b2 = (darkBlue) & 0xFF;
            BYTE r = BYTE(r1 + (r2 - r1) * t);
            BYTE g = BYTE(g1 + (g2 - g1) * t);
            BYTE b = BYTE(b1 + (b2 - b1) * t);
            DWORD color = 0xFF000000 | (r << 16) | (g << 8) | b;
            float headRadius = 4.0f;
            float headX = cx - 3.0f;
            float headY = cy + 4.0f;
            float ndx = x - headX;
            float ndy = y - headY;
            float ndist = sqrtf(ndx * ndx + ndy * ndy);
            bool inHead = ndist <= headRadius;
            bool inStem = false;
            if (x >= headX + 3 && x <= headX + 5) {
                if (y <= headY && y >= headY - 14)
                    inStem = true;
            }
            if (inHead || inStem) {
                float rt = 1.0f - t;
                BYTE nr = BYTE(r1 + (r2 - r1) * rt);
                BYTE ng = BYTE(g1 + (g2 - g1) * rt);
                BYTE nb = BYTE(b1 + (b2 - b1) * rt);
                const float dark = 0.85f;
                nr = BYTE(nr * dark);
                ng = BYTE(ng * dark);
                nb = BYTE(nb * dark);
                color = 0xFF000000 | (nr << 16) | (ng << 8) | nb;
            }
            px[y * w + x] = color;
        }
    }
    HBITMAP maskBmp = CreateBitmap(w, h, 1, 1, nullptr);
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = colorBmp;
    ii.hbmMask = maskBmp;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(colorBmp);
    DeleteObject(maskBmp);
    return icon;
}
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    CoInitialize(nullptr);
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, nullptr);
    CoCreateInstance(__uuidof(WindowsMediaPlayer), nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&g_core));
    g_core->get_controls(&g_controls);
    g_prevState = wmppsUndefined;
    std::wstring folder = open_folder_dialog();
    if (folder.empty()) {
        GdiplusShutdown(g_gdiplusToken);
        CoUninitialize();
        return 0;
    }
    std::wstring searchPath = folder;
    if (!searchPath.empty() && searchPath.back() != L'\\')
        searchPath += L'\\';
    searchPath += L"*.mp3";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        while (FindNextFileW(hFind, &fd)) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                std::wstring fullPath = folder;
                if (!fullPath.empty() && fullPath.back() != L'\\')
                    fullPath += L'\\';
                fullPath += fd.cFileName;

                g_mp3s.push_back(fullPath);
            }
        }
        FindClose(hFind);
    }
    if (g_mp3s.empty()) {
        GdiplusShutdown(g_gdiplusToken);
        CoUninitialize();
        return 0;
    }
    const wchar_t CLASS_NAME[] = L"MP3PlayerWindow";
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInst;
    wc.hIcon = CreateRuntimeIcon();
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = CLASS_NAME;
    wc.hIconSm = wc.hIcon;
    RegisterClassEx(&wc);
    HWND hwnd = CreateWindowEx(
        0, CLASS_NAME, L"PicoMP3",
        WS_OVERLAPPED | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        winWidth + 20, 520,
        nullptr, nullptr, hInst, nullptr
    );
    DragAcceptFiles(hwnd, TRUE);
    int startX = CenterX(360, winWidth);
    int buttonCount = 4;
    int spacing = 10;
    int margin = 10;
    int totalSpacing = spacing * (buttonCount - 1);
    int availableWidth = winWidth - (margin * 2) - totalSpacing;
    int buttonWidth = availableWidth / buttonCount;
    int x = margin;
    CreateWindowW(L"BUTTON", L"Previous",
        WS_VISIBLE | WS_CHILD,
        x, 10, buttonWidth, 30,
        hwnd, (HMENU)6, hInst, nullptr);
    x += buttonWidth + spacing;
    CreateWindowW(L"BUTTON", L"Pause/Play",
        WS_VISIBLE | WS_CHILD,
        x, 10, buttonWidth, 30,
        hwnd, (HMENU)1, hInst, nullptr);
    x += buttonWidth + spacing;
    CreateWindowW(L"BUTTON", L"Random",
        WS_VISIBLE | WS_CHILD,
        x, 10, buttonWidth, 30,
        hwnd, (HMENU)2, hInst, nullptr);
    x += buttonWidth + spacing;
    CreateWindowW(L"BUTTON", L"Open File",
        WS_VISIBLE | WS_CHILD,
        x, 10, buttonWidth, 30,
        hwnd, (HMENU)4, hInst, nullptr);
    g_hwndAlbumArt = CreateWindowW(
        L"STATIC", nullptr,
        WS_VISIBLE | WS_CHILD | SS_BITMAP | SS_CENTERIMAGE,
        CenterX(250, winWidth), 50, 250, 250,
        hwnd, nullptr, hInst, nullptr
    );
    int titleY = 50 + 250 + 10;
    int authorY = titleY + 20;
    g_hwndTitle = CreateWindowW(
        L"STATIC", L"",
        WS_VISIBLE | WS_CHILD | SS_CENTER,
        CenterX(350, winWidth), titleY, 350, 20,
        hwnd, nullptr, hInst, nullptr
    );
    g_hwndAuthor = CreateWindowW(
        L"STATIC", L"",
        WS_VISIBLE | WS_CHILD | SS_CENTER,
        CenterX(200, winWidth), authorY, 200, 20,
        hwnd, nullptr, hInst, nullptr
    );
    int volWidth = 200;
    int volX = CenterX(volWidth, winWidth);
    int volY = authorY + 30;
    g_hwndVolume = CreateWindowEx(
        0, TRACKBAR_CLASS, L"",
        WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
        volX, volY, volWidth, 30,
        hwnd, (HMENU)3, hInst, nullptr
    );
    g_hwndVolLabel = CreateWindowW(
        L"STATIC", L"50%",
        WS_VISIBLE | WS_CHILD | SS_CENTER,
        volX, volY + 30, volWidth, 20,
        hwnd, nullptr, hInst, nullptr
    );
    int posWidth = 300;
    int posX = CenterX(posWidth, winWidth);
    int posY = volY + 70;
    g_hwndPosSlider = CreateWindowEx(
        0, TRACKBAR_CLASS, L"",
        WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
        posX, posY, posWidth, 30,
        hwnd, (HMENU)5, hInst, nullptr
    );
    SendMessage(g_hwndPosSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
    g_hwndCurTime = CreateWindowW(
        L"STATIC", L"0:00",
        WS_VISIBLE | WS_CHILD | SS_RIGHT,
        posX - 50, posY + 5, 45, 20,
        hwnd, nullptr, hInst, nullptr
    );
    g_hwndTotalTime = CreateWindowW(
        L"STATIC", L"0:00",
        WS_VISIBLE | WS_CHILD | SS_LEFT,
        posX + posWidth + 5, posY + 5, 45, 20,
        hwnd, nullptr, hInst, nullptr
    );
    InitCommonControls();
    SendMessage(g_hwndVolume, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessage(g_hwndVolume, TBM_SETPOS, TRUE, 50);
    ShowWindow(hwnd, SW_SHOW);
    SetTimer(hwnd, 100, 300, nullptr);
    PlayRandom();
    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    if (g_hAlbumArt) {
        DeleteObject(g_hAlbumArt);
        g_hAlbumArt = nullptr;
    }
    g_controls->Release();
    g_core->Release();
    GdiplusShutdown(g_gdiplusToken);
    CoUninitialize();
    return 0;
}