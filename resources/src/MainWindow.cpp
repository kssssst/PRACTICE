#include "MainWindow.h"
#include "resource.h"
#include "TrayServiceClient.h"
#include <windowsx.h>
#include <shellapi.h>

namespace winrt::TrayApp::implementation
{
    // Сообщение для уведомлений от иконки трея
    constexpr UINT WM_TRAYICON = WM_APP + 1;
    // Сообщение о пересоздании панели задач
    UINT WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");

    MainWindow::MainWindow()
    {
        InitializeComponent();

        // Находим дескриптор окна
        auto nativeWindow = this->as<::IWindowNative>();
        nativeWindow->get_WindowHandle(&m_hwnd);

        // Подклассируем окно для перехвата сообщений от трея
        SetWindowSubclass(m_hwnd, WindowSubclassProc, 0, reinterpret_cast<DWORD_PTR>(this));

        // Создаём иконку трея
        CreateTrayIcon();

        // Подписываемся на закрытие окна (крестик)
        auto closeHandler = [this](auto&&, auto&&)
        {
            // Скрываем окно, а не закрываем приложение
            ShowWindow(m_hwnd, SW_HIDE);
        };
        Closed(closeHandler);

        // Подписываемся на пункт меню "Выход"
        ExitMenuItem().Click({ this, &MainWindow::OnExitClick });
    }

    MainWindow::~MainWindow()
    {
        RemoveTrayIcon();
        RemoveWindowSubclass(m_hwnd, WindowSubclassProc, 0);
    }

    void MainWindow::OnExitClick(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        RemoveTrayIcon();
        RequestServiceStopAndWait();
        ::ExitProcess(0);
    }

    void MainWindow::CreateTrayIcon()
    {
        m_nid.cbSize = sizeof(NOTIFYICONDATAW);
        m_nid.hWnd = m_hwnd;
        m_nid.uID = 1;
        m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        m_nid.uCallbackMessage = WM_TRAYICON;
        m_nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_TRAY_ICON));
        wcscpy_s(m_nid.szTip, L"Tray Application");

        Shell_NotifyIconW(NIM_ADD, &m_nid);
        m_trayCreated = true;
    }

    void MainWindow::RemoveTrayIcon()
    {
        if (m_trayCreated)
        {
            Shell_NotifyIconW(NIM_DELETE, &m_nid);
            m_trayCreated = false;
        }
    }

    void MainWindow::ShowContextMenu()
    {
        HMENU hMenu = CreatePopupMenu();
        AppendMenuW(hMenu, MF_STRING, 1, L"Открыть");
        AppendMenuW(hMenu, MF_STRING, 2, L"Выход");

        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(m_hwnd);
        TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, m_hwnd, nullptr);
        DestroyMenu(hMenu);
    }

    LRESULT CALLBACK MainWindow::WindowSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR dwRefData)
    {
        MainWindow* pThis = reinterpret_cast<MainWindow*>(dwRefData);

        if (msg == WM_TRAYICON)
        {
            if (lParam == WM_LBUTTONUP)
            {
                // Левая кнопка мыши – показать главное окно
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
                return 0;
            }
            else if (lParam == WM_RBUTTONUP)
            {
                // Правая кнопка – показать контекстное меню
                pThis->ShowContextMenu();
                return 0;
            }
        }
        else if (msg == WM_COMMAND)
        {
            // Обработка команд контекстного меню
            switch (LOWORD(wParam))
            {
            case 1: // Открыть
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
                break;
            case 2: // Выход
                pThis->RemoveTrayIcon();
                ::ExitProcess(0);
                break;
            }
            return 0;
        }
        else if (msg == pThis->WM_TASKBARCREATED)
        {
            // Панель задач пересоздана – добавляем иконку заново
            pThis->CreateTrayIcon();
            return 0;
        }

        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }
}