/* 
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2011, Stefano Teso
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

#include "hl/pointers.h"
#include "hl/emulator.h"
#include "hl/shell.h"
#include "hl/window.h"
#include "hl/config.h"
#include "hl/controls.h"
#include "hl/paths.h"
#include "hl/profile.h"
#include "ui/ui.h"

static struct {
	char rom_path[2048];
	char rom_name[256];
	int num_frames;
	int start_state;
	char state_path[256];
	unsigned shell_port;
	bool agent;
	bool list;
	char script[256];
} options;

static hl::Emulator *emulator;

static const char global_opts[] = "R:r:n:l:s:x:Lvh?";
static const char global_help[] = "Usage: %s [options]\n"
"	-R <path>	Path to the ROM directory (MAME zips, or a folder per game);\n"
"			several separated by ';', looked in in that order\n"
"	-r <string>	Name of the game to run (MAME set name); none: the picker\n"
"	-n <num>	Only run for num frames\n"
"	-l <num|file>	Load state num (<game>-<num>.hlstate, 0 the latest, 4 the\n"
"			oldest), or a state file, at startup:\n"
"			the emulation goes on from its frame\n"
"	-s <port>	Open the Lua shell on 127.0.0.1:port (telnet, tools/hlshell.py)\n"
"	-a		Agent mode, as Supermodel's and sm2-emu's: commands on stdin,\n"
"			answers on stdout, frames on its steps; also -agent, --agent,\n"
"			-agent-show and --agent-show\n"
"	-x <file>	Run a Lua script once the game is loaded, after init.lua\n"
"	-L		List the games (also --list): title, set, version, MAME\n"
"			sets, ROM files and CRC32s, those found under -R's path\n"
"	--write-dir=<path>\n"
"			Write all under path, as Supermodel and Flycast (also\n"
"			--write-dir <path>, or WRITE_DIR in hikaruloader.ini): the\n"
"			settings and the log at its root, each game's nvram, states\n"
"			and dumps in path/<game>/; the settings and nvram copied\n"
"			there when it has none, then read there only. Relative: to\n"
"			the program's folder\n"
"	-v		Set verbosity (affects debug messages)\n"
"	-h              Show this help\n";

static void
show_help (int argc, char **argv)
{
	hl::Log::write (stdout, global_help, argv[0]);
}

/* Supermodel's and sm2-emu's flags for the agent mode, which getopt () cannot
 * read */
static bool
is_agent_flag (const char *arg)
{
	return !strcmp (arg, "-a") || !strcmp (arg, "-agent") || !strcmp (arg, "--agent") ||
	       !strcmp (arg, "-agent-show") || !strcmp (arg, "--agent-show");
}

static int
parse_global_opts (int argc, char **argv)
{
	static char *args[64];
	int i, num = 0, opt, words;

	memset (&options, 0, sizeof (options));
	options.start_state = -1;

	for (i = 0; i < argc && num < (int) NUMELEM (args) - 1; i++) {
		if (i > 0 && is_agent_flag (argv[i]))
			options.agent = true;
		/* Taken at the start (resolve_write_dir) */
		else if (i > 0 && (words = hl::Paths::write_dir_option (argc, argv, i, NULL)))
			i += words - 1;
		else if (i > 0 && (!strcmp (argv[i], "--list") || !strcmp (argv[i], "-list")))
			options.list = true;
		else
			args[num++] = argv[i];
	}
	args[num] = NULL;

	while ((opt = getopt (num, args, global_opts)) != -1) {
		switch (opt) {
		case 'R':
			strncpy (options.rom_path, optarg, sizeof (options.rom_path) - 1);
			break;
		case 'r':
			strncpy (options.rom_name, optarg, 32);
			break;
		case 'n':
			options.num_frames = atoi (optarg);
			break;
		case 'l':
			if (strspn (optarg, "0123456789") == strlen (optarg))
				options.start_state = atoi (optarg);
			else
				strncpy (options.state_path, optarg, sizeof (options.state_path) - 1);
			break;
		case 's':
			options.shell_port = (unsigned) atoi (optarg);
			break;
		case 'x':
			strncpy (options.script, optarg, sizeof (options.script) - 1);
			break;
		case 'L':
			options.list = true;
			break;
		case 'v':
			hl_verbosity = 1;
			break;
		default:
			HL_ERROR ("unrecognized option '%c'", opt);
			/* fall-through */
		case 'h':
		case '?':
			show_help (argc, argv);
			return -1;
		}
	}
	return 0;
}

