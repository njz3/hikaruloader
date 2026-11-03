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

// Shared pieces of the game picker and of the Tab settings overlay.
//
// The interface follows linuxloader's, itself a modified work based on
// sm2-emu's (https://github.com/dmanlfc/sm2-emu), a Sega Model 2 emulator by
// Daniel Martin (dmanlfc) and the sm2-emu contributors, GPL-3.0-or-later: its
// dark style, its scaling with the window, its picker with a description pane
// and its tabbed settings. This is a modified work based on that interface,
// adapted to Hikaruloader; the About tab says so.

#include "ui/ui_common.h"
#include "ui/ui_folder.h"
#include "ui/ui_po.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include "imgui_impl_sdl2.h"
#include "lua.h"

#include "hl/audio.h"
#include "hl/config.h"
#include "hl/crt_preset.h"
#include "hl/outputs.h"
#include "hl/renderer.h"
#include "hl/romset.h"
#include "hl/window.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace ui
{
namespace
{
bool g_initialised = false;
float g_uiScale = 0.0f;
ImGuiStyle g_baseStyle;
std::string g_status; // the result of the last save, shown beside the buttons
bool g_rescan = false;
bool g_rescanTyped = false; // the folder typed in, its field not left yet

// The languages: English, the texts' own, and the gettext catalogs data/lang/<code>.po beside the program,
// each a translation of the English texts (data/lang/hikaruloader.pot; see README.md). All are loaded once,
// at the start: a language chosen later is a pointer moved, the texts tr () gave out stay valid.
struct Language
{
    std::string code; // "fr", from the file's name
    std::string name; // "Français", as the language calls itself (X-Language-Name)
    PoCatalog texts;  // English -> this language
};
std::vector<Language> g_languages;
const Language *g_language = nullptr; // nullptr: English

// The directories the language files may be in: beside the program, then the working directory's data/
std::vector<std::string> languageDirs()
{
    std::vector<std::string> dirs;
    if (char *base = SDL_GetBasePath())
    {
        dirs.push_back(std::string(base) + "data/lang");
        SDL_free(base);
    }
    dirs.push_back("data/lang");
    return dirs;
}

void loadLanguages()
{
    static bool loaded = false;
    if (loaded)
        return;
    loaded = true;
    for (const std::string &dir : languageDirs())
    {
        std::error_code ec;
        for (const auto &file : std::filesystem::directory_iterator(dir, ec))
        {
            if (file.path().extension() != ".po")
                continue;
            const std::string code = file.path().stem().string();
            if (std::any_of(g_languages.begin(), g_languages.end(), [&](const Language &l) { return l.code == code; }))
                continue;
            Language language;
            language.code = code;
            if (!language.texts.load(file.path().string().c_str()))
                HL_ERROR("language file %s: a line not understood", file.path().string().c_str());
            language.name = language.texts.header("X-Language-Name");
            if (language.name.empty())
                language.name = code;
            g_languages.push_back(std::move(language));
        }
    }
    std::sort(g_languages.begin(), g_languages.end(), [](const Language &a, const Language &b) { return a.name < b.name; });
}

const Language *findLanguage(const char *code)
{
    for (const Language &language : g_languages)
        if (code && language.code == code)
            return &language;
    return nullptr;
}

// The system's language, two letters ("fr", "de"...)
std::string systemLanguage()
{
#ifdef _WIN32
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = L"";
    if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) > 2)
        return std::string{(char)locale[0], (char)locale[1]};
    return "en";
#else
    const char *lang = getenv("LANG");
    return lang && strlen(lang) >= 2 ? std::string(lang, 2) : "en";
#endif
}

void applyScale()
{
    // Scale with the window as sm2-emu does: against a 992x768 base, the smaller ratio winning, never under 80 % of
    // the font's own size; the settings then scroll when they do not fit.
    const ImGuiIO &io = ImGui::GetIO();
    const float sx = io.DisplaySize.x > 0.0f ? io.DisplaySize.x / 992.0f : 1.0f;
    const float sy = io.DisplaySize.y > 0.0f ? io.DisplaySize.y / 768.0f : 1.0f;
    const float fb = io.DisplayFramebufferScale.y > 0.0f ? io.DisplayFramebufferScale.y : 1.0f;
    float scale = std::min(sx, sy);
    scale = std::clamp(scale, std::clamp(0.8f / fb, 0.5f, 0.8f), 4.0f);
    if (scale != g_uiScale)
    {
        g_uiScale = scale;
        ImGuiStyle &style = ImGui::GetStyle();
        style = g_baseStyle;
        style.ScaleAllSizes(scale);
        style.FontScaleMain = scale;
    }
}

const char *windowSizeLabel(int scale)
{
    static char labels[4][32];
    snprintf(labels[scale - 1], sizeof(labels[0]), "%d x %d (x%d)", 640 * scale, 480 * scale, scale);
    return labels[scale - 1];
}

void restartNote(bool inGame)
{
    if (inGame)
        ImGui::TextDisabled("%s", tr("Takes effect at the next start."));
}

void wrappedNote(const char *text);

