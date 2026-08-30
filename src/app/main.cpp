#include "../platform/OverlayWindow.h"

#include <Windows.h>
#include <winrt/base.h>

#include <exception>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    // Per-monitor-v2 must be selected before the first HWND is created.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\Isle.DynamicOverlay.Singleton");
    if (!singleInstance || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (singleInstance) CloseHandle(singleInstance);
        return 0;
    }

    int result = 1;
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        isle::OverlayWindow window;
        if (window.create(instance, showCommand)) {
            result = window.message_loop();
        }
        winrt::uninit_apartment();
    } catch (const std::exception& ex) {
        MessageBoxA(nullptr, ex.what(), "Isle failed to start", MB_OK | MB_ICONERROR);
    } catch (...) {
        MessageBoxW(nullptr, L"Unexpected startup failure.", L"Isle failed to start", MB_OK | MB_ICONERROR);
    }

    ReleaseMutex(singleInstance);
    CloseHandle(singleInstance);
    return result;
}
