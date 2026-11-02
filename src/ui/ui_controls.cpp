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
// page of its own; what the game receives; the dead zones.

#include "ui/ui_common.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <imgui.h>

#include "hl/config.h"
#include "hl/controls.h"
#include "hl/ffb.h"
#include "hl/input.h"

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
    {"Digital", N_("Stick and buttons")},
    {"Driving", N_("Driving")},
    {"Emulator", N_("Emulator")},
};

const char *actionLabel(const std::string &key)
{
    static const struct
    {
        const char *key, *label;
    } labels[] = {
        {"Test", N_("Test button")},
        {"P1_Service", N_("Service button")},
        {"P1_Coin", N_("Coin 1")},
        {"P2_Coin", N_("Coin 2")},
        {"P1_Start", N_("Start")},
        {"ExitGame", N_("Quit")},
        {"P1_Up", N_("Up")},
        {"P1_Down", N_("Down")},
        {"P1_Left", N_("Left")},
        {"P1_Right", N_("Right")},
        {"P1_Button1", N_("Button 1")},
        {"P1_Button2", N_("Button 2")},
        {"P1_Button3", N_("Button 3")},
        {"P1_Button4", N_("Button 4")},
        {"P1_Card1Insert", N_("Card: insert or take back")},
        {"P1_Steer", N_("Steering wheel")},
        {"P1_Gas", N_("Accelerator")},
        {"P1_Brake", N_("Brake")},
        {"P1_Steer_Left", N_("Steer left (all the way)")},
        {"P1_Steer_Right", N_("Steer right (all the way)")},
        {"P1_Gas_Digital", N_("Accelerator (all the way)")},
        {"P1_Brake_Digital", N_("Brake (all the way)")},
        {"Pause", N_("Pause")},
        {"SaveState", N_("Save the state")},
        {"LoadState", N_("Load the state")},
        {"Menu", N_("Settings (this window)")},
        {"Back", N_("Back to the picker, or quit")},
        {"ShowFps", N_("Show the frame rate")},
        {"Steer", N_("Steering wheel")},
        {"Gas", N_("Accelerator")},
        {"Brake", N_("Brake")},
    };
    for (const auto &l : labels)
        if (!strcasecmp(key.c_str(), l.key))
            return tr(l.label);
    return key.c_str();
}

bool isSteering(const std::string &key)
{
    return !strcasecmp(key.c_str(), "P1_Steer");
}