void drawDisplayTab()
{
    hl::Config *cfg = hl::Config::get ();
    bool changed = false;

    // The renderers to come are listed, not selectable yet
    const char *apiLabels[] = {"OpenGL 3.1", tr("Vulkan 1.3 (to come)"), tr("Direct3D 11 (to come)")};
    if (ImGui::BeginCombo(tr("Graphics API"), apiLabels[std::clamp(cfg->graphics_api, 0, IM_ARRAYSIZE(apiLabels) - 1)]))
    {
        for (int api = 0; api < IM_ARRAYSIZE(apiLabels); api++)
        {
            ImGui::BeginDisabled(api != hl::GRAPHICS_OPENGL);
            if (ImGui::Selectable(apiLabels[api], api == cfg->graphics_api))
                cfg->graphics_api = api;
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }

    // RENDER_SCALE: the 3D's internal resolution, as Flycast and sm2-emu list it
    {
        const int width = cfg->widescreen ? 854 : 640;
        auto resolution = [&](int scale) {
            static char text[64];
            snprintf(text, sizeof(text), scale == 1 ? tr("%d x %d (native)") : "%d x %d (x%d)", width * scale, 480 * scale, scale);
            return (const char *)text;
        };
        std::string current = resolution(cfg->render_scale);
        if (ImGui::BeginCombo(tr("Internal resolution"), current.c_str()))
        {
            for (int scale = 1; scale <= hl::MAX_RENDER_SCALE; scale++)
                if (ImGui::Selectable(resolution(scale), scale == cfg->render_scale))
                    cfg->render_scale = scale;
            ImGui::EndCombo();
        }
        wrappedNote(tr("The 3D drawn larger, then fitted to the aspect ratio and the window: sharper edges and textures, more work for "
                       "the graphics card. The 2D (texts, logos) keeps its size. Above the window's size, the picture is averaged down "
                       "(supersampling)."));
    }

    // ASPECT_RATIO and its companions, as linuxloader's (hl/aspect.h): read at each frame
    ImGui::Separator();
    const char *aspectLabels[] = {tr("Original"),        tr("4:3, black bars"), tr("16:9, black bars"), tr("4:3, stretched"),
                                  tr("16:9, stretched"), tr("4:3, cropped"),    tr("16:9, cropped")};
    ImGui::Combo(tr("Aspect ratio"), &cfg->aspect, aspectLabels, IM_ARRAYSIZE(aspectLabels));
    if (cfg->aspect == hl::ASPECT_4_3_CROP || cfg->aspect == hl::ASPECT_16_9_CROP)
    {
        ImGui::SliderInt(tr("Crop position"), &cfg->crop_position, -100, 100);
        ImGui::TextDisabled("%s", tr("-100: left or top, 0: the middle, 100: right or bottom."));
        ImGui::Checkbox(tr("Bend the cut parts onto the edges"), &cfg->crop_barrel);
        if (cfg->crop_barrel)
        {
            ImGui::SliderInt(tr("Undistorted middle"), &cfg->barrel_center, 0, 100, "%d %%");
            ImGui::SliderInt(tr("Curvature radius"), &cfg->barrel_radius, 10, 300, "%d %%");
            // xgettext:no-c-format (shown as it is)
            wrappedNote(tr("The whole picture stays visible: the middle (in % of the frame) as cropped, the parts the crop would cut "
                           "squeezed onto the edges as on a cylinder seen from the front. A small radius bends hard at the edge, a large "
                           "one squeezes evenly."));
        }
    }
    ImGui::TextDisabled("%s", tr("Original: the picture's own shape, 4:3 (16:9 in widescreen)."));
    wrappedNote(tr("Black bars: the frame's shape, with the whole picture inside at its own shape and black bars where the shapes differ. "
                   "Stretched: the picture fills the frame, deformed. Cropped: the picture fills the frame at its own shape, cut on the sides "
                   "(or above and below); the crop position moves the part kept. The display mode then fits the frame to the window or "
                   "the screen, or stretches it."));

    // DISPLAY_MODE, each also _STRETCH: the frame fills the window or the screen
    ImGui::Separator();
    const char *modeLabels[] = {tr("Window, square pixels"),        tr("Window, stretched"),
                                tr("Full screen"),                  tr("Full screen, stretched"),
                                tr("Borderless fullscreen window"), tr("Borderless fullscreen window, stretched")};
    int mode = cfg->display_mode * 2 + (cfg->display_stretch ? 1 : 0);
    if (ImGui::Combo(tr("Display mode"), &mode, modeLabels, IM_ARRAYSIZE(modeLabels)))
    {
        cfg->display_mode = mode / 2;
        cfg->display_stretch = mode % 2;
        changed = true;
    }
    wrappedNote(tr("On a CRT driven at a super resolution (1920x240): its real shape as the aspect ratio (4:3 or 16:9, black bars) "
                   "and a stretched mode. The frame then covers the screen, whatever the shape of its pixels."));

    ImGui::BeginDisabled(cfg->display_mode != hl::DISPLAY_WINDOW);
    if (ImGui::BeginCombo(tr("Window size"), windowSizeLabel(cfg->window_scale)))
    {
        for (int scale = 1; scale <= 4; scale++)
            if (ImGui::Selectable(windowSizeLabel(scale), scale == cfg->window_scale))
            {
                cfg->window_scale = scale;
                changed = true;
            }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("%s", tr("The window can also be resized with the mouse."));

    // INTEGER_SCALE, as Flycast's "Integer Scaling"
    ImGui::Checkbox(tr("Whole multiples only (integer scaling)"), &cfg->integer_scale);
    wrappedNote(tr("The picture at 1x, 2x, 3x... its size only, for even pixels, with larger black bars. Not with a stretched or cropped "
                   "picture, nor a stretched display mode."));

    // SCALING_FILTER, as linuxloader lists it: read at each frame
    const char *filterLabels[] = {tr("Smooth"), tr("Sharp pixels")};
    int filter = cfg->scale_filter == hl::FILTER_LINEAR ? 0 : 1;
    if (ImGui::Combo(tr("Scaling filter"), &filter, filterLabels, IM_ARRAYSIZE(filterLabels)))
        cfg->scale_filter = filter == 0 ? hl::FILTER_LINEAR : hl::FILTER_NEAREST;
    wrappedNote(tr("Sharp pixels: square and crisp, but uneven unless the picture is enlarged a whole number of times. Smooth: down to "
                   "240 rows (a CRT), each one the mean of two, where Sharp pixels drops one in two (thin lines, small texts)."));

    // BEZEL: what is around the picture where the window is wider than its frame (hl/bezel.h): read at each frame
    const char *bezelLabels[] = {tr("Off (black)"), tr("The picture's edges, blurred"), tr("The game's bezel picture")};
    ImGui::Combo(tr("Bezel"), &cfg->bezel, bezelLabels, IM_ARRAYSIZE(bezelLabels));
    wrappedNote(tr("Around the picture where the window is wider than its frame: black, the picture's own edges blurred and dimmed, "
                   "or the game's bezel picture with the game in its screen. The pictures are Batocera's: artwork\\bezels\\<set>.png "
                   "beside the program and its <set>.info (where the screen is; else the picture's see-through middle), a clone "
                   "taking its parent's, else default.png; black without any."));

    ImGui::Separator();
    changed |= ImGui::Checkbox(tr("Vertical sync"), &cfg->vsync);
    ImGui::Checkbox(tr("Limit the frame rate"), &cfg->fps_limiter);
    ImGui::BeginDisabled(!cfg->fps_limiter);
    ImGui::SliderFloat(tr("Target frame rate"), &cfg->fps_target, 30.0f, 120.0f, "%.1f");
    ImGui::EndDisabled();
    ImGui::Checkbox(tr("Show the frame rate (F12)"), &cfg->fps_overlay);
    ImGui::Checkbox(tr("Show the frame number in the title bar"), &cfg->title_frame);
    ImGui::Checkbox(tr("Hide the mouse pointer"), &cfg->hide_cursor);

    // Taken at the game's start (hikaru::Renderer)
    ImGui::Separator();
    ImGui::Checkbox(tr("Draw on a thread of its own"), &cfg->threaded_rendering);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr("Each frame drawn while the next one is emulated: faster when the drawing slows the game, "
                                 "but the picture comes a frame later. Takes effect at the game's next start."));
    ImGui::PopTextWrapPos();

    ImGui::Checkbox(tr("Sort the translucent polygons at each pixel"), &cfg->sort_translucent_pixels);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr("Blended from the farthest at each pixel, as the board does: right where they cross. "
                                 "Needs OpenGL 4.3 and no MSAA; off, each mesh is sorted as a whole, lighter for a weak "
                                 "graphics card."));
    ImGui::PopTextWrapPos();

    ImGui::Checkbox(tr("Widescreen 3D (16:9)"), &cfg->widescreen);
    ImGui::BeginDisabled(!cfg->widescreen);
    const char *layerLabels[] = {tr("4:3, in the middle"), tr("Stretched to 16:9")};
    ImGui::Combo(tr("2D in widescreen"), &cfg->widescreen_2d, layerLabels, IM_ARRAYSIZE(layerLabels));
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr("The 3D seen wider, at 16:9, its middle as the game draws it; the 2D (texts, logos) at 4:3 in the middle, "
                                 "or stretched for a game that needs it. The game may leave objects out at the sides, made for 4:3. "
                                 "Takes effect at the game's next start."));
    ImGui::PopTextWrapPos();

    ImGui::SliderFloat(tr("Draw distance"), &cfg->draw_distance, 1.0f, 4.0f, "x%.1f");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr("The 3D scenery's detailed models and its fog that much farther; what the game itself leaves out "
                                 "is not drawn. x1.0: as the game draws it."));
    ImGui::PopTextWrapPos();

    if (changed)
        hl::Window::apply_display ();
}

