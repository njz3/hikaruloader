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

// The Tab settings window over a running game, the frame rate (F12), and the
// notice of a write directory that could not be written to.

#include "ui/ui.h"
#include "ui/ui_artwork.h"
#include "ui/ui_common.h"

#include <cstdio>

#include <imgui.h>

#include "hl/config.h"
#include "hl/emulator.h"
#include "hl/ffb.h"
#include "hl/input.h"
#include "hl/paths.h"
#include "hl/pointers.h"
#include "hl/renderer.h"
#include "hl/window.h"

namespace
{
bool g_ready = false;
bool g_open = false;
bool g_closed = false;

// The write directory's notice, as Flycast's and Supermodel's: 20 s from the
// game's first frame, when it could not be written to (hl::Paths)
const Uint32 kNoticeMs = 20000;
bool g_noticeStarted = false;
Uint32 g_noticeStart = 0;

bool noticeShown()
{
    if (hl::Paths::unusable_write_dir ().empty ())
        return false;
    if (!g_noticeStarted)
    {
        g_noticeStarted = true;
        g_noticeStart = SDL_GetTicks();
    }
    return SDL_GetTicks() - g_noticeStart < kNoticeMs;
}

void drawNotice()
{
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y - 16.0f),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::Begin("##write_dir_notice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::PushTextWrapPos(viewport->WorkSize.x * 0.8f);
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", ui::tr("Cannot write to the write folder"));
    ImGui::TextUnformatted(hl::Paths::unusable_write_dir ().c_str ());
    ImGui::TextUnformatted(ui::tr("Nothing is saved: the settings and the game saves are read from the usual folders and not written."));
    ImGui::PopTextWrapPos();
    ImGui::End();
}

// The force feedback's effects, the values the device is told, for debugging
// ([ForceFeedback] HUD): at the top left, over the game. Not translated
void drawFfbHud()
{
    const hl::ForceFeedback::Status s = hl::ForceFeedback::status();
    char text[400];
    snprintf(text, sizeof(text),
             "FFB: %s\n"
             "spring: %.2f, offset %.2f, dead band %.3f\n"
             "friction: %.2f\n"
             "force: %+.2f\n"
             "sine: %.2f, %.0f Hz\n"
             "rumble: low %.2f, high %.2f%s",
             s.device[0] ? s.device : "no device", s.spring, s.spring_offset, s.spring_deadband, s.friction, s.constant,
             s.sine, s.sine_hz, s.rumble_low, s.rumble_high, s.rumble_on_wheel ? " (not sent to the wheel)" : "");
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 pos(10.0f, 8.0f);
    ImDrawList *draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(pos.x - 4.0f, pos.y - 2.0f), ImVec2(pos.x + size.x + 4.0f, pos.y + size.y + 2.0f),
                        IM_COL32(0, 0, 0, 170));
    draw->AddText(pos, IM_COL32(230, 235, 240, 255), text);
}

// Which save state F1 will load, and what became of the last save or load: at
// the top, in the middle
void drawStateNotice(const hl::Emulator::StateNotice &n)
{
    char text[160];
    const int slot = (int)n.slot;
    switch (n.message)
    {
        case hl::Emulator::STATE_SELECTED:
            if (n.age_seconds < 60)
                snprintf(text, sizeof(text), ui::tr("Save state %d (%d s ago)"), slot, n.age_seconds);
            else
                snprintf(text, sizeof(text), ui::tr("Save state %d (%d min ago)"), slot, n.age_seconds / 60);
            break;
        case hl::Emulator::STATE_EMPTY:
            snprintf(text, sizeof(text), ui::tr("Save state %d: empty"), slot);
            break;
        case hl::Emulator::STATE_LOADED:
            snprintf(text, sizeof(text), ui::tr("Save state %d loaded"), slot);
            break;
        case hl::Emulator::STATE_LOAD_FAILED:
            snprintf(text, sizeof(text), ui::tr("Save state %d could not be loaded"), slot);
            break;
        case hl::Emulator::STATE_SAVED:
            snprintf(text, sizeof(text), "%s", ui::tr("State saved as number 0"));
            break;
        case hl::Emulator::STATE_SAVE_FAILED:
            snprintf(text, sizeof(text), "%s", ui::tr("The state could not be saved"));
            break;
        default:
            return;
    }
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 pos((ImGui::GetMainViewport()->WorkSize.x - size.x) * 0.5f, 8.0f);
    ImDrawList *draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(pos.x - 6.0f, pos.y - 3.0f), ImVec2(pos.x + size.x + 6.0f, pos.y + size.y + 3.0f),
                        IM_COL32(0, 0, 0, 190));
    draw->AddText(pos, IM_COL32(255, 225, 120, 255), text);
}

