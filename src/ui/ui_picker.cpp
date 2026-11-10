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

// The game picker: the games of data/games.json, with what the games folder holds
// of each, until one is launched.

#include "ui/ui.h"
#include "ui/ui_artwork.h"
#include "ui/ui_common.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <imgui.h>

#include "hl/config.h"
#include "hl/input.h"
#include "hl/renderer.h"
#include "hl/romset.h"
#include "hl/window.h"

namespace
{
struct Entry
{
    unsigned index; // in the game list
    unsigned found = 0, total = 0;
    bool ready() const { return total > 0 && found == total; }
};

struct Picker
{
    hl::GameList *list = nullptr;
    // The games shown: those whose files are all in the games folders
    std::vector<Entry> entries;
    // The machines' games the picker knows, shown or not
    int known = 0;
    std::string gamesDir;
    // Its folders, one or several (';')
    std::vector<std::string> gamesFolders;
    std::string message;
    int selected = 0;
    bool scrollToSelected = true;
    bool settingsOpen = false;
    int launch = -1;

    void scan()
    {
        char dir[2048];
        hl::Config::get ()->games_dir_in_use (dir, sizeof(dir));
        gamesDir = dir;
        gamesFolders = hl::RomSet::split_folders(dir);
        entries.clear();
        known = 0;
        for (unsigned i = 0; i < list->entries.size (); i++)
        {
            // The other machines' games start from the command line (-r, -z)
            const char *mach = list->entries[i].mach;
            if (strcmp(mach, "hikaru") && strcmp(mach, "gaelco"))
                continue;
            Entry entry;
            entry.index = i;
            list->check (i, dir, &entry.found, &entry.total);
            known++;
            if (entry.ready())
                entries.push_back(entry);
        }
        // The pictures of the games found, when the setting downloads them
        std::vector<std::string> sets;
        for (const Entry &entry : entries)
            if (entry.found)
                sets.push_back(game(entry).name);
        ui::artworkStart(sets);
    }

    // The picture fitted in the box, its proportions kept, centred
    static void drawFitted(ImDrawList *draw, const ui::ArtworkImage &image, ImVec2 p0, ImVec2 p1)
    {
        const float w = p1.x - p0.x, h = p1.y - p0.y;
        if (w <= 0.0f || h <= 0.0f || image.width <= 0 || image.height <= 0)
            return;
        const float s = std::min(w / image.width, h / image.height);
        const ImVec2 size(image.width * s, image.height * s);
        const ImVec2 q0(p0.x + (w - size.x) * 0.5f, p0.y + (h - size.y) * 0.5f);
        draw->AddImage(ImTextureRef(image.texture), q0, ImVec2(q0.x + size.x, q0.y + size.y));
    }

    const hl::GameEntry &game(const Entry &entry) const { return list->entries[entry.index]; }

    void statusText(const Entry &entry, char *text, size_t size) const
    {
        if (entry.ready())
            snprintf(text, size, "%s", ui::tr("Ready"));
        else if (entry.found == 0)
            snprintf(text, size, "%s", ui::tr("Not found"));
        else
            snprintf(text, size, ui::tr("Incomplete: %u of %u files found"), entry.found, entry.total);
    }

    void draw()
    {
        ui::artworkPoll();
        const ImGuiViewport *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.07f, 0.09f, 1.0f));
        if (!ImGui::Begin("##picker", nullptr, flags))
        {
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            return;
        }