// A setting among a few values, shown by their labels
void comboValue(const char *label, int *value, const int *values, const char *const *labels, int count)
{
    int index = 0;
    for (int i = 0; i < count; i++)
        if (values[i] == *value)
            index = i;
    if (ImGui::BeginCombo(label, labels[index]))
    {
        for (int i = 0; i < count; i++)
            if (ImGui::Selectable(labels[i], i == index))
                *value = values[i];
        ImGui::EndCombo();
    }
}

void wrappedNote(const char *text)
{
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
}

// [Enhancements]: beyond the board's picture, taken at once (hikaru::Renderer's draw ())
void drawEnhancementsTab()
{
    hl::Config *cfg = hl::Config::get ();

    // The internal resolution (RENDER_SCALE) is the Display tab's, as in the other emulators
    const int samples[] = {0, 2, 4, 8};
    const char *sampleLabels[] = {tr("Off"), "x2", "x4", "x8"};
    comboValue(tr("Antialiasing (MSAA)"), &cfg->msaa, samples, sampleLabels, IM_ARRAYSIZE(samples));
    wrappedNote(tr("The 3D's edges smoothed by several samples a pixel. The textures' own edges (fences, leaves) stay as they are."));

    ImGui::Checkbox(tr("Edge smoothing (FXAA)"), &cfg->fxaa);
    wrappedNote(tr("The 3D's edges smoothed once drawn: cheaper than MSAA, a little blurrier; the 2D left alone."));

    ImGui::Separator();
    const int levels[] = {1, 2, 4, 8, 16};
    const char *levelLabels[] = {tr("Off"), "x2", "x4", "x8", "x16"};
    comboValue(tr("Anisotropic filtering"), &cfg->anisotropy, levels, levelLabels, IM_ARRAYSIZE(levels));
    wrappedNote(tr("The textures seen at a slant (roads, floors, walls) kept sharp into the distance."));

    const int factors[] = {1, 2, 4};
    const char *factorLabels[] = {tr("Off"), "x2 (Scale2x)", "x4 (Scale4x)"};
    comboValue(tr("Texture upscaling"), &cfg->texture_scale, factors, factorLabels, IM_ARRAYSIZE(factors));
    wrappedNote(tr("The textures enlarged, the edges of their flat colours (texts, signs, markings) kept sharp; the photographic ones change little. "
                   "More memory, and the textures take longer to load."));

    ImGui::Separator();
    ImGui::SliderInt(tr("Bloom"), &cfg->bloom, 0, 100, "%d %%");
    wrappedNote(tr("A glow around the 3D's bright parts (lights, sky, explosions). 0: none."));

    // CRT_PRESET: a libretro preset's passes in CRT_FILTER's place (hl/crt_preset.h), the folder read when the list opens
    static std::vector<std::string> presets = hl::crt_preset::list();
    if (ImGui::BeginCombo(tr("CRT preset"), cfg->crt_preset[0] ? cfg->crt_preset : tr("None")))
    {
        if (ImGui::IsWindowAppearing())
            presets = hl::crt_preset::list();
        if (ImGui::Selectable(tr("None"), !cfg->crt_preset[0]))
            cfg->crt_preset[0] = '\0';
        for (const std::string &name : presets)
            if (ImGui::Selectable(name.c_str(), name == cfg->crt_preset))
                snprintf(cfg->crt_preset, sizeof(cfg->crt_preset), "%s", name.c_str());
        ImGui::EndCombo();
    }
    wrappedNote(tr("A CRT drawn over the whole picture by a shader preset (data\\crt beside the program): Lola Retro's, by Zoki. "
                   "sega-monitor-29 and sega-projector-50: the cabinets' screens; pc-monitor-vga: PC monitors, from good to extreme "
                   "(-flat: a flat tube). The tube stays 4:3; a crop's bent edges are not drawn. Needs OpenGL 3.3."));

    // The preset draws its own scanlines and mask: ours over them would beat with them
    ImGui::BeginDisabled(cfg->crt_preset[0] != '\0');
    ImGui::SliderInt(tr("CRT filter"), &cfg->crt_filter, 0, 100, "%d %%");
    ImGui::EndDisabled();
    if (cfg->crt_preset[0])
        wrappedNote(tr("Not with a CRT preset, which draws its own scanlines and mask."));
    else
        wrappedNote(tr("A CRT's look on a flat screen: its scanlines and its phosphors' stripes, over the whole picture. "
                       "From 960 rows up (a window of x2 or more, full screen); none on a CRT itself. 0: none."));
}