static void
finalize (void)
{
	HL_PRINT ("Finalizing");
	/* HR_PROFILE's times, for the frames run */
	if (emulator && emulator->mach)
		hl::Profile::report (emulator->mach->frame);
	hl::destroy (emulator);
	ui::Interface::shutdown ();
	hl::Log::close ();
}

/* The write directory (hl::Paths), before anything is written: --write-dir,
 * else the WRITE_DIR of the hikaruloader.ini beside the program. Its path,
 * empty without */
static std::string
resolve_write_dir (int argc, char **argv)
{
	std::string dir;
	int i;

	for (i = 1; i < argc; i++)
		hl::Paths::write_dir_option (argc, argv, i, &dir);
	if (dir.empty ()) {
		hl::Config usual;

		usual.set_defaults ();
		hl::Config::set_path (hl::Paths::config_read ("hikaruloader.ini").c_str ());
		if (usual.load ())
			dir = usual.write_dir;
	}
	return dir;
}

#ifdef _WIN32
/* On a PC with two GPUs (most gaming laptops), the drivers run a program on
 * the integrated one unless it asks for the other: these exports ask for the
 * high performance GPU, to NVIDIA's Optimus and AMD's PowerXpress drivers,
 * which look them up in the executable when it starts. A choice made in
 * Windows' graphics settings still wins */
