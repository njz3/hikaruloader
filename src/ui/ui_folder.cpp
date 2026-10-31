/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Hikaruloader is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Hikaruloader is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Hikaruloader.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ui/ui_folder.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>

#include <SDL.h>
#include <SDL_syswm.h>

#include "hl/window.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
std::wstring widen(const char *text, UINT codePage)
{
    int n = MultiByteToWideChar(codePage, 0, text, -1, nullptr, 0);
    std::wstring wide(n > 0 ? n : 1, L'\0');
    if (n > 0)
        MultiByteToWideChar(codePage, 0, text, -1, &wide[0], n);
    wide.resize(wcslen(wide.c_str()));
    return wide;
}

// The program's window, for the dialog to be modal over it
HWND ownerWindow()
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    SDL_Window *window = hl::Window::sdl_window();
    if (window && SDL_GetWindowWMInfo(window, &info) && info.subsystem == SDL_SYSWM_WINDOWS)
        return info.info.win.window;
    return nullptr;
}
} // namespace

namespace ui
{
bool canPickFolder()
{
    return true;
}

bool pickFolder(const char *title, const char *start, std::string &folder)
{
    // The dialog wants a single-threaded apartment; SDL may have set this
    // thread up already, in either way, and then it is left as it is
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog *dialog = nullptr;
    bool ok = false;

    if (SUCCEEDED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(widen(title, CP_UTF8).c_str());
        if (start && start[0])
        {
            IShellItem *item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(widen(start, CP_ACP).c_str(), nullptr, IID_PPV_ARGS(&item))))
            {
                dialog->SetFolder(item);
                item->Release();
            }
        }
        IShellItem *result = nullptr;
        if (SUCCEEDED(dialog->Show(ownerWindow())) && SUCCEEDED(dialog->GetResult(&result)))
        {
            PWSTR path = nullptr;
            if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            {
                int n = WideCharToMultiByte(CP_ACP, 0, path, -1, nullptr, 0, nullptr, nullptr);
                if (n > 1)
                {
                    folder.assign(n, '\0');
                    WideCharToMultiByte(CP_ACP, 0, path, -1, &folder[0], n, nullptr, nullptr);
                    folder.resize(n - 1);
                    ok = true;
                }
                CoTaskMemFree(path);
            }
            result->Release();
        }
        dialog->Release();
    }
    if (SUCCEEDED(init))
        CoUninitialize();
    return ok;
}
} // namespace ui

#else

namespace ui
{
bool canPickFolder()
{
    return false;
}

bool pickFolder(const char *, const char *, std::string &)
{
    return false;
}
} // namespace ui

#endif