        ImGui::TextUnformatted(ui::tr("Select a game"));
        const char *hint = ui::tr("Enter: launch   Tab: settings   Esc: quit");
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(hint).x - ImGui::GetStyle().WindowPadding.x);
        ImGui::TextDisabled("%s", hint);
        ImGui::Separator();

        if (gamesFolders.size() > 1)
            ImGui::TextDisabled(ui::tr("%d of %d games ready in %d folders"), (int)entries.size(), known, (int)gamesFolders.size());
        else
            ImGui::TextDisabled(ui::tr("%d of %d games ready in %s"), (int)entries.size(), known, gamesDir.c_str());
        if (!message.empty())
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", message.c_str());
            ImGui::PopTextWrapPos();
        }

        if (entries.empty())
        {
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            return;
        }

        selected = std::clamp(selected, 0, (int)entries.size() - 1);
        const float scale = ImGui::GetStyle().FontScaleMain;
        const float paneW = std::min(360.0f * scale, ImGui::GetContentRegionAvail().x * 0.34f);
        const float gridW = ImGui::GetContentRegionAvail().x - paneW - 8.0f;
        const ImVec2 tile(180.0f * scale, 135.0f * scale);
        const int columns = std::max(1, (int)(gridW / (tile.x + ImGui::GetStyle().ItemSpacing.x)));
        const int count = (int)entries.size();

        // The picker drives the selection itself, so the pane and the launch follow the highlighted tile
        if (!settingsOpen)
        {
            auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, true); };
            int sel = selected;
            if (pressed(ImGuiKey_RightArrow) || pressed(ImGuiKey_GamepadDpadRight))
                ++sel;
            if (pressed(ImGuiKey_LeftArrow) || pressed(ImGuiKey_GamepadDpadLeft))
                --sel;
            if (pressed(ImGuiKey_DownArrow) || pressed(ImGuiKey_GamepadDpadDown))
                sel += columns;
            if (pressed(ImGuiKey_UpArrow) || pressed(ImGuiKey_GamepadDpadUp))
                sel -= columns;
            sel = std::clamp(sel, 0, count - 1);
            if (sel != selected)
            {
                selected = sel;
                scrollToSelected = true;
            }
            if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter) || pressed(ImGuiKey_GamepadFaceDown))
                launch = selected;
        }

        ImGui::BeginChild("##grid", ImVec2(gridW, 0.0f), false);
        for (int i = 0; i < count; ++i)
        {
            const Entry &entry = entries[i];
            const hl::GameEntry &g = game(entry);
            if (i % columns != 0)
                ImGui::SameLine();

            ImGui::PushID(i);
            const bool isSelected = i == selected;
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            if (ImGui::Button("##tile", tile))
            {
                if (selected == i)
                    launch = i;
                else
                    selected = i;
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                launch = i;
            if (isSelected && scrollToSelected)
            {
                ImGui::SetScrollHereY(0.5f);
                scrollToSelected = false;
            }

            ImDrawList *draw = ImGui::GetWindowDrawList();
            const ImVec2 p1(p0.x + tile.x, p0.y + tile.y);
            const ImU32 text = entry.ready() ? IM_COL32(200, 205, 215, 255) : IM_COL32(120, 124, 134, 255);
            draw->AddRectFilled(p0, p1, IM_COL32(28, 30, 36, 255));
            draw->PushClipRect(p0, p1, true);
            draw->AddText(nullptr, 0.0f, ImVec2(p0.x + 8.0f * scale, p0.y + 8.0f * scale), text, g.description, nullptr, tile.x - 16.0f * scale);
            // Its picture, between the title and the footer
            ui::ArtworkImage picture;
            if (ui::artworkGet(g.name, g.parent, 0, &picture))
            {
                const float titleH = ImGui::CalcTextSize(g.description, nullptr, false, tile.x - 16.0f * scale).y;
                drawFitted(draw, picture, ImVec2(p0.x + 6.0f * scale, p0.y + 12.0f * scale + titleH),
                           ImVec2(p1.x - 6.0f * scale, p1.y - ImGui::GetTextLineHeight() - 12.0f * scale));
            }
            char footer[64];
            snprintf(footer, sizeof(footer), "%s  [%s]", g.year, g.name);
            draw->AddText(ImVec2(p0.x + 8.0f * scale, p1.y - ImGui::GetTextLineHeight() - 8.0f * scale), IM_COL32(130, 136, 150, 255), footer);
            // A dot for the status: green ready, orange incomplete, grey not found
            const ImU32 dot = entry.ready() ? IM_COL32(90, 200, 120, 255) : entry.found ? IM_COL32(230, 160, 60, 255) : IM_COL32(90, 94, 104, 255);
            draw->AddCircleFilled(ImVec2(p1.x - 12.0f * scale, p1.y - 12.0f * scale), 5.0f * scale, dot);
            draw->PopClipRect();
            draw->AddRect(p0, p1, isSelected ? IM_COL32(120, 190, 255, 255) : IM_COL32(60, 64, 74, 255), 0.0f, 0, isSelected ? 3.0f : 1.0f);
            ImGui::PopID();
        }
        ImGui::EndChild();

        // Description pane
        ImGui::SameLine();
        ImGui::BeginChild("##pane", ImVec2(paneW, 0.0f), true);
        const Entry &cur = entries[selected];
        const hl::GameEntry &g = game(cur);
        // Its pictures, one after the other every 2 s
        const int pictures = ui::artworkCount(g.name, g.parent);
        if (pictures > 0)
        {
            ui::ArtworkImage picture;
            const int shown = (int)(ImGui::GetTime() / 2.0) % pictures;
            const float w = ImGui::GetContentRegionAvail().x;
            const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + w, p0.y + w * 0.75f);
            if (ui::artworkGet(g.name, g.parent, shown, &picture) || ui::artworkGet(g.name, g.parent, 0, &picture))
                drawFitted(ImGui::GetWindowDrawList(), picture, p0, p1);
            ImGui::Dummy(ImVec2(w, w * 0.75f));
        }
        ImGui::TextWrapped("%s", g.description);
        ImGui::TextDisabled("%s  %s", g.manufacturer, g.year);
        ImGui::Separator();
        ImGui::PushTextWrapPos(0.0f);
        char status[128];
        statusText(cur, status, sizeof(status));
        ImGui::Text(ui::tr("%s: %s"), ui::tr("Status"), status);
        ImGui::Spacing();
        ImGui::TextDisabled("%s", ui::tr("Cabinet"));
        ImGui::Text(ui::tr("%s: %s"), ui::tr("Link"),
                    g.link ? ui::tr("yes (communication board, emulated: [Network] in hikaruloader.ini)") : ui::tr("no"));
        ImGui::Text(ui::tr("%s: %s"), ui::tr("Force feedback"),
                    g.ffb ? ui::tr("yes (drive board: its EPROM from srally2.zip, simulated without)") : ui::tr("no"));
        ImGui::Spacing();
        ImGui::TextDisabled("%s", ui::tr("MAME set"));
        ImGui::TextUnformatted(g.name);
        if (g.parent[0])
            ImGui::TextDisabled(ui::tr("Clone of %s"), g.parent);
        if (g.bios[0])
        {
            ImGui::TextDisabled("%s", ui::tr("BIOS"));
            ImGui::TextUnformatted(g.bios);
        }
        ImGui::Spacing();
        ImGui::TextDisabled("%s", gamesFolders.size() > 1 ? ui::tr("Games folders") : ui::tr("Games folder"));
        for (const std::string &folder : gamesFolders)
            ImGui::TextUnformatted(folder.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }
};

