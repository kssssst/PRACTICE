#include "App.h"
#include "MainWindow.h"
#include <windows.h>
#include <shellapi.h>

namespace winrt::TrayApp::implementation
{
    void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&)
    {
        // Проверка единственного экземпляра приложения через мьютекс
        HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Global\\TrayApp_SingleInstance_Mutex");
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            // Завершаемся до создания иконки трея
            exit(0);
        }

        auto window = make<MainWindow>();
        window.Activate();

        // Скрываем главное окно при старте (запуск в фоне)
        auto nativeWindow = window.as<::IWindowNative>();
        HWND hwnd;
        nativeWindow->get_WindowHandle(&hwnd);
        ShowWindow(hwnd, SW_HIDE);
    }
}