void drawSoundTab(bool inGame)
{
    hl::Config *cfg = hl::Config::get ();

    // The PC's speakers, by their channels (hl::Audio::Output)
    static const struct
    {
        int output;
        const char *label;
    } outputs[] = {
        {hl::Audio::OUTPUT_AUTO, N_("The sound card's own")},
        {hl::Audio::OUTPUT_MONO, N_("Mono")},
        {hl::Audio::OUTPUT_STEREO, N_("Stereo")},
        {hl::Audio::OUTPUT_2_1, N_("2.1 (stereo and subwoofer)")},
        {hl::Audio::OUTPUT_QUAD, N_("Quad (front and rear)")},
        {hl::Audio::OUTPUT_5_1, "5.1"},
        {hl::Audio::OUTPUT_7_1, "7.1"},
    };
    const char *current = outputs[0].label;
    for (const auto &o : outputs)
        if (o.output == cfg->sound_output)
            current = o.label;
    if (ImGui::BeginCombo(tr("Speakers"), tr(current)))
    {
        for (const auto &o : outputs)
            if (ImGui::Selectable(tr(o.label), o.output == cfg->sound_output))
                cfg->sound_output = o.output;
        ImGui::EndCombo();
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr("The game's sound is its cabinet's: the front speakers', and the rear's when it has some. The rear goes to "
                                 "the rear speakers (quad, 5.1, 7.1), or into the front without; the subwoofer gets all of the sound, "
                                 "low-passed; the center speaker none. The sound card's own: the speakers Windows has set for it."));
    ImGui::PopTextWrapPos();

    ImGui::Separator();
    ImGui::SliderInt(tr("Front / rear balance"), &cfg->sound_balance, 0, 100, "%d");
    // As hl::Audio fades them: each side from the middle
    const int front = cfg->sound_balance <= 50 ? 100 : (100 - cfg->sound_balance) * 2;
    const int rear = cfg->sound_balance >= 50 ? 100 : cfg->sound_balance * 2;
    ImGui::TextDisabled(tr("Front at %d %%, rear at %d %%; 50 for both at their full level."), front, rear);
    ImGui::SliderInt(tr("Volume"), &cfg->sound_volume, 0, 100, "%d %%");
    ImGui::TextDisabled("%s", tr("The changes take effect as soon as the game goes on."));

    // The BIOS's ADVERTISE SOUND, from -1: the EEPROM's own
    ImGui::Separator();
    const char *attractLabels[] = {tr("The board's setting"), tr("Off"), tr("On")};
    int attract = cfg->attract_sound + 1;
    if (ImGui::Combo(tr("Attract sound"), &attract, attractLabels, IM_ARRAYSIZE(attractLabels)))
        cfg->attract_sound = attract - 1;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr("The sound of the game's attract, while nobody plays. Written to the game's EEPROM at its start; "
                                 "the board's setting: as its BIOS menu set it (ADVERTISE SOUND, off on a new board)."));
    ImGui::PopTextWrapPos();
    restartNote(inGame);

    // The output's queue (hl::Audio::latency)
    ImGui::Separator();
    ImGui::SliderInt(tr("Latency"), &cfg->sound_latency, 20, 200, "%d ms");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr("The sound kept ahead in the output: less, the sound closer to the picture, but gaps when a frame "
                                 "comes late; more, safer on a slow PC. The output opens again with it."));
    ImGui::PopTextWrapPos();

    // The sound chips' emulation (hikaru::Aica, SOUND_CORE)
    const char *soundCoreLabels[] = {tr("Optimized"), tr("Interpreted")};
    int soundCore = cfg->sound_optimized ? 0 : 1;
    if (ImGui::Combo(tr("Sound emulation"), &soundCore, soundCoreLabels, IM_ARRAYSIZE(soundCoreLabels)))
        cfg->sound_optimized = soundCore == 0;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr("Optimized keeps the sound chips' channel registers and effects program decoded until the game "
                                 "writes them; interpreted decodes them at each sample, slower, as the reference. The same sound "
                                 "either way; the change takes effect at once."));
    ImGui::PopTextWrapPos();
}

