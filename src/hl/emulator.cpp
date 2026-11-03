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

#include <filesystem>

#include "hl/emulator.h"
#include "hl/shell.h"
#include "hl/agent.h"
#include "hl/audio.h"
#include "hl/bezel.h"
#include "hl/cpu.h"
#include "hl/input.h"
#include "hl/outputs.h"
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
	destroy (audio);
	destroy (agent);
	destroy (shell);
	destroy (mach);
	destroy (game);
	destroy (game_list);
}

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
}

int
Emulator::load_or_save_state (bool load)
{
	std::string file = std::string (mach->game->name) + ".hlstate";
	std::string path = load ? Paths::game_read (mach->game->name, file)
	                        : Paths::game_write (mach->game->name, file);

	return load ? load_state (path.c_str ()) : save_state (path.c_str ());
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
		if (event.type == SDL_QUIT) {
			HL_PRINT ("quitting: the window was closed");
			quit = true;
			continue;
		}
		/* Tab, and everything while the settings are open */
		if (ui::Interface::process_event (&event))
			continue;
		/* XXX use CTRL+[1,5] for saving, SHIFT+[1,5] for loading */
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
		} else
			Window::present_paused ();
		limit_frame_rate ();
	}
	if (shell)
		shell->call_hooks (Shell::EVENT_EXIT);
}

} /* namespace hl */
