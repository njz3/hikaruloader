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

// The Controls tab, laid out as linuxloader's and windowsloader's: the
// devices, each binding the actions of controls.ini to its own inputs on a
// page of its own; what the game receives; NASCAR Arcade's gear shifter and
// Star Wars: Racer Arcade's levers, as sm2-emu's; the dead zones.

#include <filesystem>
#include "ui/ui_common.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include <imgui.h>

#include "hl/pointers.h"
#include "hl/config.h"
#include "hl/controls.h"
#include "hl/gamecontrols.h"
#include "hl/ffb.h"
#include "hl/gearbox.h"
#include "hl/input.h"
#include "hl/jvs.h"

namespace ui
{
namespace
{
// The sections of controls.ini the device's page shows, in this order
struct Section
{
    const char *name;
    const char *label;
};
const Section kSections[] = {
    {"Common", N_("All games")},
    {"Player1", N_("Player 1")},
    {"Player2", N_("Player 2")},
    {"Player3", N_("Player 3")},
    {"Player4", N_("Player 4")},
    {"Driving", N_("Driving")},
    {"Flight", N_("Flying")},
    {"Pointer", N_("Guns, mice and analog sticks")},
    {"Emulator", N_("Emulator")},
};

const char *actionLabel(const std::string &key)
{
    static const struct
    {
        const char *key, *label;
    } labels[] = {
        {"Test", N_("Test button")},
        {"Service", N_("Service button")},
        {"Coin1", N_("Coin 1")},
        {"Coin2", N_("Coin 2")},
        {"Coin3", N_("Coin 3")},
        {"Coin4", N_("Coin 4")},
        {"Card", N_("Card: insert or take back")},
        {"VolumeUp", N_("Volume up")},
        {"VolumeDown", N_("Volume down")},
        {"P1_Start", N_("Start")},
        {"P1_Up", N_("Up")},
        {"P1_Down", N_("Down")},
        {"P1_Left", N_("Left")},
        {"P1_Right", N_("Right")},
        {"P1_Button1", N_("Button 1")},
        {"P1_Button2", N_("Button 2")},
        {"P1_Button3", N_("Button 3")},
        {"P1_Button4", N_("Button 4")},
        {"P1_Button5", N_("Button 5")},
        {"P1_Button6", N_("Button 6")},
        {"P2_Start", N_("Player 2: start")},
        {"P2_Up", N_("Player 2: up")},
        {"P2_Down", N_("Player 2: down")},
        {"P2_Left", N_("Player 2: left")},
        {"P2_Right", N_("Player 2: right")},
        {"P2_Button1", N_("Player 2: button 1")},
        {"P2_Button2", N_("Player 2: button 2")},
        {"P2_Button3", N_("Player 2: button 3")},
        {"P2_Button4", N_("Player 2: button 4")},
        {"P2_Button5", N_("Player 2: button 5")},
        {"P2_Button6", N_("Player 2: button 6")},
        {"P3_Start", N_("Player 3: start")},
        {"P3_Up", N_("Player 3: up")},
        {"P3_Down", N_("Player 3: down")},
        {"P3_Left", N_("Player 3: left")},
        {"P3_Right", N_("Player 3: right")},
        {"P3_Button1", N_("Player 3: button 1")},
        {"P3_Button2", N_("Player 3: button 2")},
        {"P3_Button3", N_("Player 3: button 3")},
        {"P3_Button4", N_("Player 3: button 4")},
        {"P3_Button5", N_("Player 3: button 5")},
        {"P3_Button6", N_("Player 3: button 6")},
        {"P4_Start", N_("Player 4: start")},
        {"P4_Up", N_("Player 4: up")},
        {"P4_Down", N_("Player 4: down")},
        {"P4_Left", N_("Player 4: left")},
        {"P4_Right", N_("Player 4: right")},
        {"P4_Button1", N_("Player 4: button 1")},
        {"P4_Button2", N_("Player 4: button 2")},
        {"P4_Button3", N_("Player 4: button 3")},
        {"P4_Button4", N_("Player 4: button 4")},
        {"P4_Button5", N_("Player 4: button 5")},
        {"P4_Button6", N_("Player 4: button 6")},
        {"Steering", N_("Steering wheel")},
        {"Accelerator", N_("Accelerator")},
        {"Brake", N_("Brake")},
        {"Accelerator_Digital", N_("Accelerator (all the way)")},
        {"Brake_Digital", N_("Brake (all the way)")},
        {"ViewChange", N_("Change view")},
        {"ShiftUp", N_("Shift up")},
        {"ShiftDown", N_("Shift down")},
        {"GearReverse", N_("Reverse")},
        {"GearNeutral", N_("Neutral")},
        {"Gear1", N_("Gear 1")},
        {"Gear2", N_("Gear 2")},
        {"Gear3", N_("Gear 3")},
        {"Gear4", N_("Gear 4")},
        {"Gear5", N_("Gear 5")},
        {"Gear6", N_("Gear 6")},
        {"Clutch", N_("Clutch")},
        {"Clutch_Digital", N_("Clutch (all the way)")},
        {"Horn", N_("Horn")},
        {"LeftBrake", N_("Left brake lever")},
        {"RightBrake", N_("Right brake lever")},
        {"LeftLever", N_("Left lever (its engine's thrust)")},
        {"RightLever", N_("Right lever (its engine's thrust)")},
        {"LeftLever_Digital", N_("Left lever (all the way)")},
        {"RightLever_Digital", N_("Right lever (all the way)")},
        {"Elevator", N_("Elevator (wheel pushed, pulled)")},
        {"Aileron", N_("Aileron (wheel turned)")},
        {"Rudder", N_("Rudder pedals")},
        {"ThrustL", N_("Left thrust lever")},
        {"ThrustR", N_("Right thrust lever")},
        {"P1_GunX", N_("Sight or gun left and right")},
        {"P1_GunY", N_("Sight or gun up and down")},
        {"P1_Trigger", N_("Trigger")},
        {"P1_Reload", N_("Gun off the screen, reload")},
        {"P1_Fire2", N_("Second button")},
        {"P1_Fire3", N_("Third button")},
        {"P2_GunX", N_("Player 2's sight or gun left and right")},
        {"P2_GunY", N_("Player 2's sight or gun up and down")},
        {"P2_Trigger", N_("Player 2's trigger")},
        {"P2_Reload", N_("Player 2's gun off the screen")},
        {"P2_Fire2", N_("Player 2's second button")},
        {"P2_Fire3", N_("Player 2's third button")},
        {"P1_StickX", N_("Analog stick left and right")},
        {"P1_StickY", N_("Analog stick up and down")},
        {"P2_StickX", N_("Player 2's analog stick left and right")},
        {"P2_StickY", N_("Player 2's analog stick up and down")},
        {"P3_StickX", N_("Player 3's analog stick left and right")},
        {"P3_StickY", N_("Player 3's analog stick up and down")},
        {"P4_StickX", N_("Player 4's analog stick left and right")},
        {"P4_StickY", N_("Player 4's analog stick up and down")},
        {"P1_Kick", N_("Kick (its strength)")},
        {"P2_Kick", N_("Player 2's kick")},
        {"P3_Kick", N_("Player 3's kick")},
        {"P4_Kick", N_("Player 4's kick")},
        {"Pause", N_("Pause")},
        {"SaveState", N_("Save the state")},
        {"LoadState", N_("Load the state")},
        {"StateNewer", N_("Newer save state")},
        {"StateOlder", N_("Older save state")},
        {"Menu", N_("Settings (this window)")},
        {"Back", N_("Back to the picker, or quit")},
        {"Quit", N_("Quit")},
        {"ShowFps", N_("Show the frame rate")},
        {"NextScreen", N_("Next board's picture (several boards)")},
        {"Aim", N_("Stick moving a sight")},
    };
    for (const auto &l : labels)
        if (!strcasecmp(key.c_str(), l.key))
            return tr(l.label);
    // An axis's keys: the axis's name, and which way
    static std::map<std::string, std::string> made;
    for (const char *suffix : {"_Left", "_Right"})
    {
        size_t n = strlen(suffix);
        if (key.size() > n && !strcasecmp(key.c_str() + key.size() - n, suffix))
        {
            std::string base = key.substr(0, key.size() - n);
            const char *label = actionLabel(base);
            if (label != base.c_str() && strcasecmp(label, base.c_str()))
                return (made[key] = std::string(label) + tr(*suffix == '_' && suffix[1] == 'L' ? " (down, left)" : " (up, right)")).c_str();
        }
    }
    return key.c_str();
}

// How a row moves (a switch when unknown)
hl::Controls::RowKind rowKind(const std::string &key)
{
    const hl::Controls::RowDef *row = hl::Controls::row_def(key.c_str());
    return row ? row->kind : hl::Controls::ROW_SWITCH;
}

bool isSteering(const std::string &key)
{
    return !strcasecmp(key.c_str(), "Steering");
}

// The sights and the sticks: an axis each, without a dead zone
bool isPointerAxis(const std::string &key)
{
    const hl::Controls::RowDef *row = hl::Controls::row_def(key.c_str());
    return row && !strcmp(row->section, "Pointer") && (row->kind == hl::Controls::ROW_LEVER || row->kind == hl::Controls::ROW_AXIS);
}
bool isPedal(const std::string &key)
{
    return rowKind(key) == hl::Controls::ROW_PEDAL;
}

// A title bar across the page
void header(const char *label)
{
    const ImGuiStyle &style = ImGui::GetStyle();
    ImGui::Spacing();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x, height = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), ImGui::GetColorU32(ImGuiCol_Header),
                                              style.FrameRounding);
    ImGui::SetCursorScreenPos(ImVec2(pos.x + style.FramePadding.x, pos.y + style.FramePadding.y));
    ImGui::TextUnformatted(label);
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + height + style.ItemSpacing.y));
    ImGui::Dummy(ImVec2(0, 0));
}