void drawEmulationTab(bool inGame)
{
    hl::Config *cfg = hl::Config::get ();

    // The board's country, from -1: the EEPROM's own
    const char *regionLabels[] = {tr("The board's own"), tr("Japan"), tr("USA"), tr("Export"), tr("Australia"), tr("Korea")};
    int region = cfg->region + 1;
    if (ImGui::Combo(tr("Region"), &region, regionLabels, IM_ARRAYSIZE(regionLabels)))
        cfg->region = region - 1;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr("The board's country, which the games read for their language and texts (Brave Firefighters in "
                                 "English outside Japan). Written to the game's EEPROM at its start; the board's own: the "
                                 "country its BIOS menu set."));
    ImGui::PopTextWrapPos();
    restartNote(inGame);

    ImGui::Separator();
    const char *coreLabels[] = {tr("Dynamic recompiler (dynarec)"), tr("Interpreter")};
    int core = cfg->dynarec ? 0 : 1;
    if (ImGui::Combo(tr("SH-4 processors"), &core, coreLabels, IM_ARRAYSIZE(coreLabels)))
        cfg->dynarec = core == 0;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr("The dynarec runs their code translated to x86-64; the interpreter, one instruction at a time, as the "
                                 "debugger does. Both give the same machine: the change takes effect at the next frame."));
    ImGui::PopTextWrapPos();

    const char *memoryLabels[] = {tr("Regions, one after the other"), tr("Page table"), tr("Fastmem")};
    const char *fastmemTip = tr("Fastmem takes much more memory: 512 MB of address space reserved for each SH-4, their RAM and ROM "
                                "mapped there a second time. For no significant gain in emulation speed.");
    if (ImGui::BeginCombo(tr("Memory access"), memoryLabels[cfg->memory_access]))
    {
        for (int i = 0; i < hl::NUM_MEMORY_ACCESSES; i++)
        {
            if (ImGui::Selectable(memoryLabels[i], cfg->memory_access == i))
                cfg->memory_access = (hl::MemoryAccess) i;
            if (i == hl::MEMORY_FASTMEM)
                ImGui::SetItemTooltip("%s", fastmemTip);
        }
        ImGui::EndCombo();
    }
    if (cfg->memory_access == hl::MEMORY_FASTMEM)
        ImGui::SetItemTooltip("%s", fastmemTip);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr("How the CPUs find their RAM and ROM: the page table at once, the regions searched one after the other "
                                 "(the first way), fastmem at their own addresses in the PC's address space. The same machine "
                                 "either way: the change takes effect at the next frame."));
    ImGui::PopTextWrapPos();

    // The SH-4s' clock (Config::sh4_clock_mhz)
    ImGui::Separator();
    ImGui::SliderInt(tr("SH-4 clock"), (int *) &cfg->sh4_clock, 20, 200, "%d MHz");
    restartNote(inGame);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr("The SH-4s' clock: 200 MHz is the board's. Their timers, the MIE and the sound keep real time at any "
                                 "clock; the faster, the more of the PC the waits the games poll take (at 200 MHz, the games' boots "
                                 "run below 60 frames a second on this PC). 100 MHz by default; below, Air Trix's "
                                 "skater loses his body."));
    ImGui::PopTextWrapPos();

    // The low-level emulations, off by default (IO_BOARD_PROGRAM, SECURITY_CHIPS)
    ImGui::Separator();
    ImGui::Checkbox(tr("I/O board running its own program"), &cfg->io_board_program);
    restartNote(inGame);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr("The JVS I/O board's processor runs its program, as on the cabinet, instead of its simulation: "
                                 "315-6215 (MAME's jvs13551 set), epr-21868 for NASCAR Arcade (the 18wheelr set); without the "
                                 "file, the simulation. Test and Service then go through that board, as a cabinet's."));
    ImGui::PopTextWrapPos();
    ImGui::Checkbox(tr("Security chips"), &cfg->security_chips);
    restartNote(inGame);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr("The ROM board's X76F100 and the main board's 93C46 answer from their images (x76f100.ic85 and "
                                 "93c46.ic115, in MAME's newer hikaru set), the BIOS checking them instead of skipping the check; "
                                 "NASCAR Arcade's protection answers as its Actel chip."));
    ImGui::PopTextWrapPos();
}

