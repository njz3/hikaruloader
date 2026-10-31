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

// The download follows sm2-emu's picker artwork (Daniel Martin's scraper,
// GPL v3 or later): the Arcade Database's query by set name, its pictures at
// resize=400, a <set>.json kept for each game asked, a failure retried at the
// next start.

#include "ui/ui_artwork.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

#include <GL/glew.h>
#include <jansson.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <stb_image.h>

#include "hl/config.h"
#include "hl/core.h"
#include "hl/paths.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>
#pragma comment(lib, "wininet.lib")
#endif

namespace
{
// The kinds of pictures, in their order, and the folders of MAME's packs
const char *const KINDS[] = { "title", "ingame", "flyer", "marquee" };
const char *const PACKS[] = { "titles", "snap", "flyers", "marquees" };
const char *const EXTS[] = { ".png", ".jpg" };
// Textures made a frame, so that the picker does not stall
const int TEXTURES_A_FRAME = 2;

struct SetArt
{
    std::vector<std::string> files;
    std::vector<ui::ArtworkImage> images;
    std::vector<int> state; // 0 to load, 1 loaded, -1 not an image
};

std::map<std::string, SetArt> g_sets;
int g_budget = TEXTURES_A_FRAME;

std::mutex g_mutex;
std::vector<std::string> g_ready; // downloaded, for the main thread
std::thread g_worker;
std::atomic<bool> g_stopping{false};
bool g_started = false;

std::string cacheDir()
{
    return hl::Paths::config_write("cache/artwork/");
}

bool exists(const std::string &path)
{
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

// The set's picture files: the artwork folder's first, the downloaded ones next
SetArt &scan(const std::string &set)
{
    auto found = g_sets.find(set);
    if (found != g_sets.end())
        return found->second;
    SetArt &art = g_sets[set];
    for (const char *ext : EXTS)
    {
        std::string path = hl::Paths::config_read("artwork/" + set + ext);
        if (exists(path))
            art.files.push_back(path);
    }
    for (const char *pack : PACKS)
        for (const char *ext : EXTS)
        {
            std::string path = hl::Paths::config_read(std::string("artwork/") + pack + "/" + set + ext);
            if (exists(path))
                art.files.push_back(path);
        }
    const std::string cache = cacheDir();
    for (const char *kind : KINDS)
        for (const char *ext : EXTS)
        {
            std::string path = cache + set + "." + kind + ext;
            if (exists(path))
                art.files.push_back(path);
        }
    art.images.resize(art.files.size());
    art.state.assign(art.files.size(), 0);
    return art;
}

// The set's own pictures, else its parent's
SetArt *resolve(const char *set, const char *parent)
{
    SetArt *art = &scan(set);
    if (art->files.empty() && parent && parent[0])
        art = &scan(parent);
    return art->files.empty() ? nullptr : art;
}

bool makeTexture(const std::string &path, ui::ArtworkImage *image)
{
    int width, height, channels;
    stbi_uc *pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!pixels)
    {
        HL_ERROR("artwork: cannot read '%s': %s", path.c_str(), stbi_failure_reason());
        return false;
    }
    GLint bound = 0, alignment = 4;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
    stbi_image_free(pixels);
    image->texture = (ImTextureID)(intptr_t)texture;
    image->width = width;
    image->height = height;
    return true;
}

void freeTextures(SetArt &art)
{
    for (ui::ArtworkImage &image : art.images)
        if (image.texture)
        {
            GLuint texture = (GLuint)(intptr_t)image.texture;
            glDeleteTextures(1, &texture);
            image.texture = 0;
        }
}

#ifdef _WIN32
std::atomic<HINTERNET> g_session{nullptr};

// A GET, its body (4 MB at most) and its Content-Type; false on any failure
bool httpGet(HINTERNET session, const std::string &url, std::string &body, std::string *type)
{
    HINTERNET request = InternetOpenUrlA(session, url.c_str(), nullptr, 0,
                                         INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_UI, 0);
    if (!request)
        return false;
    DWORD status = 0, size = sizeof(status);
    bool ok = HttpQueryInfoA(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &size, nullptr) && status == 200;
    if (ok && type)
    {
        char buffer[128] = "";
        size = sizeof(buffer) - 1;
        *type = HttpQueryInfoA(request, HTTP_QUERY_CONTENT_TYPE, buffer, &size, nullptr) ? buffer : "";
    }
    body.clear();
    char chunk[16384];
    DWORD read = 0;
    while (ok && InternetReadFile(request, chunk, sizeof(chunk), &read) && read)
    {
        body.append(chunk, read);
        if (body.size() > 4u << 20 || g_stopping)
            ok = false;
    }
    InternetCloseHandle(request);
    return ok && !body.empty();
}

// A picture of one colour, almost all black (97 %), or not a picture: the
// database's title and in-game shots of the Hikaru games are black, at most
// a grey border (MAME draws none), left out
bool blank(const std::string &data)
{
    int width, height, channels;
    stbi_uc *pixels = stbi_load_from_memory((const stbi_uc *)data.data(), (int)data.size(), &width, &height, &channels, 4);
    if (!pixels)
        return true;
    const size_t n = (size_t)width * height;
    const uint32_t *p = (const uint32_t *)pixels;
    size_t same = 0, black = 0;
    for (size_t i = 0; i < n; i++)
    {
        same += p[i] == p[0];
        black += pixels[i * 4] <= 16 && pixels[i * 4 + 1] <= 16 && pixels[i * 4 + 2] <= 16;
    }
    stbi_image_free(pixels);
    return same == n || black * 100 >= n * 97;
}

bool writeFile(const std::string &path, const std::string &data)
{
    FILE *fp = fopen(path.c_str(), "wb");
    if (!fp)
        return false;
    bool ok = fwrite(data.data(), 1, data.size(), fp) == data.size();
    return fclose(fp) == 0 && ok;
}

// A pause between two games, cut short when stopping
void pause(int ms)
{
    for (int t = 0; t < ms && !g_stopping; t += 50)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void download(std::vector<std::string> sets, std::string dir)
{
    HINTERNET session = InternetOpenA("hikaruloader/" HL_VERSION " (SEGA Hikaru emulator; game picker artwork)",
                                      INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!session)
        return;
    DWORD connect = 8000, receive = 15000;
    InternetSetOptionA(session, INTERNET_OPTION_CONNECT_TIMEOUT, &connect, sizeof(connect));
    InternetSetOptionA(session, INTERNET_OPTION_RECEIVE_TIMEOUT, &receive, sizeof(receive));
    InternetSetOptionA(session, INTERNET_OPTION_SEND_TIMEOUT, &receive, sizeof(receive));
    g_session = session;

    std::error_code error;
    std::filesystem::create_directories(dir, error);
    for (const std::string &set : sets)
    {
        if (g_stopping)
            break;
        // Asked already, found or not
        if (exists(dir + set + ".json"))
            continue;
        std::string body;
        if (!httpGet(session, "https://adb.arcadeitalia.net/service_scraper.php?ajax=query_mame&lang=en&game_name=" + set, body, nullptr))
        {
            // Offline, or the service down: again at the next start
            HL_ERROR("artwork: the Arcade Database did not answer for %s; again at the next start", set.c_str());
            break;
        }
        json_error_t jerror;
        json_t *root = json_loads(body.c_str(), 0, &jerror);
        json_t *result = root ? json_object_get(root, "result") : nullptr;
        json_t *game = json_is_array(result) && json_array_size(result) ? json_array_get(result, 0) : nullptr;
        json_t *saved = json_object();
        json_t *images = json_array();
        for (const char *kind : KINDS)
        {
            if (!game || g_stopping)
                break;
            const char *url = json_string_value(json_object_get(game, (std::string("url_image_") + kind).c_str()));
            if (!url || !url[0])
                continue;
            std::string small = url, data, type;
            size_t at = small.find("resize=0");
            if (at != std::string::npos)
                small.replace(at, 8, "resize=400");
            if (!httpGet(session, small, data, &type) || type.compare(0, 6, "image/") || blank(data))
                continue;
            const char *ext = type.find("png") != std::string::npos ? ".png" : ".jpg";
            if (writeFile(dir + set + "." + kind + ext, data))
                json_array_append_new(images, json_string(kind));
        }
        json_object_set_new(saved, "found", json_boolean(game != nullptr));
        json_object_set_new(saved, "images", images);
        if (!g_stopping)
        {
            json_dump_file(saved, (dir + set + ".json").c_str(), JSON_INDENT(1));
            HL_PRINT("artwork: %s, %u pictures from the Arcade Database%s", set.c_str(),
                     (unsigned)json_array_size(images), game ? "" : " (not known there)");
            std::lock_guard<std::mutex> lock(g_mutex);
            g_ready.push_back(set);
        }
        json_decref(saved);
        if (root)
            json_decref(root);
        pause(300);
    }
    session = g_session.exchange(nullptr);
    if (session)
        InternetCloseHandle(session);
}
#endif
} // namespace

namespace ui
{
void artworkStart(const std::vector<std::string> &sets)
{
#ifdef _WIN32
    if (g_started || !hl::Config::get()->artwork_download || sets.empty())
        return;
    g_started = true;
    g_stopping = false;
    g_worker = std::thread(download, sets, cacheDir());
#else
    (void)sets;
#endif
}

void artworkPoll()
{
    g_budget = TEXTURES_A_FRAME;
    std::vector<std::string> ready;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ready.swap(g_ready);
    }
    if (ready.empty())
        return;
    // Scanned again: the set itself, and those without a picture (its clones)
    for (auto it = g_sets.begin(); it != g_sets.end();)
    {
        bool again = it->second.files.empty();
        for (const std::string &set : ready)
            again |= it->first == set;
        if (again)
        {
            freeTextures(it->second);
            it = g_sets.erase(it);
        }
        else
            ++it;
    }
}

int artworkCount(const char *set, const char *parent)
{
    SetArt *art = resolve(set, parent);
    return art ? (int)art->files.size() : 0;
}

bool artworkGet(const char *set, const char *parent, int index, ArtworkImage *image)
{
    SetArt *art = resolve(set, parent);
    if (!art || index < 0 || index >= (int)art->files.size())
        return false;
    if (art->state[index] == 0)
    {
        if (g_budget <= 0)
            return false;
        g_budget--;
        art->state[index] = makeTexture(art->files[index], &art->images[index]) ? 1 : -1;
    }
    if (art->state[index] != 1)
        return false;
    *image = art->images[index];
    return true;
}

void artworkShutdown()
{
#ifdef _WIN32
    if (g_worker.joinable())
    {
        g_stopping = true;
        // Closing the session cuts the request in progress short
        HINTERNET session = g_session.exchange(nullptr);
        if (session)
            InternetCloseHandle(session);
        g_worker.join();
    }
    g_started = false;
#endif
    for (auto &set : g_sets)
        freeTextures(set.second);
    g_sets.clear();
}
} // namespace ui