bool isPedal(const std::string &key)
{
    return !strcasecmp(key.c_str(), "P1_Gas") || !strcasecmp(key.c_str(), "P1_Brake");
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

// [Config] <Action>_DeadZone of a row, P1_Steer's being Steer_DeadZone
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

void bindingRow(const char *section, const hl::Controls::Row &row)
{
    hl::Controls &controls = hl::Controls::get();
    const int player = g_mapping.device.player;
    ImGui::PushID(row.key.c_str());
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(actionLabel(row.key));
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
        controls.capture(player, isPedal(row.key) ? hl::Controls::CAPTURE_PEDAL
                                 : isSteering(row.key) ? hl::Controls::CAPTURE_STEERING
                                                       : hl::Controls::CAPTURE_BUTTON);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(mine.empty());
    if (ImGui::Button(tr("Unbind")))
        controls.set_value(section, row.key.c_str(), replaceDeviceTokens(g_mapping.device, row.value, {}));
    ImGui::EndDisabled();
    ImGui::SameLine();
    // The device's inputs back to the default ones, the others' kept
    std::vector<std::string> def = deviceTokens(g_mapping.device, controls.default_value(section, row.key.c_str()), true);
    ImGui::BeginDisabled(def == mine);
    if (ImGui::Button(tr("Default")))
        controls.set_value(section, row.key.c_str(), replaceDeviceTokens(g_mapping.device, row.value, def));
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(tr("Default: %s"), def.empty() ? "-" : describeAll(def).c_str());
    ImGui::PopID();
}

// The device's inputs back to the defaults in every row, the others' kept
void resetDevice()
{
    hl::Controls &controls = hl::Controls::get();
    for (const Section &s : kSections)
        for (const auto &row : controls.rows(s.name))
        {
            std::vector<std::string> def = deviceTokens(g_mapping.device, controls.default_value(s.name, row.key.c_str()), true);
            if (def != deviceTokens(g_mapping.device, row.value, true))
                controls.set_value(s.name, row.key.c_str(), replaceDeviceTokens(g_mapping.device, row.value, def));
        }
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
        if (isSteering(g_capture.key))
            ImGui::TextUnformatted(tr("Turn to the left."));
        else if (isPedal(g_capture.key))
            ImGui::TextUnformatted(tr("Press the pedal."));
        else if (g_mapping.device.game_controller)
            ImGui::TextUnformatted(tr("Hold Guide and press a button for a hotkey: the game does not see it."));
        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr("Escape: cancel"));
        if (ImGui::Button(tr("Cancel")))
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
    if (!ImGui::BeginPopupModal(id.c_str(), NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
        return;

    float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    if (ImGui::Button(tr("Close")))
    {
        controls.capture_cancel();
        ImGui::CloseCurrentPopup();
    }
    // On the right, away from Close: the device's bindings back to the defaults
    const char *resetLabel = tr("Reset...");
    ImGui::SameLine(right - ImGui::CalcTextSize(resetLabel).x - ImGui::GetStyle().FramePadding.x * 2);
    if (ImGui::Button(resetLabel))
        ImGui::OpenPopup("##reset");
    if (ImGui::BeginPopupModal("##reset", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
    {
        ImGui::TextUnformatted(tr("Put this device's bindings back to the defaults?"));
        if (ImGui::Button(tr("Yes")))
        {
            resetDevice();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("No")))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginChild("##bindings", ImVec2(0, 0), ImGuiChildFlags_Borders))
        for (const Section &s : kSections)
        {
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

// What the game reads now: the switches, the wheel and the pedals
void drawGameInputs()
{
    static const hl::Action switches[] = {
        hl::ACTION_TEST,    hl::ACTION_SERVICE, hl::ACTION_COIN1,   hl::ACTION_COIN2,   hl::ACTION_START1,
        hl::ACTION_UP,      hl::ACTION_DOWN,    hl::ACTION_LEFT,    hl::ACTION_RIGHT,   hl::ACTION_BUTTON1,
        hl::ACTION_BUTTON2, hl::ACTION_BUTTON3, hl::ACTION_BUTTON4, hl::ACTION_CARD,
    };
    hl::Controls &controls = hl::Controls::get();
    if (!ImGui::BeginTable("##game", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg))
        return;
    ImGui::TableSetupColumn("##input", ImGuiTableColumnFlags_WidthStretch, 0.4f);
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
    for (hl::Action a : switches)
    {
        const char *section, *key;
        if (!hl::Input::row_of(a, &section, &key))
            continue;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(actionLabel(key));
        ImGui::TableSetColumnIndex(1);
        pressDot(controls.pressed(section, key));
    }
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Steering wheel"));
    ImGui::TableSetColumnIndex(1);
    centreBar("##steer", controls.axis("Driving", "P1_Steer"), 0.0);
    static const char *pedals[2] = {"P1_Gas", "P1_Brake"};
    for (const char *p : pedals)
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(actionLabel(p));
        ImGui::TableSetColumnIndex(1);
        float v = (float)controls.pedal("Driving", p);
        char text[16];
        snprintf(text, sizeof text, "%d%%", (int)(v * 100.0f + 0.5f));
        ImGui::ProgressBar(v, ImVec2(-FLT_MIN, 0), text);
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
            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            if (d.player < 0)
                ImGui::TextDisabled("KEY_");
            else if (d.game_controller)
                ImGui::TextDisabled("GC%d_, JOY%d_", d.player, d.player);
            else
                ImGui::TextDisabled("JOY%d_", d.player);
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

    header(tr("What the game receives"));
    drawGameInputs();

    header(tr("Steering wheel"));
    drawWheel();

    header(tr("Dead zones and options"));
    drawOptions();

    mappingPage();
}
} // namespace ui