// A port typed in, taken once the field is left: not each digit, which
// would open the server on each number on the way
void portField(const char *label, unsigned *port, int lowest, int *edit, bool *editing)
{
    if (!*editing)
        *edit = (int)*port;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    ImGui::InputInt(label, edit, 0, 0);
    *editing = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit())
        *port = (unsigned)std::clamp(*edit, lowest, 65535);
}

// [Outputs]: the lamps and the drive board's commands (hl::Outputs), taken at once. The list of what the game sends,
// laid out as sm2-emu's and Flycast's
void drawOutputsTab(bool inGame)
{
    hl::Config *cfg = hl::Config::get ();
    ImGui::Checkbox(tr("Network outputs (MAME protocol)"), &cfg->outputs_network);
    wrappedNote(tr("The cabinet's lamps and NASCAR Arcade's drive board commands, sent as MAME's network output sends them, "
                   "as Supermodel, Flycast and sm2-emu do: for MAMEHooker, BackForceFeeder, DOFLinx and the like. Taken at once."));

    static int portEdit = 0, announceEdit = 0;
    static bool portEditing = false, announceEditing = false;
    ImGui::BeginDisabled(!cfg->outputs_network);
    portField(tr("TCP port"), &cfg->outputs_port, 1, &portEdit, &portEditing);
    wrappedNote(tr("The server's port, on all of this PC's addresses: 8000 as in MAME; another one for a second instance."));
    portField(tr("Announcement port (UDP)"), &cfg->outputs_announce_port, 0, &announceEdit, &announceEditing);
    wrappedNote(tr("When a game starts, its name and the TCP port go once to this port on this PC, as Supermodel sends them "
                   "(8001, where BackForceFeeder listens). 0: none."));
    ImGui::EndDisabled();

    const hl::Outputs::Status status = hl::Outputs::get ()->status ();
    if (!cfg->outputs_network)
        ImGui::TextDisabled("%s", tr("Network outputs off."));
    else if (!inGame)
        ImGui::TextDisabled("%s", tr("The server listens while a game runs."));
    else if (!status.error.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), tr("Not listening: %s"), status.error.c_str());
    else if (!status.listening)
        ImGui::TextDisabled("%s", tr("Not listening yet."));
    else if (status.clients == 0)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), tr("Listening on port %u; no client connected."), cfg->outputs_port);
    else
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), tr("Listening on port %u; clients connected: %u."), cfg->outputs_port,
                           status.clients);

    ImGui::Separator();
    const std::vector<hl::Outputs::Row> rows = hl::Outputs::get ()->rows (tr);
    if (!inGame || rows.empty())
    {
        wrappedNote(tr("What the game sends shows here while it runs (Tab in a game): each name, its type, its meaning and its "
                       "value. README.md lists them for every game."));
        return;
    }
    ImGui::TextUnformatted(tr("What this game sends"));
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##outputs", 4, flags, ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 10.5f)))
    {
        ImGui::TableSetupScrollFreeze(0, 1); // the header stays
        ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(tr("Type"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(tr("Meaning"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Value"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (const hl::Outputs::Row &row : rows)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(row.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(row.type.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(row.meaning.c_str());
            ImGui::SetItemTooltip("%s", row.meaning.c_str()); // whole, when the column cuts it
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(row.value.c_str());
        }
        ImGui::EndTable();
    }
    wrappedNote(tr("Each line goes as \"name = value\" at each change, the drive board's in hexadecimal, the others in decimal; "
                   "a client that connects gets the game's name first, then every value known. Value: the last one, "
                   "with the outputs off too."));
}

void drawPickerTab()
{
    hl::Config *cfg = hl::Config::get ();
    if (ImGui::InputText(tr("Games folder"), cfg->games_dir, sizeof(cfg->games_dir)))
        g_rescanTyped = true;
    if (ImGui::IsItemDeactivatedAfterEdit())
        g_rescan = true;
    char used[2048];
    hl::Config::get ()->games_dir_in_use (used, sizeof(used));
    std::vector<std::string> folders = hl::RomSet::split_folders(used);
    // Windows' dialog: the folder chosen added at the end of the list
    if (canPickFolder())
    {
        if (ImGui::Button(tr("Add a folder...")))
        {
            std::string folder;
            if (pickFolder(tr("Add a games folder"), folders.empty() ? nullptr : folders.back().c_str(), folder))
            {
                std::string list = cfg->games_dir;
                if (!list.empty() && list.back() != ';')
                    list += ';';
                list += folder;
                if (list.size() < sizeof(cfg->games_dir))
                    snprintf(cfg->games_dir, sizeof(cfg->games_dir), "%s", list.c_str());
                g_rescan = true;
            }
        }
        ImGui::SameLine();
    }
    if (ImGui::Button(tr("Scan again")))
        g_rescan = true;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr("Empty: the roms folder next to the program. Several folders: separate them with ';', looked in in that order "
                                 "(the BIOS can be in one, the games in another)."));
    ImGui::TextDisabled("%s", tr("In use:"));
    for (const std::string &folder : folders)
    {
        std::error_code error;
        if (std::filesystem::is_directory(folder, error))
            ImGui::BulletText("%s", folder.c_str());
        else
            ImGui::BulletText(tr("%s (not found)"), folder.c_str());
    }
    ImGui::TextDisabled("%s", tr("MAME romsets (merged, split or non-merged), zips or 7z: <game>.zip or <game>.7z, its parent's and hikaru's for the "
                                 "BIOS. Folders <game>/ with the files unpacked still work."));
    ImGui::PopTextWrapPos();

    ImGui::Separator();
    // Automatic, English, then the language files found, each by its own name
    const char *current = !strcmp(cfg->language, "auto") ? tr("Automatic") : !strcmp(cfg->language, "en") ? "English" : nullptr;
    if (const Language *language = findLanguage(cfg->language))
        current = language->name.c_str();
    if (ImGui::BeginCombo(tr("Language"), current ? current : cfg->language))
    {
        const char *chosen = nullptr;
        if (ImGui::Selectable(tr("Automatic"), !strcmp(cfg->language, "auto")))
            chosen = "auto";
        if (ImGui::Selectable("English", !strcmp(cfg->language, "en")))
            chosen = "en";
        for (const Language &language : g_languages)
            if (ImGui::Selectable(language.name.c_str(), language.code == cfg->language))
                chosen = language.code.c_str();
        ImGui::EndCombo();
        if (chosen)
        {
            snprintf(cfg->language, sizeof(cfg->language), "%s", chosen);
            setLanguage(cfg->language);
        }
    }
    ImGui::TextDisabled("%s", tr("Other languages: a file in data/lang, see README.md."));

    ImGui::Separator();
    ImGui::Checkbox(tr("Write a log file"), &cfg->log_file);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr("hikaruloader.log, next to the program (in the write folder when there is one), the previous "
                                 "start's kept as hikaruloader.old.log: what to send with a problem report. Taken at the next start."));
    ImGui::PopTextWrapPos();

    ImGui::Separator();
    if (ImGui::Checkbox(tr("Download the games' pictures"), &cfg->artwork_download))
        g_rescan = true;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr("From the Arcade Database (adb.arcadeitalia.net, by motoschifo), into cache/artwork: one game at a time, "
                                 "those found, each asked once. Your own pictures show either way: artwork/ next to the program, "
                                 "<set>.png or .jpg, or MAME's titles/, snap/, flyers/ and marquees/ folders."));
    ImGui::PopTextWrapPos();
}

