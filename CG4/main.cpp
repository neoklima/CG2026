#include "App.h"

#if defined(_DEBUG)
#include <crtdbg.h>
#endif

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, PSTR, int)
{
#if defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try
    {
        App app(hInstance);
        return app.Run();
    }
    catch(const std::exception& e)
    {
        MessageBoxW(nullptr, ToWString(e.what()).c_str(), L"Fatal error", MB_OK | MB_ICONERROR);
        return 0;
    }
}
