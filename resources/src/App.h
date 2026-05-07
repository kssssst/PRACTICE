#pragma once
#include "winrt/Microsoft.UI.Xaml.h"
#include "winrt/Microsoft.UI.Xaml.Markup.h"

namespace winrt::TrayApp::implementation
{
    struct App : AppT<App>
    {
        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);
    };
}

namespace winrt::TrayApp::factory_implementation
{
    struct App : AppT<App, implementation::App> {};
}