void drawAboutTab()
{
    ImGui::Text("Hikaru Loader %s", HL_VERSION_FULL);
    ImGui::TextDisabled("Dear ImGui %s", IMGUI_VERSION);
    ImGui::TextDisabled("%s (MIT)", LUA_COPYRIGHT);
    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(tr("Hikaru Loader, a SEGA Hikaru emulator."));
    ImGui::TextUnformatted("Copyright (C) 2026, Benjamin Maurin (njz3)");
    ImGui::TextUnformatted(tr("Released under the GNU GPL, version 3 or later."));
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("A work derived from Valkyrie, by Stefano Teso, Copyright (C) 2011-2013."));
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Source code:"));
    ImGui::TextUnformatted("https://github.com/njz3/hikaruloader");
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Interface based on linuxloader's, itself a modified work based on sm2-emu's, by Daniel Martin (dmanlfc) and the sm2-emu "
                              "contributors; modified for Hikaru Loader:"));
    ImGui::TextUnformatted("https://github.com/dmanlfc/sm2-emu");
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Romsets read as sm2-emu does: by CRC32, with miniz; the 7z ones with the LZMA SDK, by Igor Pavlov (public domain)."));
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Games' pictures, when downloaded: scraping using Arcade Database by motoschifo, as sm2-emu does; their copyrights "
                              "belong to their owners. Pictures read with stb_image, by Sean Barrett (public domain)."));
    ImGui::TextUnformatted("https://adb.arcadeitalia.net");
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("CRT presets: Lola Retro's, by Zoki (MIT licence, data/crt/lola-retro/LICENSE), as Odin ships them. The presets' "
                              "reader and passes and the bezels derive from Odin's, by Réda Chérif-Touil (GPL v3 or later)."));
    ImGui::PopTextWrapPos();
}
} // namespace

