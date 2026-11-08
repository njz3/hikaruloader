/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from src/vk/main.c, Copyright (C) 2011, Stefano Teso.
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

/* TODO: move to a scheduler-based system, so that machines register their
 * components and the scheduler executes them. */

#include <atomic>
#include <chrono>
#include <filesystem>

#include "hl/emulator.h"
#include "hl/ffb-effects.h"
#include "hl/shell.h"
#include "hl/agent.h"
#include "hl/audio.h"
#include "hl/bezel.h"
#include "hl/cpu.h"
#include "hl/input.h"
#include "hl/outputs.h"
#include "hl/telemetry.h"
#include "hl/renderer.h"
#include "hl/window.h"
#include "hl/config.h"
#include "hl/paths.h"
#include "hl/profile.h"
#include "ui/ui.h"

#ifdef HL_HAVE_HIKARU
#include "mach/hikaru/hikaru.h"
#include "mach/gaelco/gaelco.h"
#endif

namespace hl {

Emulator::Emulator () :
	paused (false),
	crashed (false),
	quit (false),
	from_picker (false),
	back_to_picker (false),
	in_frame (false),
	in_machine (false),
	last_stop (STOP_NONE),
	snapshot_every (0),
	snapshot_keep (0),
	game_list (NULL),
	game (NULL),
	mach (NULL),
	shell (NULL),
	agent (NULL),
	audio (new Audio ()),
	next_frame (0)
{
	rom_name[0] = '\0';
	stop_message[0] = '\0';
}

Emulator::~Emulator ()
{
	/* mame_stop, the game's last line */
	Outputs::get ()->stop ();
	telemetry::Service::get ().stop ();
	destroy (audio);
	destroy (agent);
	destroy (shell);
	destroy (mach);
	destroy (game);
	destroy (game_list);
}

/* The save states' message over the game (show_state_notice) */
static std::atomic<int> notice_message, notice_slot, notice_age;
static std::atomic<Uint32> notice_until;

static char *
get_home_dir (void)
{
	char *path;
	path = getenv ("HOME");
#ifdef _WIN32
	if (!path)
		path = getenv ("USERPROFILE");
#else
	if (!path) {
		struct passwd *pwd = getpwuid (getuid ());
		if (pwd)
			path = pwd->pw_dir;
	}
#endif
	return path;
}

bool
Emulator::load_game_list (const char *base)
{
	char paths[3][1024], *home;
	unsigned i;

	home = get_home_dir ();

	/* Beside the program first (base; the write directory's when there),
	 * so that it runs from anywhere */
	snprintf (paths[0], sizeof (paths[0]), "%s", Paths::config_read ("data/games.json").c_str ());
	snprintf (paths[1], sizeof (paths[1]), "./data/games.json");
	snprintf (paths[2], sizeof (paths[2]), "%s/.local/share/hikaruloader/games.json", home ? home : ".");

	for (i = 0; i < NUMELEM (paths) && !game_list; i++) {
		FILE *fp = fopen (paths[i], "rb");
		if (!fp)
			continue;
		fclose (fp);
		game_list = GameList::load (paths[i]);
		if (game_list)
			HL_PRINT ("loaded the game list from '%s'", paths[i]);
	}
	return game_list != NULL;
}

Machine *
Emulator::create_machine ()
{
	if (!strcmp (game->mach, "hikaru"))
		return hikaru::Hikaru::create (game);
	if (!strcmp (game->mach, "gaelco"))
		return gaelco::Gaelco::create (game);
	return NULL;
}

bool
Emulator::load_game (const char *name, const char *log_path)
{
	char message[512] = "", folders[2048];

	from_picker = !name[0];
	back_to_picker = false;
	snprintf (rom_name, sizeof (rom_name), "%s", name);

	while (true) {
		GameEntry *entry;

		if (from_picker &&
		    !ui::Interface::pick_game (game_list, rom_name, sizeof (rom_name),
		                               message[0] ? message : NULL))
			return false;

		entry = game_list->find (rom_name);
		ui::Interface::show_loading (entry ? entry->description : rom_name);

		Config::get ()->games_dir_in_use (folders, sizeof (folders));
		game = Game::create (game_list, folders, rom_name);
		if (!game) {
			HL_ERROR ("failed to load '%s': can't load game files", rom_name);
			snprintf (message, sizeof (message), ui::Interface::tr ("%s could not be loaded; see %s."),
			          entry ? entry->description : rom_name, log_path);
		} else {
			/* The machine's I/O board declares its outputs */
			Outputs::get ()->stop ();
			/* Its bezel picture (BEZEL = IMAGE), read at its first frame */
			Bezel::set_game (game->name, game->parent);
			mach = create_machine ();
			cpu_core = -1;
			memory_access = -1;
			if (mach) {
				char title[256];
				snprintf (title, sizeof (title), "%s [%s]",
				          entry ? entry->description : game->name, game->name);
				Window::set_title (title);
				Outputs::get ()->start (game->name, game->parent);
				telemetry::Service::get ().start (mach);
				return true;
			}
			HL_ERROR ("failed to load '%s': game name not in game list", rom_name);
			snprintf (message, sizeof (message), ui::Interface::tr ("%s needs a machine Hikaruloader does not emulate."),
			          entry ? entry->description : rom_name);
			destroy (game);
		}
		if (!from_picker)
			return false;
	}
}

void
Emulator::unload_game ()
{
	Outputs::get ()->stop ();
	telemetry::Service::get ().stop ();
	destroy (mach);
	destroy (game);
	Bezel::set_game (NULL, NULL);
	/* The next output opens with the next game */
	audio->close ();
	paused = false;
	crashed = false;
	quit = false;
	last_stop = STOP_NONE;
	snapshots.clear ();
	state_slot = 0;
	notice_until = 0;
}

/* The save states' message over the game: 3 seconds, or until the next one */
#define STATE_NOTICE_MS		3000
#define STATE_NOTICE_HELD	0xFFFFFFFFu

static void
show_state_notice (Emulator::StateMessage message, unsigned slot, int age = 0, bool held = false)
{
	notice_message = (int) message;
	notice_slot = (int) slot;
	notice_age = age;
	notice_until = held ? STATE_NOTICE_HELD : ((SDL_GetTicks () + STATE_NOTICE_MS) | 1);
}

Emulator::StateNotice
Emulator::state_notice ()
{
	Uint32 until = notice_until;

	if (!until || (until != STATE_NOTICE_HELD && SDL_TICKS_PASSED (SDL_GetTicks (), until)))
		return { STATE_NO_MESSAGE, 0, 0 };
	return { (StateMessage) notice_message.load (), (unsigned) notice_slot.load (), notice_age.load () };
}

static std::string
state_file (const char *game, unsigned slot)
{
	return std::string (game) + "-" + std::to_string (slot) + ".hlstate";
}

/* The slot's file, empty when it has none */
static std::string
state_to_read (const char *game, unsigned slot)
{
	std::error_code ec;
	std::string path = Paths::game_read (game, state_file (game, slot));

	if (std::filesystem::exists (path, ec))
		return path;
	if (slot == 0) {
		path = Paths::game_read (game, std::string (game) + ".hlstate");
		if (std::filesystem::exists (path, ec))
			return path;
	}
	return "";
}

/* A state saved beside them, and only then the rotation: a save that fails
 * loses none of the others */
static int
save_rotating (Machine *mach)
{
	namespace fs = std::filesystem;
	const char *game = mach->game->name;
	std::string name (game);
	std::string fresh = Paths::game_write (game, name + "-new.hlstate");
	std::string newest = Paths::game_write (game, state_file (game, 0));
	std::string older = Paths::game_write (game, name + ".hlstate");
	std::error_code ec;
	unsigned i;

	if (mach->save_state (fresh.c_str ())) {
		HL_ERROR ("failed to save state '%s'", fresh.c_str ());
		fs::remove (fresh, ec);
		return -1;
	}
	if (!fs::exists (newest, ec) && fs::exists (older, ec))
		fs::rename (older, newest, ec);
	for (i = Emulator::NUM_STATE_SLOTS - 1; i > 0; i--) {
		std::string from = Paths::game_write (game, state_file (game, i - 1));
		std::string to = Paths::game_write (game, state_file (game, i));

		if (!fs::exists (from, ec))
			continue;
		fs::rename (from, to, ec);
		if (ec)
			HL_ERROR ("failed to rotate state '%s': %s", from.c_str (), ec.message ().c_str ());
	}
	fs::rename (fresh, newest, ec);
	if (ec) {
		HL_ERROR ("failed to name state '%s': %s", newest.c_str (), ec.message ().c_str ());
		return -1;
	}
	HL_PRINT ("saved state '%s'", newest.c_str ());
	return 0;
}

int
Emulator::load_or_save_state (bool load)
{
	int ret;

	if (!load) {
		ret = save_rotating (mach);
		state_slot = 0;
		show_state_notice (ret ? STATE_SAVE_FAILED : STATE_SAVED, 0);
		return ret;
	}
	std::string path = state_to_read (mach->game->name, state_slot);
	if (path.empty ()) {
		HL_PRINT ("no state number %u to load", state_slot);
		show_state_notice (STATE_EMPTY, state_slot, 0, state_slot != 0);
		return -1;
	}
	ret = load_state (path.c_str ());
	show_state_notice (ret ? STATE_LOAD_FAILED : STATE_LOADED, state_slot, 0, ret && state_slot != 0);
	if (!ret)
		state_slot = 0;
	return ret;
}

void
Emulator::select_state (int delta)
{
	namespace fs = std::filesystem;
	std::error_code ec;

	state_slot = (state_slot + NUM_STATE_SLOTS + delta) % NUM_STATE_SLOTS;
	std::string path = state_to_read (mach->game->name, state_slot);
	if (path.empty ()) {
		show_state_notice (STATE_EMPTY, state_slot, 0, state_slot != 0);
		return;
	}
	auto age = fs::file_time_type::clock::now () - fs::last_write_time (path, ec);
	show_state_notice (STATE_SELECTED, state_slot,
	                   ec ? 0 : (int) std::chrono::duration_cast<std::chrono::seconds> (age).count (),
	                   state_slot != 0);
}

int
Emulator::load_state (const char *path)
{
	int ret = mach->load_state (path);

	if (!ret) {
		HL_PRINT ("loaded state '%s'", path);
		crashed = false;
		last_stop = STOP_NONE;
	} else
		HL_ERROR ("failed to load state '%s'", path);
	return ret;
}

int
Emulator::save_state (const char *path)
{
	int ret = mach->save_state (path);

	if (!ret)
		HL_PRINT ("saved state '%s'", path);
	else
		HL_ERROR ("failed to save state '%s'", path);
	return ret;
}

void
Emulator::reset (ResetType type)
{
	mach->reset (type);
	crashed = false;
	last_stop = STOP_NONE;
}

bool
Emulator::open_shell (unsigned port)
{
	shell = new Shell (this);
	if (!port)
		return true;
	if (!shell->listen (port))
		return false;
	Log::recover_aborts = true;
	return true;
}

void
Emulator::open_agent ()
{
	agent = new Agent (this);
	Log::recover_aborts = true;
	paused = true;
	agent->start ();
}

bool
Emulator::process_events ()
{
	SDL_Event event;
	bool quit = false;

	while (SDL_PollEvent (&event)) {
		if (Window::closing (&event)) {
			HL_PRINT ("quitting: the window was closed");
			quit = true;
			continue;
		}
		/* For a problem report: without the focus the keys do nothing,
		 * which a still menu shows as a freeze */
		if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
			HL_PRINT ("window: the keyboard's focus lost, the keys do nothing until it is back");
		else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
			HL_PRINT ("window: the keyboard's focus back");
		/* Tab, and everything while the settings are open */
		if (ui::Interface::process_event (&event))
			continue;
		switch (Input::handle_event (&event)) {
		case ACTION_QUIT:
			HL_PRINT ("quitting: the Quit input was pressed");
			quit = true;
			break;
		case ACTION_BACK:
			/* Escape, Guide + B: back to the picker the game came
			 * from, else out; not for an agent nor a shell, which
			 * drive the machine */
			if (agent || shell)
				break;
			back_to_picker = from_picker;
			if (back_to_picker)
				HL_PRINT ("back to the picker: the Back input was pressed");
			else
				HL_PRINT ("quitting: the Back input was pressed");
			quit = true;
			break;
		case ACTION_PAUSE:
			HL_PRINT ("%s: the Pause input was pressed", paused ? "resumed" : "paused");
			if (paused)
				resume ();
			else
				paused = true;
			break;
		case ACTION_LOAD_STATE:
			load_or_save_state (true);
			break;
		case ACTION_SAVE_STATE:
			load_or_save_state (false);
			break;
		case ACTION_STATE_NEWER:
			select_state (-1);
			break;
		case ACTION_STATE_OLDER:
			select_state (1);
			break;
		case ACTION_SHOW_FPS:
			Config::get ()->fps_overlay ^= 1;
			break;
		default:
			break;
		}
	}
	return quit;
}

/* Waits for the next frame's time, as [Display] FPS_TARGET says; a frame late
 * by more than a few is not caught up. */
void
Emulator::limit_frame_rate ()
{
	Config *cfg = Config::get ();
	Uint64 freq = SDL_GetPerformanceFrequency (), now, period;

	if (!cfg->fps_limiter || cfg->fps_target <= 0.0f) {
		next_frame = 0;
		return;
	}

	period = (Uint64) (freq / cfg->fps_target);
	now = SDL_GetPerformanceCounter ();
	if (!next_frame || now > next_frame + 4 * period)
		next_frame = now;
	next_frame += period;

	while ((now = SDL_GetPerformanceCounter ()) < next_frame) {
		Uint64 left_ms = (next_frame - now) * 1000 / freq;
		if (left_ms > 1)
			SDL_Delay ((Uint32) (left_ms - 1));
	}
}

Emulator::Stop
Emulator::run_frame ()
{
	/* HR_PROFILE: all but the limiter's wait (hl/profile.h) */
	HL_PROFILE (FRAME);

	last_stop = STOP_NONE;
	in_frame = true;
	/* The settings' CPU core, changed: from this frame on. The dynamic
	 * recompilers leave the machine as the interpreters do */
	if (cpu_core != (int) Config::get ()->dynarec) {
		cpu_core = Config::get ()->dynarec;
		for (Cpu *cpu : mach->cpus)
			cpu->set_option ("dynarec", cpu_core);
	}
	/* The memory access, likewise; fastmem keeps the page table for
	 * what it does not show, and falls back to it where the host cannot */
	if (memory_access != (int) Config::get ()->memory_access) {
		memory_access = Config::get ()->memory_access;
		for (Cpu *cpu : mach->cpus) {
			cpu->mmap->set_page_table (memory_access != MEMORY_REGIONS);
			if (!cpu->mmap->set_fastmem (memory_access == MEMORY_FASTMEM))
				HL_ERROR ("fastmem: this system cannot, the page table instead");
		}
	}
	if (shell)
		shell->hook_number (Shell::EVENT_FRAME_BEGIN, mach->frame + 1);

	try {
		in_machine = true;
		{
			HL_PROFILE (RENDER_BEGIN);
			mach->renderer->begin_frame ();
		}
		{
			HL_PROFILE (MACHINE);
			mach->run_frame ();
		}
		{
			HL_PROFILE (RENDER_END);
			mach->renderer->end_frame ();
		}
		in_machine = false;
	} catch (const Abort &e) {
		in_machine = false;
		aborted (e.what ());
		in_frame = false;
		return STOP_ABORT;
	}
	/* The settings' sound: the volume and the balance at once, other
	 * speakers and another latency when the output opens again */
	if (audio->output != (Audio::Output) Config::get ()->sound_output ||
	    audio->latency != (unsigned) Config::get ()->sound_latency) {
		audio->output = (Audio::Output) Config::get ()->sound_output;
		audio->latency = (unsigned) Config::get ()->sound_latency;
		audio->close ();
	}
	audio->balance = (unsigned) Config::get ()->sound_balance;
	audio->volume = (unsigned) Config::get ()->sound_volume;
	/* The sound's pace: the limiter's, none for the agent's steps */
	audio->frame_rate = (!agent && Config::get ()->fps_limiter) ? Config::get ()->fps_target : 0.0f;
	{
		HL_PROFILE (AUDIO);
		audio->play (mach->audio, mach->audio_channels);
	}
	/* The racing game's telemetry of this frame, its sound still there
	 * for a decoder that reads it */
	telemetry::Service::get ().on_frame (mach->frame + 1);
	GameRumble::frame (mach, mach->frame + 1);
	mach->audio.clear ();
	mach->frame++;
	Window::set_frame (mach->frame);
	if (shell)
		shell->hook_number (Shell::EVENT_FRAME_END, mach->frame);

	for (Cpu *cpu : mach->cpus)
		if (cpu->break_hit) {
			snprintf (stop_message, sizeof (stop_message),
			          "break: %s at %08X, frame %llu",
			          cpu->name ? cpu->name : "cpu", cpu->break_pc,
			          (unsigned long long) mach->frame);
			HL_PRINT ("%s", stop_message);
			paused = true;
			last_stop = STOP_BREAK;
			if (shell)
				shell->hook_text (Shell::EVENT_BREAK, stop_message);
			break;
		}
	in_frame = false;

	if (last_stop == STOP_NONE && snapshot_every && !(mach->frame % snapshot_every))
		take_snapshot ();
	return last_stop;
}

/* Snapshots */

void
Emulator::set_snapshots (unsigned every, unsigned keep, const char *dir)
{
	std::error_code error;

	snapshot_every = every;
	snapshot_keep = keep ? keep : 1;
	snapshot_dir = dir;
	if (every)
		std::filesystem::create_directories (snapshot_dir, error);
}

void
Emulator::take_snapshot ()
{
	char name[96];
	std::string path;

	snprintf (name, sizeof (name), "%s-%llu.hlstate", game->name,
	          (unsigned long long) mach->frame);
	path = snapshot_dir + "/" + name;
	if (mach->save_state (path.c_str ()))
		return;
	snapshots[mach->frame] = path;
	/* The oldest go */
	while (snapshots.size () > snapshot_keep) {
		remove (snapshots.begin ()->second.c_str ());
		snapshots.erase (snapshots.begin ());
	}
}

bool
Emulator::seek (uint64_t target)
{
	auto after = snapshots.upper_bound (target);
	bool back = target < mach->frame;

	if (in_frame)
		return false;
	/* From the last snapshot before target, unless it is nearer from here */
	if (after != snapshots.begin () &&
	    (back || std::prev (after)->first > mach->frame)) {
		if (load_state (std::prev (after)->second.c_str ()))
			return false;
	} else if (back)
		reset (RESET_HARD);

	/* Going back, the snapshots after target belong to the past run */
	while (back && !snapshots.empty () && snapshots.rbegin ()->first > target) {
		remove (snapshots.rbegin ()->second.c_str ());
		snapshots.erase (std::prev (snapshots.end ()));
	}

	while (mach->frame < target)
		if (step (1) != 1 || last_stop != STOP_NONE)
			return false;
	return true;
}

void
Emulator::aborted (const char *what)
{
	snprintf (stop_message, sizeof (stop_message), "abort: %s", what);
	HL_PRINT ("the machine aborted, frame %llu; paused for the shell",
	          (unsigned long long) mach->frame);
	crashed = true;
	paused = true;
	last_stop = STOP_ABORT;
	if (shell)
		shell->hook_text (Shell::EVENT_ABORT, stop_message);
}

/* The CPUs stopped at a breakpoint run its instruction next */
static void
leave_breakpoints (Machine *mach)
{
	for (Cpu *cpu : mach->cpus)
		cpu->resume ();
}

int
Emulator::step (int num)
{
	int i;

	if (crashed)
		return -1;
	leave_breakpoints (mach);
	for (i = 0; i < num; i++) {
		/* Keeps the window alive during long steps */
		SDL_PumpEvents ();
		if (run_frame () != STOP_NONE)
			return i + 1;
	}
	return num;
}

bool
Emulator::resume ()
{
	if (crashed) {
		HL_ERROR ("the machine aborted: reset it, or load a state");
		return false;
	}
	leave_breakpoints (mach);
	paused = false;
	return true;
}

void
Emulator::run (int num_frames)
{
	int frame = 0;

	next_frame = 0;
	if (shell)
		shell->call_hooks (Shell::EVENT_START);
	while (!quit && !process_events ()) {
		/* The shell's commands, between two frames */
		if (shell)
			shell->poll ();
		if (quit)
			break;
		/* The outputs' clients; paused, the settings open too (not for
		 * the agent, whose frames run on its steps) */
		Outputs::get ()->set_paused (!agent && (paused || ui::Interface::settings_open ()));
		Outputs::get ()->poll ();
		/* The agent's: frames run on its steps only, as fast as they can;
		 * the window is redrawn while it waits */
		if (agent) {
			if (!agent->poll (16))
				Window::present_paused ();
			continue;
		}
		if (!paused && !ui::Interface::settings_open ()) {
			if (num_frames > 0 && frame++ >= num_frames)
				break;
			if (run_frame () != STOP_NONE && shell)
				shell->notify (stop_message);
		} else {
			/* The telemetry's last frame again, as a pause */
			telemetry::Service::get ().on_pause ();
			GameRumble::stop (mach->frame);
			Window::present_paused ();
		}
		limit_frame_rate ();
	}
	if (shell)
		shell->call_hooks (Shell::EVENT_EXIT);
}

} /* namespace hl */