void setOpen(bool open)
{
    g_open = open;
    // Read in the game only: see hl::Pointers::enable
    hl::Pointers::enable(!open);
    if (open)
        hl::Pointers::capture(false);
    else
        g_closed = true;
    // The settings need the pointer, the game may hide it
    SDL_ShowCursor(open || !hl::Config::get ()->hide_cursor ? SDL_ENABLE : SDL_DISABLE);
    // The inputs held when the settings opened would stay down in the game,
    // and the game sees none while they are open
    hl::Input::release_all ();
    hl::Input::suspend (open);
    ImGui::GetIO().ClearInputKeys();
}
} // namespace

bool ui::Interface::init (SDL_Window *window)
{
    g_ready = ui::initContext(window);
    return g_ready;
}

void ui::Interface::shutdown ()
{
    // The download stopped and the pictures freed while OpenGL is there
    ui::artworkShutdown();
    ui::shutdownContext();
    g_ready = false;
}

bool ui::Interface::settings_open ()
{
    return g_open;
}

bool ui::Interface::take_closed ()
{
    const bool was = g_closed;
    g_closed = false;
    return was;
}

const char *ui::Interface::tr (const char *text)
{
    return ui::tr(text);
}

bool ui::Interface::process_event (const SDL_Event *event)
{
    if (!g_ready)
        return false;

    if (ui::controlsCaptureEvent(event)) // an input being bound: not for the game nor the menu
        return true;
    if (hl::Input::event_action (event) == hl::ACTION_SETTINGS)
    {
        setOpen(!g_open);
        return true;
    }
    if (!g_open)
        return false;

    if (event->type == SDL_KEYDOWN && event->key.keysym.sym == SDLK_ESCAPE)
    {
        setOpen(false);
        return true;
    }
    ui::processEvent(event);
    switch (event->type)
    {
        case SDL_KEYDOWN:
        case SDL_KEYUP:
        case SDL_TEXTINPUT:
        case SDL_MOUSEMOTION:
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        case SDL_MOUSEWHEEL:
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
        case SDL_CONTROLLERAXISMOTION:
            return true;
        default:
            return false;
    }
}

bool ui::Interface::overlay_wanted ()
{
    return g_ready && (g_open || hl::Config::get ()->fps_overlay || hl::Config::get ()->ffb_hud || noticeShown() ||
                       hl::Emulator::state_notice().message != hl::Emulator::STATE_NO_MESSAGE);
}

void ui::Interface::draw_overlay (int width, int height)
{
    hl::Config *cfg = hl::Config::get ();
    const bool notice = noticeShown();
    const hl::Emulator::StateNotice state = hl::Emulator::state_notice();
    const bool state_shown = state.message != hl::Emulator::STATE_NO_MESSAGE;
    if (!g_ready || (!g_open && !cfg->fps_overlay && !cfg->ffb_hud && !notice && !state_shown))
        return;

    // Over the game, what is drawn must not bring back a hidden pointer: the
    // backend shows it at each frame otherwise
    ImGuiIO &io = ImGui::GetIO();
    if (!g_open && cfg->hide_cursor)
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    else
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    ui::newFrame();
    if (notice)
        drawNotice();
    if (cfg->ffb_hud && !g_open)
        drawFfbHud();
    if (state_shown && !g_open)
        drawStateNotice(state);
    if (cfg->fps_overlay)
    {
        char text[300];
        const char *message = hl::Window::renderer_message ();
        int len = snprintf(text, sizeof(text), ui::tr("%.1f FPS"), hl::Window::fps ());
        if (message[0] && len > 0 && len < (int)sizeof(text))
            snprintf(text + len, sizeof(text) - len, "  [%s]", message);
        const ImVec2 size = ImGui::CalcTextSize(text);
        const ImVec2 pos(ImGui::GetMainViewport()->WorkSize.x - size.x - 10.0f, 8.0f);
        ImDrawList *draw = ImGui::GetForegroundDrawList();
        draw->AddRectFilled(ImVec2(pos.x - 4.0f, pos.y - 2.0f), ImVec2(pos.x + size.x + 4.0f, pos.y + size.y + 2.0f), IM_COL32(0, 0, 0, 160));
        draw->AddText(pos, IM_COL32(230, 235, 240, 255), text);
    }
    if (g_open)
    {
        bool open = true;
        ui::drawSettings(&open, true);
        // Behind the windows
        ImGui::GetBackgroundDrawList()->AddText(ImVec2(8.0f, 8.0f), IM_COL32(220, 225, 235, 255),
                                                ui::tr("Game paused - Tab: close the settings"));
        if (!open)
            setOpen(false);
    }
    ui::renderFrame(width, height);
}