extern "C" {
__declspec (dllexport) unsigned long NvOptimusEnablement = 1;
__declspec (dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

int
main (const int argc, char **argv)
{
	char *base = SDL_GetBasePath ();
	char path[1024], old_path[1024], rom_path[2048];
	std::string write_dir, ini_path, init_path;
	bool has_write_dir, loaded;
	hl::Config *cfg;
	bool has_init;
	FILE *fp;
	int i;

	if (!base)
		base = SDL_strdup ("./");
	hl::Paths::set_base (base);

	/* The agent's protocol has stdout, from the first message on */
	for (i = 1; i < argc; i++)
		if (is_agent_flag (argv[i]))
			hl::Log::stdout_reserved = true;

	write_dir = resolve_write_dir (argc, argv);
	has_write_dir = hl::Paths::set_write_dir (write_dir);
	/* The log, beside the program or at the write directory's root, the
	 * previous start's kept as hikaruloader.old.log; none when the
	 * settings in use (the write directory's when there) have LOG_FILE =
	 * false */
	{
		hl::Config settings;
		settings.set_defaults ();
		hl::Config::set_path (hl::Paths::config_read ("hikaruloader.ini").c_str ());
		settings.load ();
		snprintf (path, sizeof (path), "%s", hl::Paths::config_write ("hikaruloader.log").c_str ());
		snprintf (old_path, sizeof (old_path), "%s", hl::Paths::config_write ("hikaruloader.old.log").c_str ());
		if (settings.log_file)
			hl::Log::open (path, old_path);
	}
	hl::Profile::init ();

	HL_PRINT ("Hikaru Loader, Copyright (C) 2026 Benjamin Maurin (njz3)");
	HL_PRINT ("Version %s. Released under the GNU GPL v3 or later.", HL_VERSION_FULL);
	if (HL_VERSION_DEV)
		HL_PRINT (" ** Warning: this is a development snapshot, not a release. **");

	if (!write_dir.empty ()) {
		if (has_write_dir)
			HL_PRINT ("write directory: %s", hl::Paths::write_dir ().c_str ());
		else
			HL_ERROR ("cannot write to the write directory '%s' (%s): nothing is saved; "
			          "the settings and the games' saves are read from the usual folders",
			          hl::Paths::unusable_write_dir ().c_str (),
			          hl::Paths::write_dir_error ().c_str ());
	}

	if (parse_global_opts (argc, argv))
		goto fail;

	/* With a write directory, its copy, made from the one beside the
	 * program when it has none; that one read, never written, when the
	 * write directory is rejected */
	ini_path = hl::Paths::config_file ("hikaruloader.ini");
	hl::Config::set_path (hl::Paths::config_read ("hikaruloader.ini").c_str ());
	cfg = hl::Config::get ();
	loaded = cfg->load ();
	if (loaded)
		HL_PRINT ("loaded the settings from '%s'", hl::Config::path ());
	hl::Config::set_path (ini_path.c_str ());
	/* None yet: the defaults, each with its comment, before the command
	 * line changes any */
	if (!loaded && cfg->save ())
		HL_PRINT ("wrote the default settings to '%s'", ini_path.c_str ());
	/* -R stands for the games folder of the settings */
	if (options.rom_path[0])
		snprintf (cfg->games_dir, sizeof (cfg->games_dir), "%s", options.rom_path);
	cfg->games_dir_in_use (rom_path, sizeof (rom_path));
	/* The agent's frames go as fast as they can; not saved */
	if (options.agent)
		cfg->vsync = false;

	emulator = new hl::Emulator ();
	if (!emulator->load_game_list (base)) {
		HL_ERROR ("failed to load the game list");
		goto fail;
	}

	/* -L: the list, and no window */
	if (options.list) {
		emulator->game_list->print (stdout, rom_path);
		hl::destroy (emulator);
		hl::Log::close ();
		return 0;
	}

	if (hl::Window::open (640 * cfg->window_scale, 480 * cfg->window_scale))
		goto fail;
	hl::Pointers::init (hl::Window::sdl_window ());
	if (!ui::Interface::init (hl::Window::sdl_window ()))
		HL_ERROR ("could not start the interface");
	hl::Window::set_overlay (ui::Interface::draw_overlay);
	hl::Window::set_overlay_wanted (ui::Interface::overlay_wanted);

	/* controls.ini beside hikaruloader.ini, once SDL sees the controllers;
	 * in agent mode, read for the settings page only: the agent alone
	 * drives the game */
	hl::Controls::get ().set_enabled (!options.agent);
	ini_path = hl::Paths::config_file ("controls.ini");
	hl::Controls::get ().load (hl::Paths::config_read ("controls.ini").c_str (), ini_path.c_str ());

	atexit (finalize);
	//signal (SIGINT,  finalize);
	//signal (SIGKILL, finalize);

	if (!emulator->load_game (options.rom_name, path))
		goto fail;

next_game:
	emulator->mach->reset (hl::RESET_HARD);

	if (options.state_path[0])
		emulator->load_state (options.state_path);
	else if (options.start_state >= 0) {
		emulator->state_slot = (unsigned) options.start_state % hl::Emulator::NUM_STATE_SLOTS;
		emulator->load_or_save_state (true);
	}

	/* The Lua shell: for its port, init.lua beside the program, -x, and
	 * the agent's lua command */
	init_path = hl::Paths::config_read ("init.lua");
	fp = fopen (init_path.c_str (), "rb");
	has_init = fp != NULL;
	if (fp)
		fclose (fp);
	if (!emulator->shell &&
	    (options.shell_port || has_init || options.script[0] || options.agent)) {
		if (!emulator->open_shell (options.shell_port))
			goto fail;
		if (has_init)
			emulator->shell->run_file (init_path.c_str ());
		if (options.script[0])
			emulator->shell->run_file (options.script);
	}
	if (options.agent)
		emulator->open_agent ();

	SDL_ShowCursor (cfg->hide_cursor ? SDL_DISABLE : SDL_ENABLE);
	HL_PRINT ("Running");
	emulator->run (options.num_frames);

	/* Escape in a game from the picker: the picker again, then its next
	 * game; the states and options of the command line were the first's */
	if (emulator->back_to_picker) {
		emulator->unload_game ();
		options.state_path[0] = '\0';
		options.start_state = -1;
		if (emulator->load_game ("", path))
			goto next_game;
	}

fail:
	SDL_free (base);
	return 0;
}