// sm2-emu's centre bar (https://github.com/dmanlfc/sm2-emu, GPL v3, Daniel
// Martin (dmanlfc) and the sm2-emu contributors), as linuxloader and
// windowsloader have it: the axis around its middle, the dead zone shaded
void centreBar(const char *id, double value, double deadzone)
{
    const ImGuiStyle &style = ImGui::GetStyle();
    float width = ImGui::GetContentRegionAvail().x, height = ImGui::GetFrameHeight() * 0.6f;
    if (width < 1)
        width = 1;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(width, height));
    ImDrawList *draw = ImGui::GetWindowDrawList();
    float mid = pos.x + width * 0.5f;
    draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), ImGui::GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);
    if (deadzone > 0)
    {
        float half = width * 0.5f * (float)deadzone;
        draw->AddRectFilled(ImVec2(mid - half, pos.y), ImVec2(mid + half, pos.y + height), ImGui::GetColorU32(ImGuiCol_FrameBgHovered));
    }
    draw->AddLine(ImVec2(mid, pos.y), ImVec2(mid, pos.y + height), ImGui::GetColorU32(ImGuiCol_TextDisabled));
    value = value < -1 ? -1 : value > 1 ? 1 : value;
    float x = (float)(int)(mid + width * 0.5f * (float)value + 0.5f);
    bool inside = (value < 0 ? -value : value) <= deadzone;
    draw->AddRectFilled(ImVec2(x - 2, pos.y), ImVec2(x + 2, pos.y + height),
                        ImGui::GetColorU32(inside ? ImGuiCol_TextDisabled : ImGuiCol_SliderGrabActive));
}

// A dot lit while the input is held
void pressDot(bool on)
{
    float size = ImGui::GetFontSize() * 0.7f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, ImGui::GetFrameHeight()));
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(pos.x + size * 0.5f, pos.y + ImGui::GetFrameHeight() * 0.5f), size * 0.5f,
                                                ImGui::GetColorU32(on ? ImGuiCol_SliderGrabActive : ImGuiCol_FrameBg));
}

