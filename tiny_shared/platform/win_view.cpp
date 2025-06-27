#include "platform_view.h"

#include "cmake_defines.h"

// C-style window callback
LRESULT CALLBACK window_callback(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    // Retrieve the graphics delegate stored in window's user data.
    auto* delegate = reinterpret_cast<Graphics_delegate*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    
    switch (message)
    {
        case WM_PAINT:
        {
            if (delegate)
            {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(window, &ps);
                
                // Create a red brush
                HBRUSH redBrush = CreateSolidBrush(RGB(255, 0, 0));
                FillRect(hdc, &ps.rcPaint, redBrush);
                DeleteObject(redBrush);

                // Call delegate's paint method if available
                delegate->draw((void*)hdc);
                EndPaint(window, &ps);
            }
            return 0;
        }
        
        case WM_DESTROY:
        {
            // Clean up delegate if needed
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            return 0;
        }
        
        // Add other message handlers as needed
        
        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

// Singleton to handle registration/unregistration of window class.
struct Window_registrar {

    static auto instance() -> Window_registrar&
    {
        static auto _instance = Window_registrar{};
        return _instance;
    }
    
    const wchar_t* class_name = tiny::Cmake_defines::wbase_identifier;
    const wchar_t* window_name = tiny::Cmake_defines::wproduct_name;

private:

    Window_registrar() 
    {
        const auto window_class = WNDCLASSW{
            .style = CS_DBLCLKS | CS_OWNDC,
            .lpfnWndProc = window_callback,
            .lpszClassName = class_name,
        };
		RegisterClassW(&window_class);
    };

public:

    ~Window_registrar()
    {
        UnregisterClassW(class_name, 0);
    }

};

Platform_view::Platform_view(std::shared_ptr<Graphics_delegate> delegate) : _delegate{delegate}
{
    const auto& registrar = Window_registrar::instance(); // Register/unregisters the window class.

    const auto size = delegate->getSize();

    auto* window = CreateWindowW(
        registrar.class_name, 
        registrar.window_name, 
        WS_CHILD | WS_CLIPSIBLINGS, 
        CW_USEDEFAULT, 
        CW_USEDEFAULT, 
        size.width,
        size.height, 
        GetDesktopWindow(), 
        nullptr, 
        nullptr, // hInstance optional per Microsoft Docs.
        nullptr
    );
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(_delegate.get()));

    _view = window;
}

Platform_view::~Platform_view()
{
    auto* window = static_cast<HWND>(_view);
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    DestroyWindow(window);
}

auto Platform_view::receive_parent(void* parent) -> void
{
    HWND parentWnd = static_cast<HWND>(parent);
    HWND window = static_cast<HWND>(_view);
    
    // Set parent window
    SetParent(window, parentWnd);
    
    // Show the window
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
}

auto Platform_view::resize(int32_t w, int32_t h) -> void
{
    HWND window = static_cast<HWND>(_view);
    // Update window size based on delegate
    const auto size = _delegate->getSize();
    SetWindowPos(window, nullptr, 0, 0, size.width, size.height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

auto Platform_view::redraw() -> void
{
    //
}

void* CreatePlatformView(Graphics_delegate* delegate)
{
    const auto& registrar = Window_registrar::instance(); // Register/unregisters the window class.

    const auto size = delegate->getSize();

    auto* window = CreateWindowW(
        registrar.class_name, 
        registrar.window_name, 
        WS_CHILD | WS_CLIPSIBLINGS, 
        CW_USEDEFAULT, 
        CW_USEDEFAULT, 
        size.width,
        size.height, 
        GetDesktopWindow(), 
        nullptr, 
        nullptr, // hInstance optional per Microsoft Docs.
        nullptr
    );
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(delegate));

    return window;
}

void DestroyPlatformView(void* view)
{
    auto* window = static_cast<HWND>(view);
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    DestroyWindow(window);
}

void RedrawPlatformView(void* view, Graphics_delegate* delegate)
{
    if (view)
    {
        HWND window = static_cast<HWND>(view);
        // Update the delegate pointer if provided
        if (delegate)
        {
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(delegate));

            // Update window size based on delegate
            const auto size = delegate->getSize();
            SetWindowPos(window, nullptr, 0, 0, size.width, size.height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}

void AttachPlatformView(void* parent, void* view)
{
    if (parent && view)
    {
        HWND parentWnd = static_cast<HWND>(parent);
        HWND window = static_cast<HWND>(view);
        
        // Set parent window
        SetParent(window, parentWnd);
        
        // Show the window
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);
    }
}