const char *tr(const char *text)
{
    if (!g_language)
        return text;
    const std::string *translated = g_language->texts.find(text);
    return translated ? translated->c_str() : text;
}

void setLanguage(const char *language)
{
    loadLanguages();
    if (!language || !strcmp(language, "auto"))
        g_language = findLanguage(systemLanguage().c_str());
    else
        g_language = findLanguage(language); // "en", or a file missing: English
}

bool takeRescanRequest(bool settingsOpen)
{
    bool rescan = g_rescan || (g_rescanTyped && !settingsOpen);
    g_rescan = false;
    if (rescan)
        g_rescanTyped = false;
    return rescan;
}

bool initContext(SDL_Window *window)
{
    if (g_initialised)
        return true;

    setLanguage(hl::Config::get ()->language);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr; // the settings live in hikaruloader.ini

    // sm2-emu's dark style
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.GrabRounding = 2.0f;
    style.WindowBorderSize = 0.0f;
    g_baseStyle = style;
    g_uiScale = 0.0f;

    // A font with the accented letters of the French texts
#ifdef _WIN32
    char fontPath[MAX_PATH];
    const char *windir = getenv("WINDIR");
    snprintf(fontPath, sizeof(fontPath), "%s\\Fonts\\segoeui.ttf", windir ? windir : "C:\\Windows");
    if (FILE *font = fopen(fontPath, "rb"))
    {
        fclose(font);
        io.Fonts->AddFontFromFileTTF(fontPath, 17.0f);
    }
#endif

    if (!ImGui_ImplSDL2_InitForOpenGL(window, SDL_GL_GetCurrentContext()) || !ImGui_ImplOpenGL3_Init("#version 140"))
    {
        ImGui::DestroyContext();
        return false;
    }
    g_initialised = true;
    return true;
}

void shutdownContext()
{
    if (!g_initialised)
        return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    g_initialised = false;
}

bool processEvent(const SDL_Event *event)
{
    if (!g_initialised)
        return false;
    return ImGui_ImplSDL2_ProcessEvent(event);
}

void newFrame()
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    applyScale();
    ImGui::NewFrame();
}

void renderFrame(int width, int height)
{
    ImGui::Render();
    glViewport(0, 0, width, height);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void drawSettings(bool *open, bool inGame)
{
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImVec2 size(std::min(viewport->WorkSize.x * 0.9f, 660.0f * g_uiScale), std::min(viewport->WorkSize.y * 0.9f, 500.0f * g_uiScale));
    ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
    // Never larger than the window, even one made smaller while the settings are open
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), viewport->WorkSize);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    char title[64];
    snprintf(title, sizeof(title), "%s###settings", tr("Settings"));
    if (!ImGui::Begin(title, open, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    }
    // Kept inside the window: one made smaller after the settings opened left them out of sight
    {
        const ImVec2 pos = ImGui::GetWindowPos(), extent = ImGui::GetWindowSize();
        const ImVec2 inside(std::clamp(pos.x, viewport->WorkPos.x, std::max(viewport->WorkPos.x, viewport->WorkPos.x + viewport->WorkSize.x - extent.x)),
                            std::clamp(pos.y, viewport->WorkPos.y, std::max(viewport->WorkPos.y, viewport->WorkPos.y + viewport->WorkSize.y - extent.y)));
        if (inside.x != pos.x || inside.y != pos.y)
            ImGui::SetWindowPos(inside);
    }

    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
    ImGui::BeginChild("##tabs", ImVec2(0.0f, -footer), false);
    if (ImGui::BeginTabBar("##settingsTabs"))
    {
        if (ImGui::BeginTabItem(tr("Display")))
        {
            drawDisplayTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr("Enhancements")))
        {
            drawEnhancementsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr("Controls")))
        {
            drawControlsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr("Sound")))
        {
            drawSoundTab(inGame);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr("Emulation")))
        {
            drawEmulationTab(inGame);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr("Outputs")))
        {
            drawOutputsTab(inGame);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr("Picker")))
        {
            drawPickerTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr("About")))
        {
            drawAboutTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::Separator();
    if (ImGui::Button(tr("Save")))
    {
        char message[1200];
        snprintf(message, sizeof(message), hl::Config::get ()->save () ? tr("Saved to %s") : tr("Cannot write %s"), hl::Config::path ());
        g_status = message;
        g_rescan = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Close")))
        *open = false;
    if (!g_status.empty())
    {
        // Beside the buttons when it fits, else on the line under them
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + ImGui::CalcTextSize(g_status.c_str()).x <= right)
            ImGui::SameLine();
        ImGui::TextDisabled("%s", g_status.c_str());
    }
    ImGui::End();
}
} // namespace ui