// A pedal's travel, 0 up to 1 pressed, as its input reads the axis
float pedalLevel(const std::string &token, int raw)
{
    float level;
    if (token.find("_POSITIVE_SIDE") != std::string::npos || token.find("_POSITIVE_HALF") != std::string::npos)
        level = raw > 0 ? raw / 32767.0f : 0.0f;
    else if (token.find("_NEGATIVE_SIDE") != std::string::npos || token.find("_NEGATIVE_HALF") != std::string::npos)
        level = raw < 0 ? -raw / 32768.0f : 0.0f;
    else if (token.find("TRIGGER") != std::string::npos)
        level = raw / 32767.0f;
    else
        level = (raw + 32768.0f) / 65535.0f;
    if (token.find("_INVERTED") != std::string::npos)
        level = 1.0f - level;
    return level < 0 ? 0 : level > 1 ? 1 : level;
}

// [Config] <Row>_DeadZone of a row, a player's without its P<n>_
double deadzoneOf(const std::string &key)
{
    std::string k = key;
    if (k.size() > 3 && k[0] == 'P' && isdigit((unsigned char)k[1]) && k[2] == '_')
        k.erase(0, 3);
    std::string v = hl::Controls::get().value("Config", (k + "_DeadZone").c_str());
    return v.empty() ? 0.0 : atoi(v.c_str()) / 32767.0;
}

// The device's input: a key for the keyboard, JOY<n>_ for a controller, and
// GC<n>_ for a game controller (a joystick has no GC<n>_ inputs)
bool ownsToken(const hl::Controls::Device &device, const std::string &token)
{
    return hl::Controls::owns(device.player, token) && (device.game_controller || strncasecmp(token.c_str(), "GC", 2) != 0);
}

// The device's inputs of a row (mine), or the other devices' (!mine)
std::vector<std::string> deviceTokens(const hl::Controls::Device &device, const std::string &value, bool mine)
{
    std::vector<std::string> out;
    for (const auto &t : hl::Controls::alternatives(value))
        if (ownsToken(device, t) == mine)
            out.push_back(t);
    return out;
}

// The row's value with the device's inputs replaced by those given, where
// its first one was (else after the others'), the other devices' kept
std::string replaceDeviceTokens(const hl::Controls::Device &device, const std::string &value, const std::vector<std::string> &mine)
{
    std::vector<std::string> tokens;
    bool placed = false;
    for (const auto &t : hl::Controls::alternatives(value))
        if (!ownsToken(device, t))
            tokens.push_back(t);
        else if (!placed)
        {
            tokens.insert(tokens.end(), mine.begin(), mine.end());
            placed = true;
        }
    if (!placed)
        tokens.insert(tokens.end(), mine.begin(), mine.end());
    return hl::Controls::join(tokens);
}

std::string describeAll(const std::vector<std::string> &tokens)
{
    std::string text;
    for (const auto &t : tokens)
        text += (text.empty() ? "" : ", ") + hl::Controls::describe(t, tr, hl::Config::get()->buttons_from_one);
    return text;
}

// The device whose page is open
struct Mapping
{
    bool requestOpen = false;
    hl::Controls::Device device{-1, false, ""};
} g_mapping;

// The row whose input is being captured
struct Capture
{
    bool requestOpen = false;
    bool append = false; // "Add": the input joins the device's; "Bind": it replaces them
    std::string section, key;
} g_capture;

const char *deviceName(const hl::Controls::Device &d)
{
    return d.player < 0 ? tr("Keyboard") : d.name.c_str();
}