void drawFrame(void (*draw)(void *), void *data)
{
    int width, height;
    hl::Window::drawable_size (&width, &height);
    ui::newFrame();
    draw(data);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, width, height);
    glClearColor(0.06f, 0.07f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ui::renderFrame(width, height);
    SDL_GL_SwapWindow(hl::Window::sdl_window ());
}
} // namespace

bool ui::Interface::pick_game (hl::GameList *list, char *name, size_t size, const char *message)
{
    hl::Config *cfg = hl::Config::get ();
    Picker picker;
    picker.list = list;
    picker.message = message ? message : "";
    // A request left from the settings in a game: scanned here anyway
    ui::takeRescanRequest(false);
    picker.scan();
    for (size_t i = 0; i < picker.entries.size(); i++)
        if (!strcmp(list->entries[picker.entries[i].index].name, cfg->last_game))
            picker.selected = (int)i;

    hl::Window::set_title (nullptr);
    SDL_ShowCursor(SDL_ENABLE);
    ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;

    while (true)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (hl::Window::closing(&event))
                return false;
            if (ui::controlsCaptureEvent(&event))
                continue;
            // The controllers plugged in or out; the action the event presses
            const int action = hl::Input::handle_event (&event);
            if (action == hl::ACTION_QUIT)
                return false;
            const bool escape = event.type == SDL_KEYDOWN && !event.key.repeat &&
                                event.key.keysym.sym == SDLK_ESCAPE;
            // Escape or the Back input (Guide + B) closes the settings
            if (action == hl::ACTION_SETTINGS || ((escape || action == hl::ACTION_BACK) && picker.settingsOpen))
            {
                picker.settingsOpen = !picker.settingsOpen;
                continue;
            }
            // Escape in the list itself: out of the program
            if (escape)
                return false;
            ui::processEvent(&event);
        }
        hl::Input::release_all ();

        drawFrame([](void *data) {
            Picker *p = (Picker *)data;
            p->draw();
            if (p->settingsOpen)
            {
                bool open = true;
                ui::drawSettings(&open, false);
                if (!open)
                    p->settingsOpen = false;
            }
        }, &picker);

        // The folders changed in the settings
        if (ui::takeRescanRequest(picker.settingsOpen))
            picker.scan();

        if (picker.launch >= 0)
        {
            const Entry &entry = picker.entries[picker.launch];
            const hl::GameEntry &g = list->entries[entry.index];
            picker.launch = -1;
            if (entry.ready())
            {
                snprintf(name, size, "%s", g.name);
                snprintf(cfg->last_game, sizeof(cfg->last_game), "%s", g.name);
                // The key that launched the game is released in the game: for ImGui it would still be down
                ImGui::GetIO().ClearInputKeys();
                ImGui::GetIO().ClearInputMouse();
                return true;
            }
            char text[256];
            snprintf(text, sizeof(text), ui::tr("%s cannot start: some of its files are missing."), g.description);
            picker.message = text;
        }
        SDL_Delay(10);
    }
}

void ui::Interface::show_loading (const char *description)
{
    drawFrame([](void *data) {
        const char *description = (const char *)data;
        char text[256];
        snprintf(text, sizeof(text), ui::tr("Loading %s..."), description);
        const ImGuiViewport *viewport = ImGui::GetMainViewport();
        const ImVec2 size = ImGui::CalcTextSize(text);
        ImGui::GetBackgroundDrawList()->AddText(ImVec2(viewport->WorkPos.x + (viewport->WorkSize.x - size.x) * 0.5f,
                                                       viewport->WorkPos.y + (viewport->WorkSize.y - size.y) * 0.5f),
                                                IM_COL32(220, 225, 235, 255), text);
    }, (void *)description);
}
