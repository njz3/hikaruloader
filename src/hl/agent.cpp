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

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "hl/agent.h"
#include "hl/emulator.h"
#include "hl/shell.h"
#include "hl/controls.h"
#include "hl/cpu.h"
#include "hl/ffb.h"
#include "hl/gearbox.h"
#include "hl/input.h"
#include "hl/telemetry.h"
#include "hl/window.h"

namespace hl {

/* The lines read from stdin, and its end */
struct Agent::Queue {
	std::mutex		 lock;
	std::condition_variable	 ready;
	std::deque<std::string>	 lines;
	bool			 eof = false;
};

/* The machine's picture: "ready" and obs's default size */
static const int frame_width = 640, frame_height = 480;

/* The inputs the machine reads: the cabinet's switches, by the names
 * sm2-emu gives them */
static const struct {
	const char *name;
	Action action;
} inputs[] = {
	{ "Test",	ACTION_TEST },
	{ "Service",	ACTION_SERVICE },
	{ "Coin1",	ACTION_COIN1 },
	{ "Coin2",	ACTION_COIN2 },
	{ "Start1",	ACTION_START1 },
	{ "Up",		ACTION_UP },
	{ "Down",	ACTION_DOWN },
	{ "Left",	ACTION_LEFT },
	{ "Right",	ACTION_RIGHT },
	{ "Button1",	ACTION_BUTTON1 },
	{ "Button2",	ACTION_BUTTON2 },
	{ "Button3",	ACTION_BUTTON3 },
	{ "Button4",	ACTION_BUTTON4 },
	{ "Card",	ACTION_CARD },
	/* linuxloader's name */
	{ "ViewChange",	ACTION_VIEW_CHANGE },
	/* The Gaelco cabinets' own (controls.ini's [Gaelco]) */
	{ "VolumeUp",	ACTION_VOLUME_UP },
	{ "VolumeDown",	ACTION_VOLUME_DOWN },
	{ "LeftBrake",	ACTION_LEFT_BRAKE },
	{ "RightBrake",	ACTION_RIGHT_BRAKE },
	{ "Horn",	ACTION_HORN },
};

/* The analog controls, as sm2-emu and Supermodel name them: the wheel from
 * -1 (left) to 1, the pedals and Star Wars: Racer Arcade's levers from 0 to
 * 1; held over controls.ini's rows (hl::Controls::hold) */
static const struct {
	const char *name;
	const char *section, *key;
	bool axis;
} analogs[] = {
	{ "Steering",	"Driving",	"P1_Steer",	 true },
	{ "Accelerator", "Driving",	"P1_Gas",	 false },
	{ "Brake",	"Driving",	"P1_Brake",	 false },
	{ "LeftLever",	"Driving",	"P1_LeftLever",	 false },
	{ "RightLever",	"Driving",	"P1_RightLever", false },
};

static int
find_analog (const char *name)
{
	unsigned i;

	for (i = 0; i < NUMELEM (analogs); i++)
		if (!strcasecmp (analogs[i].name, name))
			return (int) i;
	return -1;
}

static int
find_input (const char *name)
{
	unsigned i;

	for (i = 0; i < NUMELEM (inputs); i++)
		if (!strcasecmp (inputs[i].name, name))
			return (int) i;
	return -1;
}

Agent::Agent (Emulator *emu) :
	emu (emu),
	queue (std::make_shared<Queue> ()),
	settle_ms (0)
{
}

/* Up to size bytes of stdin, as they come: 0 at its end. Not through the C
 * library's stdin: a thread blocked in fgets () holds stdin's lock, and the
 * program's exit, which takes it, waited for the client to close stdin (the
 * static runtime of the releases, /MT) */
static size_t
read_stdin (char *buf, size_t size)
{
#ifdef _WIN32
	DWORD got = 0;

	if (!ReadFile (GetStdHandle (STD_INPUT_HANDLE), buf, (DWORD) size, &got, NULL))
		return 0;
	return got;
#else
	ssize_t got = read (0, buf, size);

	return (got > 0) ? (size_t) got : 0;
#endif
}

/* The reading thread keeps its queue, and ends with the program */
Agent::~Agent ()
{
}

void
Agent::start ()
{
	std::shared_ptr<Queue> q = queue;

#ifdef _WIN32
	/* Lines end in "\n" alone, and the pictures go out as they are */
	_setmode (_fileno (stdout), _O_BINARY);
#endif
	Log::stdout_reserved = true;

	std::thread ([q] {
		std::string line;
		char buf[4096];
		size_t got, i;

		while ((got = read_stdin (buf, sizeof (buf))) > 0)
			for (i = 0; i < got; i++) {
				if (buf[i] != '\n') {
					line += buf[i];
					continue;
				}
				if (!line.empty () && line.back () == '\r')
					line.pop_back ();
				{
					std::lock_guard<std::mutex> hold (q->lock);
					q->lines.push_back (line);
				}
				q->ready.notify_one ();
				line.clear ();
			}
		{
			std::lock_guard<std::mutex> hold (q->lock);
			if (!line.empty ())
				q->lines.push_back (line);
			q->eof = true;
		}
		q->ready.notify_one ();
	}).detach ();

	HL_PRINT ("agent: reading stdin");
	reply ("ready %s %d %d", emu->game->name, frame_width, frame_height);
}

bool
Agent::poll (unsigned timeout_ms)
{
	std::deque<std::string> lines;
	bool eof;

	{
		std::unique_lock<std::mutex> hold (queue->lock);
		queue->ready.wait_for (hold, std::chrono::milliseconds (timeout_ms),
		                       [this] { return !queue->lines.empty () || queue->eof; });
		lines.swap (queue->lines);
		eof = queue->eof;
	}
	for (const std::string &line : lines) {
		if (emu->quit)
			break;
		try {
			run (line);
		} catch (const Abort &e) {
			/* A state loaded or a reset that failed */
			emu->aborted (e.what ());
			reply ("err %s", emu->stop_message);
		}
	}
	if (eof && !emu->quit) {
		HL_PRINT ("agent: end of stdin, quitting");
		emu->quit = true;
	}
	return !lines.empty () || eof;
}

void
Agent::reply (const char *fmt, ...)
{
	va_list va;

	va_start (va, fmt);
	vfprintf (stdout, fmt, va);
	va_end (va);
	fputc ('\n', stdout);
	fflush (stdout);
}

void
Agent::reply_data (const char *header, const void *data, size_t size)
{
	fputs (header, stdout);
	fputc ('\n', stdout);
	if (size)
		fwrite (data, 1, size, stdout);
	fflush (stdout);
}

/* step [N]: "ok FRAME"; Hikaruloader has no drive board, so no bytes follow.
 * A breakpoint or an abort answers "err <why>" */
void
Agent::step (const char *args)
{
	long i, num = *args ? strtol (args, NULL, 10) : 1;

	if (num <= 0) {
		reply ("err usage: step [N]");
		return;
	}
	for (i = 0; i < num; i++) {
		if (settle_ms)
			SDL_Delay (settle_ms);
		if (emu->step (1) < 0 || emu->last_stop != Emulator::STOP_NONE) {
			reply ("err %s", emu->stop_message);
			return;
		}
	}
	reply ("ok %llu", (unsigned long long) emu->mach->frame);
}

/* obs [W H]: the picture box-scaled to W x H (640 x 480), RGB, top row
 * first */
void
Agent::obs (const char *args)
{
	std::vector<uint8_t> rgb, scaled;
	int w = 0, h = 0, sw, sh, x, y;
	char header[64];

	if (*args && sscanf (args, "%d %d", &w, &h) != 2) {
		reply ("err usage: obs [W H]");
		return;
	}
	if (w <= 0 || h <= 0) {
		w = frame_width;
		h = frame_height;
	}
	if (w > 4096 || h > 4096) {
		reply ("err obs: at most 4096 x 4096");
		return;
	}
	if (!Window::read_frame (rgb, &sw, &sh)) {
		reply ("err no frame yet");
		return;
	}

	scaled.resize ((size_t) w * h * 3);
	for (y = 0; y < h; y++) {
		int y0 = y * sh / h, y1 = std::max ((y + 1) * sh / h, y0 + 1);
		for (x = 0; x < w; x++) {
			int x0 = x * sw / w, x1 = std::max ((x + 1) * sw / w, x0 + 1);
			unsigned sum[3] = { 0, 0, 0 }, n = (x1 - x0) * (y1 - y0), c;
			int u, v;

			for (v = y0; v < y1; v++)
				for (u = x0; u < x1; u++)
					for (c = 0; c < 3; c++)
						sum[c] += rgb[((size_t) v * sw + u) * 3 + c];
			for (c = 0; c < 3; c++)
				scaled[((size_t) y * w + x) * 3 + c] = (uint8_t) ((sum[c] + n / 2) / n);
		}
	}
	snprintf (header, sizeof (header), "obs %d %d", w, h);
	reply_data (header, scaled.data (), scaled.size ());
}

/* ram|mem ADDRESS LENGTH (hex): through the master SH-4's bus, as
 * Supermodel's; at most 0x100000 bytes, fewer when the bus stops answering.
 * "ram LENGTH" (decimal), then the bytes */
void
Agent::read_memory (const char *name, const char *args)
{
	Cpu *cpu = emu->mach->cpus.empty () ? NULL : emu->mach->cpus[0];
	std::vector<uint8_t> bytes;
	unsigned long i, length;
	char header[64], *end;
	uint32_t addr;

	addr = (uint32_t) strtoul (args, &end, 16);
	length = strtoul (end, NULL, 16);
	if (end == args || !cpu) {
		reply ("err usage: %s ADDRESS LENGTH", name);
		return;
	}
	length = std::min (length, 0x100000ul);

	bytes.reserve (length);
	emu->mach->set_current_cpu (cpu);
	try {
		for (i = 0; i < length; i++) {
			uint64_t val = 0;
			if (cpu->debug_read (1, addr + (uint32_t) i, &val))
				break;
			bytes.push_back ((uint8_t) val);
		}
	} catch (const Abort &) {
		/* An unhandled register: the bytes so far */
	}
	snprintf (header, sizeof (header), "%s %u", name, (unsigned) bytes.size ());
	reply_data (header, bytes.data (), bytes.size ());
}

/* lua CHUNK: "lua N", then the N lines it printed or returned */
void
Agent::lua (const char *chunk)
{
	std::string text = emu->shell->run (chunk);

	if (!text.empty () && text.back () != '\n')
		text += '\n';
	reply ("lua %u", (unsigned) std::count (text.begin (), text.end (), '\n'));
	fputs (text.c_str (), stdout);
	fflush (stdout);
}

void
Agent::run (const std::string &line)
{
	size_t space = line.find (' ');
	std::string word = line.substr (0, space);
	const char *cmd = word.c_str ();
	const char *args = (space == std::string::npos) ? "" : line.c_str () + space + 1;
	char name[64];
	double value;
	int input;

	while (*args == ' ')
		args++;
	if (word.empty ())
		return;

	if (!strcmp (cmd, "step"))
		step (args);
	else if ((!strcmp (cmd, "set") || !strcmp (cmd, "setraw")) &&
	         sscanf (args, "%63s %lf", name, &value) == 2 && !strcasecmp (name, "Gear") &&
	         Gearbox::get ().attached ()) {
		/* NASCAR Arcade's gear lever, as sm2-emu's: 0 (neutral) to 4 */
		if (value < 0 || value > 4)
			reply ("err usage: %s Gear 0..4", cmd);
		else {
			Gearbox::get ().hold ((int) value);
			reply ("ok");
		}
	} else if (!strcmp (cmd, "set") || !strcmp (cmd, "setraw")) {
		/* A switch: pressed from 0.5, or any raw value but 0 */
		if (sscanf (args, "%63s %lf", name, &value) != 2)
			reply ("err usage: %s INPUT VALUE", cmd);
		else if ((input = find_analog (name)) >= 0) {
			Controls::get ().hold (analogs[input].section, analogs[input].key, value);
			reply ("ok");
		} else if ((input = find_input (name)) < 0)
			reply ("err unknown input %s", name);
		else {
			Input::set_action (inputs[input].action,
			                   strcmp (cmd, "set") ? value != 0.0 : value >= 0.5);
			reply ("ok");
		}
	} else if (!strcmp (cmd, "get")) {
		if (!strcasecmp (args, "Gear") && Gearbox::get ().attached ())
			reply ("value %u", Gearbox::get ().gear (emu->mach->frame));
		else if ((input = find_analog (args)) >= 0)
			reply ("value %.4f", analogs[input].axis ?
			       Controls::get ().axis (analogs[input].section, analogs[input].key) :
			       Controls::get ().pedal (analogs[input].section, analogs[input].key));
		else if ((input = find_input (args)) < 0)
			reply ("err unknown input %s", args);
		else
			reply ("value %d", Input::action (inputs[input].action) ? 1 : 0);
	} else if (!strcmp (cmd, "release")) {
		if (!strcmp (args, "all")) {
			for (const auto &in : inputs)
				Input::set_action (in.action, false);
			Gearbox::get ().hold (-1);
			Controls::get ().release_held (NULL, NULL);
			reply ("ok");
		} else if (!strcasecmp (args, "Gear") && Gearbox::get ().attached ()) {
			Gearbox::get ().hold (-1);
			reply ("ok");
		} else if ((input = find_analog (args)) >= 0) {
			Controls::get ().release_held (analogs[input].section, analogs[input].key);
			reply ("ok");
		} else if ((input = find_input (args)) < 0)
			reply ("err unknown input %s", args);
		else {
			Input::set_action (inputs[input].action, false);
			reply ("ok");
		}
	} else if (!strcmp (cmd, "inputs")) {
		reply ("inputs %u", (unsigned) (NUMELEM (analogs) + NUMELEM (inputs)) +
		                    (Gearbox::get ().attached () ? 1 : 0));
		for (const auto &a : analogs)
			reply ("%s %s", a.name, a.axis ? "axis" : "analog");
		for (const auto &in : inputs)
			reply ("%s switch", in.name);
		if (Gearbox::get ().attached ())
			reply ("Gear other");
	} else if (!strcmp (cmd, "obs"))
		obs (args);
	else if (!strcmp (cmd, "shot")) {
		if (*args && Window::screenshot (args))
			reply ("ok");
		else
			reply ("err cannot save %s", args);
	} else if (!strcmp (cmd, "save")) {
		if (*args && !emu->save_state (args))
			reply ("ok");
		else
			reply ("err cannot save %s", args);
	} else if (!strcmp (cmd, "load")) {
		if (*args && !emu->load_state (args))
			reply ("ok");
		else
			reply ("err cannot load %s", args);
	} else if (!strcmp (cmd, "state")) {
		/* F4, F1, F2 and F3 */
		int ret = 0;

		if (!strcmp (args, "save"))
			ret = emu->load_or_save_state (false);
		else if (!strcmp (args, "load"))
			ret = emu->load_or_save_state (true);
		else if (!strcmp (args, "newer"))
			emu->select_state (-1);
		else if (!strcmp (args, "older"))
			emu->select_state (1);
		else if (*args)
			ret = -1;
		if (ret)
			reply ("err state %s", args);
		else
			reply ("state %u", emu->state_slot);
	} else if (!strcmp (cmd, "ffb")) {
		/* The effects since the last call: FRAME INPUT EFFECT FORCE */
		std::vector<ForceFeedback::Record> records = ForceFeedback::take_records ();
		reply ("ffb %u", (unsigned) records.size ());
		for (const ForceFeedback::Record &r : records)
			reply ("%llu Steering %s %.4f", (unsigned long long) r.frame,
			       ForceFeedback::effect_name (r.effect), r.force);
	}
	else if (!strcmp (cmd, "servo")) {
		if (!*args)
			reply ("servo 0");
		else if (!strcmp (args, "on") || !strcmp (args, "off"))
			reply ("ok");
		else
			reply ("err usage: servo [on|off]");
	} else if (!strcmp (cmd, "frame"))
		reply ("frame %llu", (unsigned long long) emu->mach->frame);
	else if (!strcmp (cmd, "settle")) {
		if (*args)
			settle_ms = (unsigned) std::clamp (atoi (args), 0, 1000);
		reply ("settle %u", settle_ms);
	} else if (!strcmp (cmd, "tel")) {
		/* The racing game's telemetry of the last frame, one JSON line
		 * (hl/telemetry.h), as sm2-emu's and Supermodel's tel; "tel none"
		 * for a game without a decoder */
		static telemetry::Selection selection;
		std::string what (args), arg;
		size_t space = what.find (' ');

		if (space != std::string::npos) {
			arg = what.substr (space + 1);
			what.resize (space);
		}
		if (what.empty ())
			reply ("%s", telemetry::Service::get ().json (selection).c_str ());
		else if (what == "fields") {
			selection.fields.clear ();
			if (arg != "all") {
				size_t start = 0;
				while (start <= arg.size ()) {
					size_t comma = arg.find (',', start);
					std::string f = arg.substr (start, comma == std::string::npos ? std::string::npos : comma - start);
					f.erase (std::remove_if (f.begin (), f.end (), [] (char c) { return c == ' ' || c == '\t'; }), f.end ());
					if (!f.empty ())
						selection.fields.push_back (f);
					if (comma == std::string::npos)
						break;
					start = comma + 1;
				}
			}
			reply ("ok");
		} else if (what == "others") {
			selection.others = arg == "all" ? -1 : atoi (arg.c_str ());
			reply ("ok");
		} else
			reply ("err usage: tel | tel fields LIST|all | tel others N|all");
	} else if (!strcmp (cmd, "ram") || !strcmp (cmd, "mem"))
		read_memory (cmd, args);
	else if (!strcmp (cmd, "poke")) {
		/* poke ADDRESS VALUE [SIZE] (hex), through the master SH-4's bus */
		Cpu *cpu = emu->mach->cpus.empty () ? NULL : emu->mach->cpus[0];
		unsigned addr, val, size = 4;
		int ret = -1;

		if (!cpu || sscanf (args, "%x %x %u", &addr, &val, &size) < 2 ||
		    (size != 1 && size != 2 && size != 4))
			reply ("err usage: poke ADDRESS VALUE [1|2|4]");
		else {
			emu->mach->set_current_cpu (cpu);
			try {
				ret = cpu->debug_write (size, addr, val);
			} catch (const Abort &) {
			}
			if (ret)
				reply ("err cannot write at %08X", addr);
			else
				reply ("ok");
		}
	} else if (!strcmp (cmd, "lua"))
		lua (args);
	else if (!strcmp (cmd, "quit")) {
		reply ("bye");
		emu->quit = true;
	} else
		reply ("err unknown command");
}

} /* namespace hl */