// label: the game's name for it, NULL the row's own
void bindingRow(const char *section, const hl::Controls::Row &row, const char *label = NULL)
{
    hl::Controls &controls = hl::Controls::get();
    const int player = g_mapping.device.player;
    ImGui::PushID(row.key.c_str());
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label ? label : actionLabel(row.key));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", row.key.c_str());

    std::vector<std::string> mine = deviceTokens(g_mapping.device, row.value, true);
    ImGui::TableSetColumnIndex(1);
    ImGui::AlignTextToFramePadding();
    if (mine.empty())
        ImGui::TextDisabled("-");
    else
    {
        ImGui::TextUnformatted(describeAll(mine).c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", hl::Controls::join(mine).c_str());
    }

    // The input now: an axis's position, a pedal's travel, a switch's dot
    ImGui::TableSetColumnIndex(2);
    bool on = false, any = false;
    int raw = 0, axis = -1;
    for (size_t k = 0; k < mine.size(); k++)
    {
        bool held;
        int value;
        int kind = controls.input_state(mine[k], &held, &value);
        if (kind == 2 && axis < 0)
        {
            axis = (int)k;
            raw = value;
        }
        on |= kind == 1 && held;
        any |= kind != 0;
    }
    if (axis >= 0 && isPedal(row.key))
        ImGui::ProgressBar(pedalLevel(mine[axis], raw), ImVec2(-FLT_MIN, ImGui::GetFrameHeight() * 0.6f), "");
    else if (axis >= 0)
        centreBar("##axis", raw / 32767.0, isSteering(row.key) ? deadzoneOf(row.key) : 0.0);
    else if (any)
        pressDot(on);

    ImGui::TableSetColumnIndex(3);
    bool bind = ImGui::Button(tr("Bind"));
    ImGui::SameLine();
    ImGui::BeginDisabled(mine.empty()); // nothing to add to: "Bind"
    bool add = ImGui::Button(tr("Add"));
    ImGui::EndDisabled();
    if (bind || add)
    {
        g_capture.requestOpen = true;
        g_capture.append = add;
        g_capture.section = section;
        g_capture.key = row.key;
        hl::Controls::RowKind kind = rowKind(row.key);
        controls.capture(player, kind == hl::Controls::ROW_PEDAL ? hl::Controls::CAPTURE_PEDAL
                                 : (kind == hl::Controls::ROW_AXIS || kind == hl::Controls::ROW_LEVER) ? hl::Controls::CAPTURE_STEERING
                                                                                                         : hl::Controls::CAPTURE_BUTTON);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(mine.empty());
    if (ImGui::Button(tr("Unbind")))
        controls.set_value(section, row.key.c_str(), replaceDeviceTokens(g_mapping.device, row.value, {}));
    ImGui::EndDisabled();
    ImGui::SameLine();
    // The device's inputs back to the default ones (its profile's when it
    // has one), the others' kept
    std::vector<std::string> def = deviceTokens(g_mapping.device, controls.device_default(section, row.key.c_str(), g_mapping.device.player, g_mapping.device.game_controller), true);
    ImGui::BeginDisabled(def == mine);
    if (ImGui::Button(tr("Default")))
        controls.set_value(section, row.key.c_str(), replaceDeviceTokens(g_mapping.device, row.value, def));
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(tr("Default: %s"), def.empty() ? "-" : describeAll(def).c_str());
    ImGui::PopID();
}

// The device's inputs back to the defaults in every row (its profile's when
// it has one), the others' kept; clear: none
void resetDevice(bool clear = false)
{
    hl::Controls &controls = hl::Controls::get();
    for (const Section &s : kSections)
        for (const auto &row : controls.rows(s.name))
        {
            std::vector<std::string> def;
            if (!clear)
                def = deviceTokens(g_mapping.device, controls.device_default(s.name, row.key.c_str(), g_mapping.device.player, g_mapping.device.game_controller), true);
            if (def != deviceTokens(g_mapping.device, row.value, true))
                controls.set_value(s.name, row.key.c_str(), replaceDeviceTokens(g_mapping.device, row.value, def));
        }
}

// The device as the Controls tab lists it now (its profile found again)
void refreshDevice()
{
    for (const auto &d : hl::Controls::get().devices())
        if (d.player == g_mapping.device.player)
            g_mapping.device = d;
}

// A yes or no question over the page: true on Yes
bool confirm(const char *id, const char *question)
{
    bool yes = false;
    if (ImGui::BeginPopupModal(id, NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(question);
        ImGui::PopTextWrapPos();
        if (ImGui::Button(tr("Yes")))
        {
            yes = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("No")))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    return yes;
}

// The capture's window: the device's next input, written to the row
void captureWindow()
{
    hl::Controls &controls = hl::Controls::get();
    if (g_capture.requestOpen)
    {
        ImGui::OpenPopup("##capture");
        g_capture.requestOpen = false;
    }
    if (!ImGui::BeginPopupModal("##capture", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        return;
    std::string got;
    if (!controls.is_capturing())
        ImGui::CloseCurrentPopup();
    else if (controls.capture_step(got))
    {
        // "Bind": the device's inputs replaced by it; "Add": it after them, once
        std::string value = controls.value(g_capture.section.c_str(), g_capture.key.c_str());
        std::vector<std::string> mine;
        if (g_capture.append)
            mine = deviceTokens(g_mapping.device, value, true);
        bool there = false;
        for (const auto &t : mine)
            there |= !strcasecmp(t.c_str(), got.c_str());
        if (!there)
            mine.push_back(got);
        controls.set_value(g_capture.section.c_str(), g_capture.key.c_str(), replaceDeviceTokens(g_mapping.device, value, mine));
        ImGui::CloseCurrentPopup();
    }
    else
    {
        ImGui::Text(tr("Press an input of %s for: %s"), deviceName(g_mapping.device), actionLabel(g_capture.key));
        const bool mouse = g_mapping.device.player >= hl::Controls::MOUSE_PLAYER;
        if (mouse && isPointerAxis(g_capture.key))
            ImGui::TextUnformatted(tr("Move it along that axis."));
        else if (isSteering(g_capture.key) || isPointerAxis(g_capture.key))
            ImGui::TextUnformatted(tr("Turn to the left."));
        else if (isPedal(g_capture.key))
            ImGui::TextUnformatted(tr("Press the pedal."));
        else if (g_mapping.device.game_controller)
            ImGui::TextUnformatted(tr("Hold Guide and press a button for a hotkey: the game does not see it."));
        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr("Escape: cancel"));
        // With a mouse, a click on Cancel would be the input captured
        if (!mouse && ImGui::Button(tr("Cancel")))
        {
            controls.capture_cancel();
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndPopup();
}

// The device's page, over the whole window: its inputs for every action
void mappingPage()
{
    hl::Controls &controls = hl::Controls::get();
    if (g_mapping.requestOpen)
    {
        ImGui::OpenPopup("###mapping");
        g_mapping.requestOpen = false;
    }
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowBgAlpha(1.0f);
    char title[192];
    snprintf(title, sizeof title, tr("Bindings: %s"), deviceName(g_mapping.device));
    std::string id = std::string(title) + "###mapping";
    // A mouse's page reads it: see hl::Pointers::enable
    static bool readingMouse = false;
    const bool mouseWanted = g_mapping.device.player >= hl::Controls::MOUSE_PLAYER && ImGui::IsPopupOpen("###mapping");
    if (mouseWanted != readingMouse)
    {
        readingMouse = mouseWanted;
        hl::Pointers::enable(mouseWanted);
    }
    if (!ImGui::BeginPopupModal(id.c_str(), NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
        return;

    float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    if (ImGui::Button(tr("Close")))
    {
        controls.capture_cancel();
        ImGui::CloseCurrentPopup();
    }
    // On the right, away from Close: the device's bindings to a profile,
    // cleared, back to the defaults (its profile's when it has one)
    const bool controller = g_mapping.device.player >= 0 && g_mapping.device.player < hl::Controls::MOUSE_PLAYER;
    const char *resetLabel = tr("Reset..."), *clearLabel = tr("Clear..."), *saveLabel = tr("Save as a profile...");
    const float pad = ImGui::GetStyle().FramePadding.x * 2, gap = ImGui::GetStyle().ItemSpacing.x;
    float width = ImGui::CalcTextSize(resetLabel).x + pad + gap + ImGui::CalcTextSize(clearLabel).x + pad;
    if (controller)
        width += gap + ImGui::CalcTextSize(saveLabel).x + pad;
    ImGui::SameLine(right - width);
    static std::string saved;
    if (controller)
    {
        if (ImGui::Button(saveLabel))
            ImGui::OpenPopup("##saveprofile");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("Its bindings to a file of its own, put on it each time it is first connected, on this PC or another: share it."));
        ImGui::SameLine();
    }
    if (ImGui::Button(clearLabel))
        ImGui::OpenPopup("##clear");
    ImGui::SameLine();
    if (ImGui::Button(resetLabel))
        ImGui::OpenPopup("##reset");
    if (confirm("##reset", g_mapping.device.profile.empty() ? tr("Put this device's bindings back to the defaults?")
                                                            : tr("Put this device's bindings back to its profile's?")))
        resetDevice();
    if (confirm("##clear", tr("Remove all of this device's bindings?")))
        resetDevice(true);
    if (confirm("##saveprofile", tr("Save this device's bindings as its profile, in the devices folder beside controls.ini?")))
    {
        saved = controls.save_profile(g_mapping.device.player);
        refreshDevice();
        ImGui::OpenPopup("##savedprofile");
    }
    if (ImGui::BeginPopupModal("##savedprofile", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
    {
        if (saved.empty())
            ImGui::TextUnformatted(tr("The profile could not be written: see hikaruloader.log."));
        else
        {
            // Its folder's name and its own: the whole path on hover
            ImGui::TextUnformatted(tr("The profile is in:"));
            std::string shortName = "devices/" + std::filesystem::path(saved).filename().string();
            ImGui::TextUnformatted(shortName.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", saved.c_str());
            ImGui::TextDisabled("%s", tr("Give it to whoever has the same device: in their devices folder, it is put on it the first time it is connected."));
        }
        if (ImGui::Button(tr("Close")))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    // Its profile, if any
    if (controller)
    {
        if (g_mapping.device.profile.empty())
            ImGui::TextDisabled("%s", tr("No profile for this device: its defaults are the common ones."));
        else
        {
            std::filesystem::path file(g_mapping.device.profile_file);
            std::string shortName = file.parent_path().filename().string() + "/" + file.filename().string();
            ImGui::TextDisabled(tr("Profile: %s"), shortName.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n%s", tr("Its bindings when it is first connected, and what Default and Reset put back."),
                                  g_mapping.device.profile_file.c_str());
        }
    }

    if (ImGui::BeginChild("##bindings", ImVec2(0, 0), ImGuiChildFlags_Borders))
    {
        // The game running's own controls first, under its names: the rows
        // they read (each once), and an axis's keys
        struct Own
        {
            std::string section, label;
            hl::Controls::Row row;
        };
        std::vector<Own> own;
        std::vector<const hl::Controls::RowDef *> seen;
        std::vector<std::pair<const hl::Controls::RowDef *, std::string>> roles;
        for (const auto &p : hl::GameControls::get().places())
            if (std::find(seen.begin(), seen.end(), p.row) == seen.end())
            {
                seen.push_back(p.row);
                roles.push_back({p.row, p.label});
            }
        if (!hl::GameControls::get().gears().empty())
            for (const char *key : {"ShiftUp", "ShiftDown", "GearReverse", "GearNeutral", "Gear1", "Gear2", "Gear3", "Gear4", "Gear5", "Gear6"})
                roles.push_back({hl::Controls::row_def(key), ""});
        for (const auto &r : roles)
        {
            static const struct
            {
                const char *suffix, *more;
            } parts[] = {{"", ""}, {"_Left", N_(" (down, left)")}, {"_Right", N_(" (up, right)")}, {"_Digital", N_(" (a key)")}};
            if (!r.first)
                continue;
            for (const auto &part : parts)
            {
                std::string key = std::string(r.first->key) + part.suffix;
                std::string name = r.second.empty() ? std::string(actionLabel(key))
                                                     : r.second + (*part.more ? tr(part.more) : "") + "  (" + actionLabel(r.first->key) + ")";
                for (const auto &row : controls.rows(r.first->section))
                    if (!strcasecmp(row.key.c_str(), key.c_str()))
                        own.push_back({r.first->section, name, row});
            }
        }
        if (!own.empty())
        {
            header(tr("This game's controls"));
            if (ImGui::BeginTable("##own", 4, ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthStretch, 0.34f);
                ImGui::TableSetupColumn("##binding", ImGuiTableColumnFlags_WidthStretch, 0.30f);
                ImGui::TableSetupColumn("##now", ImGuiTableColumnFlags_WidthStretch, 0.22f);
                ImGui::TableSetupColumn("##buttons", ImGuiTableColumnFlags_WidthFixed);
                for (const auto &o : own)
                {
                    ImGui::PushID(o.section.c_str());
                    bindingRow(o.section.c_str(), o.row, o.label.c_str());
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::Spacing();
        }
        // The others, all the games' rows: folded while a game runs
        bool all = own.empty() || ImGui::CollapsingHeader(tr("All the controls"));
        for (const Section &s : kSections)
        {
            if (!all)
                break;
            std::vector<hl::Controls::Row> rows = controls.rows(s.name);
            if (rows.empty())
                continue;
            header(tr(s.label));
            if (ImGui::BeginTable(s.name, 4, ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthStretch, 0.34f);
                ImGui::TableSetupColumn("##binding", ImGuiTableColumnFlags_WidthStretch, 0.30f);
                ImGui::TableSetupColumn("##now", ImGuiTableColumnFlags_WidthStretch, 0.22f);
                ImGui::TableSetupColumn("##buttons", ImGuiTableColumnFlags_WidthFixed);
                for (const auto &row : rows)
                    bindingRow(s.name, row);
                ImGui::EndTable();
            }
        }
    }
    ImGui::EndChild();

    captureWindow();
    ImGui::EndPopup();
}

// The [Config] options: the dead zones as sliders, the toggles as boxes, the
// rest as numbers; each change saved
void drawOptions()
{
    hl::Controls &controls = hl::Controls::get();
    for (const auto &row : controls.rows("Config"))
    {
        size_t len = row.key.size();
        bool deadZone = len > 9 && !strcasecmp(row.key.c_str() + len - 9, "_DeadZone");
        bool toggle = len > 7 && !strcasecmp(row.key.c_str() + len - 7, "_Toggle");
        std::string label = std::string(deadZone ? actionLabel(row.key.substr(0, len - 9)) : row.key.c_str()) + "##" + row.key;
        char value[32];
        if (deadZone)
        {
            int v = atoi(row.value.c_str());
            if (ImGui::SliderInt(label.c_str(), &v, 0, 16000))
            {
                snprintf(value, sizeof value, "%d", v);
                controls.set_value("Config", row.key.c_str(), value);
            }
        }
        else if (toggle)
        {
            bool v = atoi(row.value.c_str()) != 0;
            if (ImGui::Checkbox(label.c_str(), &v))
                controls.set_value("Config", row.key.c_str(), v ? "1" : "0");
        }
        else
        {
            int v = atoi(row.value.c_str());
            if (ImGui::InputInt(label.c_str(), &v))
            {
                snprintf(value, sizeof value, "%d", v);
                controls.set_value("Config", row.key.c_str(), value);
            }
        }
    }
}

// "270 degrees", or the wheel's whole turn for 0
void degreesLabel(int degrees, char *text, size_t size)
{
    if (degrees <= 0)
        snprintf(text, size, "%s", tr("Its whole turn"));
    else
        snprintf(text, size, tr("%d degrees"), degrees);
}

// A combo of degrees: the choices, the value kept even when not among them
bool degreesCombo(const char *label, int *value, const int *choices, size_t count)
{
    char text[64];
    bool changed = false;
    degreesLabel(*value, text, sizeof text);
    if (ImGui::BeginCombo(label, text))
    {
        for (size_t i = 0; i < count; i++)
        {
            degreesLabel(choices[i], text, sizeof text);
            if (ImGui::Selectable(text, choices[i] == *value) && choices[i] != *value)
            {
                *value = choices[i];
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

// [Input] WHEEL_ROTATION and WHEEL_RANGE: saved at once to hikaruloader.ini,
// alone (the window's other changes wait for Save), as the buttons' numbers;
// the force feedback's spring follows at once
void drawWheel()
{
    hl::Config *cfg = hl::Config::get();
    int rotation = cfg->wheel_rotation, range = cfg->wheel_range;
    bool changed = degreesCombo(tr("Wheel rotation"), &rotation, hl::WHEEL_ROTATIONS, IM_ARRAYSIZE(hl::WHEEL_ROTATIONS));
    ImGui::BeginDisabled(rotation <= 0);
    changed |= degreesCombo(tr("Your wheel's turn"), &range, hl::WHEEL_RANGES, IM_ARRAYSIZE(hl::WHEEL_RANGES));
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr("How far your wheel turns from one end to the other for NASCAR Arcade's wheel: its whole turn, or fewer "
                                 "degrees, the game's wheel then at its end first; the force feedback's spring as much stiffer. "
                                 "A wheel's axis, not a gamepad's stick. Your wheel's turn is the one set in its driver (Logitech "
                                 "G HUB, Thrustmaster's or Fanatec's control panel): 900 degrees for most."));
    if (rotation > 0 && rotation >= range)
        ImGui::TextDisabled(tr("Your wheel turns %d degrees: the game's wheel takes its whole turn."), range);
    ImGui::PopTextWrapPos();
    if (!changed)
        return;
    hl::Config onDisk;
    onDisk.set_defaults();
    onDisk.load();
    onDisk.wheel_rotation = cfg->wheel_rotation = rotation;
    onDisk.wheel_range = cfg->wheel_range = range;
    onDisk.save();
    hl::ForceFeedback::wheel_changed();
}

// [Input] SHIFT_NEUTRAL_MS, as sm2-emu's gear shifter: saved at once to
// hikaruloader.ini, alone, as the wheel's
void drawShifter()
{
    hl::Config *cfg = hl::Config::get();
    bool changed = false;
    int ms = cfg->shift_neutral_ms;
    if (ImGui::SliderInt(tr("Neutral between gears"), &ms, 0, 500, "%d ms"))
    {
        ms = std::clamp(((ms + 5) / 10) * 10, 0, 500);
        changed = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("A shift from one gear to another, by Shift up and\n"
                                   "Shift down or an H shifter's buttons, passes through\n"
                                   "neutral this long, as a real lever does."));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr("The gear levers held in their positions (NASCAR Arcade, F355, 18 Wheeler, Crazy Taxi, The King "
                                 "of Route 66...): Shift up and Shift down engage the next gear at each press; an H shifter's "
                                 "buttons (Reverse, Neutral, Gear 1 to 6, in the Driving section of a device's bindings) engage "
                                 "theirs when pressed, kept once let go. Neutral only by its button."));
    ImGui::PopTextWrapPos();
    if (!changed)
        return;
    hl::Config onDisk;
    onDisk.set_defaults();
    onDisk.load();
    onDisk.shift_neutral_ms = cfg->shift_neutral_ms = ms;
    onDisk.save();
}

// [Input] TWIN_LEVERS: saved at once to hikaruloader.ini, alone
void drawLevers()
{
    hl::Config *cfg = hl::Config::get();
    int mode = (int)cfg->twin_levers;
    bool changed = ImGui::RadioButton(tr("Combined: the accelerator and the wheel"), &mode, hl::TWIN_LEVERS_COMBINED);
    changed |= ImGui::RadioButton(tr("Original: an axis per lever"), &mode, hl::TWIN_LEVERS_ORIGINAL);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr("Star Wars: Racer Arcade's two levers each give a pod engine's thrust: even, the pod goes "
                                 "straight; the side pushing harder turns it the other way. Combined, the accelerator is the "
                                 "thrust of both and the wheel the difference, the pod turning to the side steered. Original, "
                                 "as on the cabinet: Left lever and Right lever, an axis each, in the Driving section."));
    ImGui::PopTextWrapPos();
    if (!changed)
        return;
    hl::Config onDisk;
    onDisk.set_defaults();
    onDisk.load();
    onDisk.twin_levers = cfg->twin_levers = (hl::TwinLevers)mode;
    onDisk.save();
}

// [ForceFeedback] EFFECTS: the force feedback made from the telemetry, for a
// cabinet without a motor (hl::FfbEffects); saved at once to
// hikaruloader.ini, alone, and taken at the next frame
void drawFfbEffects()
{
    hl::Config *cfg = hl::Config::get();
    bool on = cfg->ffb_effects;
    if (ImGui::Checkbox(tr("Emulated force feedback (wheel and controller)"), &on))
    {
        hl::Config onDisk;
        onDisk.set_defaults();
        onDisk.load();
        onDisk.ffb_effects = cfg->ffb_effects = on;
        onDisk.save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("The cabinet has no motor: the forces and the rumble are made from what\n"
                                   "the game does (the engines, the hits, the turns, the ground). Their\n"
                                   "strengths: [ForceFeedback] in hikaruloader.ini."));
}

// [ForceFeedback] DEVICE: saved at once to hikaruloader.ini, alone, and taken
// at once
void drawFfbDevice()
{
    hl::Config *cfg = hl::Config::get();
    static std::vector<std::string> names;
    const char *shown = cfg->ffb_device[0] ? cfg->ffb_device : tr("Automatic");
    if (ImGui::BeginCombo(tr("Force feedback device"), shown))
    {
        // Each time it opens: a device plugged in since, and no asking the
        // drivers at each frame
        if (ImGui::IsWindowAppearing())
            names = hl::ForceFeedback::device_names();
        const auto choose = [cfg](const char *name) {
            hl::Config onDisk;
            onDisk.set_defaults();
            onDisk.load();
            snprintf(onDisk.ffb_device, sizeof(onDisk.ffb_device), "%s", name);
            snprintf(cfg->ffb_device, sizeof(cfg->ffb_device), "%s", name);
            onDisk.save();
            hl::ForceFeedback::device_changed();
        };
        if (ImGui::Selectable(tr("Automatic"), !cfg->ffb_device[0]))
            choose("");
        for (const std::string &name : names)
            if (ImGui::Selectable(name.c_str(), name == cfg->ffb_device))
                choose(name.c_str());
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("A wheel takes the forces, a controller its motors. Automatic: the first\n"
                                   "device with force feedback, and player 1's controller's motors."));
}

// [ForceFeedback] GAIN: every force, every game's; saved at once to
// hikaruloader.ini, alone, and felt at once
void drawFfbGain()
{
    hl::Config *cfg = hl::Config::get();
    int gain = (int)cfg->ffb_gain;
    if (ImGui::SliderInt(tr("Force feedback gain"), &gain, 0, 300, "%d%%"))
    {
        hl::Config onDisk;
        onDisk.set_defaults();
        onDisk.load();
        onDisk.ffb_gain = cfg->ffb_gain = (unsigned)std::clamp(((gain + 2) / 5) * 5, 0, 300);
        onDisk.save();
        hl::ForceFeedback::gain_changed();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("All the forces, of every game, on the wheel and on the controller's\n"
                                   "motors. Above 100 percent, the strongest stop at what the device can do."));
}

// [ForceFeedback] HUD: saved at once to hikaruloader.ini, alone
void drawFfbHudOption()
{
    hl::Config *cfg = hl::Config::get();
    bool on = cfg->ffb_hud;
    if (ImGui::Checkbox(tr("Show the effects over the game (debugging)"), &on))
    {
        hl::Config onDisk;
        onDisk.set_defaults();
        onDisk.load();
        onDisk.ffb_hud = cfg->ffb_hud = on;
        onDisk.save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("At the top left, the spring, the friction, the force, the sine and the\n"
                                   "rumble, with the values the device is told."));
}

// [Input] BACKGROUND_INPUT: saved at once to hikaruloader.ini, alone, and
// taken at once by SDL
void drawBackgroundInput()
{
    hl::Config *cfg = hl::Config::get();
    bool on = cfg->background_input;
    if (ImGui::Checkbox(tr("Read the controllers in the background"), &on))
    {
        hl::Config onDisk;
        onDisk.set_defaults();
        onDisk.load();
        onDisk.background_input = cfg->background_input = on;
        onDisk.save();
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, on ? "1" : "0");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("The joysticks, wheels and controllers go on working while another\n"
                                   "window has the focus, their hotkeys too. The keyboard only ever\n"
                                   "works with the focus."));
}

// What the game reads now: its controls, each once, and its gear
void drawGameInputs()
{
    hl::GameControls &game = hl::GameControls::get();
    std::vector<const hl::Controls::RowDef *> seen;
    if (!ImGui::BeginTable("##game", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg))
        return;
    ImGui::TableSetupColumn("##input", ImGuiTableColumnFlags_WidthStretch, 0.4f);
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
    for (const auto &p : game.places())
    {
        if (std::find(seen.begin(), seen.end(), p.row) != seen.end())
            continue;
        seen.push_back(p.row);
        ImGui::PushID(p.row->key);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(p.label.empty() ? actionLabel(p.row->key) : p.label.c_str());
        ImGui::TableSetColumnIndex(1);
        // A channel: what the board sends, its top byte as the games' INPUT
        // TESTs show it (80H a wheel's middle), the combined levers too
        const bool channel = p.port.rfind("an", 0) == 0 ||
                             (p.port.size() == 2 && p.port[0] == 'a' && isdigit((unsigned char)p.port[1]));
        uint16_t raw = 0;
        if (channel && p.row->kind != hl::Controls::ROW_SWITCH && game.analog(p.port.c_str(), p.index, &raw))
        {
            char text[16];
            snprintf(text, sizeof text, "%02XH", raw >> 8);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(text);
            ImGui::SameLine(ImGui::CalcTextSize("00H").x + ImGui::GetStyle().ItemSpacing.x * 2);
            // At rest at its range's start: from it to its end
            double t = p.hi == p.lo ? 0.0 : ((double)raw - p.lo) / ((double)p.hi - p.lo);
            if (p.row->kind == hl::Controls::ROW_PEDAL)
                ImGui::ProgressBar((float)t, ImVec2(-FLT_MIN, 0), "");
            else
                centreBar("##axis", t * 2.0 - 1.0, 0.0);
        }
        else
        {
            double v = game.value(p.row->key);
            switch (p.row->kind)
            {
                case hl::Controls::ROW_AXIS:
                case hl::Controls::ROW_LEVER:
                    centreBar("##axis", v, 0.0);
                    break;
                case hl::Controls::ROW_PEDAL:
                {
                    char text[16];
                    snprintf(text, sizeof text, "%d%%", (int)(v * 100.0 + 0.5));
                    ImGui::ProgressBar((float)v, ImVec2(-FLT_MIN, 0), text);
                    break;
                }
                default:
                {
                    // A switch: ON or OFF, as the INPUT TESTs say it
                    const bool on = game.pressed(p.row->key);
                    pressDot(on);
                    ImGui::SameLine();
                    ImGui::TextUnformatted(on ? tr("ON") : tr("OFF"));
                    break;
                }
            }
        }
        ImGui::PopID();
    }
    // The gear lever's gear, as the game last read it
    if (!game.gears().empty())
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(tr("Gear"));
        ImGui::TableSetColumnIndex(1);
        int gear = hl::Gearbox::get().engaged();
        if (gear > 0)
            ImGui::Text("%d", gear);
        else
            ImGui::TextUnformatted(gear < 0 ? tr("R") : tr("N"));
    }
    ImGui::EndTable();
}
} // namespace

bool controlsCaptureEvent(const SDL_Event *event)
{
    hl::Controls &controls = hl::Controls::get();
    if (!controls.is_capturing())
        return false;
    // The capture takes the devices' events: they go nowhere else
    controls.capture_event(event);
    switch (event->type)
    {
        case SDL_KEYDOWN:
            if (event->key.keysym.sym == SDLK_ESCAPE)
                controls.capture_cancel();
            return true;
        case SDL_KEYUP:
        case SDL_TEXTINPUT:
        case SDL_JOYBUTTONDOWN:
        case SDL_JOYBUTTONUP:
        case SDL_JOYHATMOTION:
        case SDL_JOYAXISMOTION:
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
        case SDL_CONTROLLERAXISMOTION:
            return true;
        default:
            return false;
    }
}

void drawControlsTab()
{
    hl::Controls &controls = hl::Controls::get();
    if (!controls.is_enabled())
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(tr("The agent drives the game: the keyboard and the controllers are not read."));
        ImGui::PopTextWrapPos();
    }

    header(tr("Devices"));
    std::vector<hl::Controls::Device> devices = controls.devices();
    if (ImGui::BeginTable("##devices", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch, 0.6f);
        ImGui::TableSetupColumn(tr("In controls.ini"), ImGuiTableColumnFlags_WidthStretch, 0.4f);
        ImGui::TableSetupColumn("##bind", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < devices.size(); i++)
        {
            const hl::Controls::Device &d = devices[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(deviceName(d));
            if (!d.profile.empty())
            {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", tr("(profile)"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", d.profile_file.c_str());
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            if (d.player < 0)
                ImGui::TextDisabled("KEY_");
            else if (d.game_controller)
                ImGui::TextDisabled("GC%d_, JOY%d_", d.player, d.player);
            else
                ImGui::TextDisabled(d.player >= hl::Controls::MOUSE_PLAYER ? "MOUSE%d_" : "JOY%d_",
                                    d.player >= hl::Controls::MOUSE_PLAYER ? d.player - hl::Controls::MOUSE_PLAYER : d.player);
            ImGui::TableSetColumnIndex(2);
            if (ImGui::Button(tr("Bind")))
            {
                g_mapping.device = d;
                g_mapping.requestOpen = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (devices.size() < 2)
        ImGui::TextDisabled("%s", tr("No controller connected."));
    // The numbers shown, not those of controls.ini: saved at once to
    // hikaruloader.ini, alone (the window's other changes wait for Save)
    bool fromOne = hl::Config::get()->buttons_from_one;
    if (ImGui::Checkbox(tr("Number the buttons from 1 (as Windows and vJoy do)"), &fromOne))
    {
        hl::Config onDisk;
        onDisk.set_defaults();
        onDisk.load();
        onDisk.buttons_from_one = hl::Config::get()->buttons_from_one = fromOne;
        onDisk.save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("controls.ini keeps SDL's numbers, which start at 0: \"Button 1\" is JOY0_BUTTON_0 there."));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled(tr("The bindings are in %s, apart from the emulator's settings."), controls.path());
    ImGui::PopTextWrapPos();
    // A game's own bindings: controls/<set>.ini, read instead for it alone
    if (controls.game()[0])
    {
        if (controls.game_file_in_use())
        {
            ImGui::Text(tr("%s has bindings of its own."), controls.game());
            ImGui::SameLine();
            if (ImGui::Button(tr("Use the common bindings")))
                controls.use_common();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("Its file is removed; the common controls.ini is used for it again."));
        }
        else
        {
            char label[160];
            snprintf(label, sizeof(label), tr("Save these bindings for %s alone"), controls.game());
            if (ImGui::Button(label))
                controls.save_for_game();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tr("They go to a file of the game's own, read instead of controls.ini when it starts; the changes made from then on go there."));
        }
    }

    header(tr("What the game receives"));
    drawGameInputs();

    header(tr("Steering wheel"));
    drawWheel();

    header(tr("Gear lever"));
    drawShifter();

    // A pair of levers TWIN_LEVERS combines: shown with no game, or a
    // game that has one (games.json "combined")
    if (!hl::GameControls::get().game()[0] || hl::GameControls::get().combined())
    {
        header(tr("Levers (Star Wars: Racer Arcade)"));
        drawLevers();
    }

    header(tr("Force feedback"));
    drawFfbDevice();
    drawFfbGain();
    drawFfbEffects();
    drawFfbHudOption();

    header(tr("Dead zones and options"));
    drawOptions();
    drawBackgroundInput();

    mappingPage();
}
} // namespace ui
