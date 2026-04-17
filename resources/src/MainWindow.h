#pragma once
#include "winrt/Microsoft.UI.Xaml.h"
#include "winrt/Microsoft.UI.Xaml.Controls.h"

namespace winrt::TrayApp::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();
        void InitializeComponent();
        void OnExitClick(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        HWND m_hwnd = nullptr;
        NOTIFYICONDATAW m_nid = {};
        bool m_trayCreated = false;
        static LRESULT CALLBACK WindowSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
        void CreateTrayIcon();
        void RemoveTrayIcon();
        void ShowContextMenu();
    };
}

namespace winrt::TrayApp::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}