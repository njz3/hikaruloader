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
#include <array>
#include <chrono>
#include <map>

#include "hl/core.h"
#include "hl/profile.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <atomic>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#endif

namespace hl {

bool Profile::enabled = false;
bool Profile::sampling = false;
volatile int Profile::current = -1;
uint64_t Profile::total[NUM_SECTIONS];
uint64_t Profile::calls[NUM_SECTIONS];
uint64_t Profile::units[NUM_SECTIONS];

/* The time stamp counter's rate, from the wall clock between init and report */
static uint64_t start_ticks;
static std::chrono::steady_clock::time_point start_time;

static const struct {
	const char *name;
	int parent;		/* -1: none */
	const char *unit;	/* of count (), NULL: none */
} sections[Profile::NUM_SECTIONS] = {
	{ "frame",		-1,			NULL },
	{ "machine",		Profile::FRAME,		NULL },
	{ "SH-4 master",	Profile::MACHINE,	NULL },
	{ "SH-4 slave",		Profile::MACHINE,	NULL },
	{ "MEMCTL",		Profile::MACHINE,	NULL },
	{ "GPU: CP",		Profile::MACHINE,	NULL },
	{ "GPU: events",	Profile::MACHINE,	NULL },
	{ "MIE, JVS",		Profile::MACHINE,	NULL },
	{ "AICAs",		Profile::MACHINE,	NULL },
	{ "ARM7s",		Profile::AICA,		"ARM cycle" },
	{ "voices",		Profile::AICA,		"channel-sample" },
	{ "DSPs",		Profile::AICA,		"step" },
	{ "comm board",		Profile::MACHINE,	NULL },
	{ "renderer: begin",	Profile::FRAME,		NULL },
	{ "renderer: draw",	Profile::FRAME,		NULL },
	{ "thread awaited",	Profile::RENDER_END,	NULL },
	{ "frame handed over",	Profile::RENDER_END,	NULL },
	{ "frame shown",	Profile::RENDER_END,	NULL },
	{ "interface, swap",	Profile::RENDER_END,	NULL },
	{ "sound output",	Profile::FRAME,		NULL },
};

/* HR_SAMPLE=1 (Windows): where the thread that runs the machine is, a
 * sample every millisecond or so (another thread suspends it, reads its
 * RIP, lets it go, then counts it: nothing allocated while it is held); at
 * the end, to the log, the busiest functions by their names (the program's
 * PDB, through dbghelp), a module's code by its name, the rest the SH-4s'
 * translations */
#ifdef _WIN32
static std::atomic<bool> sampling (false);
static std::thread sampler;
static HANDLE sampled = NULL;
static std::unordered_map<uint64_t, uint64_t> samples;
/* With each address outside the program (a DLL's, a translation's): the
 * first returns into the program found on the stack, its callers there (an
 * index in chains: operator new's, then its caller's...) */
static std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint64_t>> tops;
static uint64_t program_lo, program_hi;
typedef std::array<uint64_t, 3> Chain;
static std::vector<Chain> chains;
static std::map<Chain, uint64_t> chain_index;

/* The translations, by their start: their end, CPU, block */
struct Written {
	uint64_t	end;
	char		cpu;
	uint32_t	pc;
	std::vector<uint16_t> words;
};
static std::map<uint64_t, Written> written;

/* The machine's thread's samples in the SH-4s' sections (Profile::current) */
static std::unordered_map<uint64_t, uint64_t> master_samples, slave_samples;

/* Another thread sampled (sample_this_thread ()), its samples apart */
static std::atomic<HANDLE> other (NULL);
static std::unordered_map<uint64_t, uint64_t> other_samples;
static std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint64_t>> other_tops;

/* One sample of thread: its RIP and, outside the program, its caller there
 * (the stack read while it is held, without a lock: ReadProcessMemory
 * fails on a page past the stack's base, then a shorter read) */
static void
sample_one (HANDLE thread, std::unordered_map<uint64_t, uint64_t> &at,
            std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint64_t>> &top_at,
            bool sections = false)
{
	CONTEXT c;
	Chain chain = {};
	uint64_t top = 0, stack[256];
	unsigned found = 0;
	SIZE_T words = 0;
	bool got;
	int section;

	if (SuspendThread (thread) == (DWORD) -1)
		return;
	section = Profile::current;
	memset (&c, 0, sizeof (c));
	c.ContextFlags = CONTEXT_CONTROL;
	got = GetThreadContext (thread, &c) != 0;
	if (got && c.Rsp && (c.Rip < program_lo || c.Rip >= program_hi))
		for (SIZE_T size = sizeof (stack); size >= 64 && !words; size /= 4)
			if (ReadProcessMemory (GetCurrentProcess (), (LPCVOID) c.Rsp, stack, size, NULL))
				words = size / sizeof (stack[0]);
	ResumeThread (thread);
	for (SIZE_T i = 0; i < words && found < chain.size (); i++)
		if (stack[i] >= program_lo && stack[i] < program_hi)
			chain[found++] = stack[i];
	if (found) {
		auto it = chain_index.find (chain);

		if (it == chain_index.end ()) {
			it = chain_index.emplace (chain, chains.size ()).first;
			chains.push_back (chain);
		}
		top = it->second + 1;
	}
	if (got) {
		at[c.Rip]++;
		top_at[c.Rip][top]++;
		if (sections && section == Profile::SH4_MASTER)
			master_samples[c.Rip]++;
		else if (sections && section == Profile::SH4_SLAVE)
			slave_samples[c.Rip]++;
	}
}

static void
sample_loop ()
{
	while (sampling) {
		HANDLE o = other.load ();

		Sleep (1);
		sample_one (sampled, samples, tops, true);
		if (o)
			sample_one (o, other_samples, other_tops);
	}
}

void
Profile::sample_this_thread ()
{
	HANDLE h = NULL;

	if (!sampling)
		return;
	DuplicateHandle (GetCurrentProcess (), GetCurrentThread (), GetCurrentProcess (), &h,
	                 THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
	other.store (h);
}

/* A section's samples: its busiest functions, the translations as one */
static void
report_section (HANDLE process, const char *title,
                const std::unordered_map<uint64_t, uint64_t> &at)
{
	std::unordered_map<std::string, uint64_t> by_name;
	std::vector<std::pair<uint64_t, std::string>> busy;
	uint64_t all = 0;
	size_t i;

	if (at.empty ())
		return;
	for (auto &kv : at) {
		char buf[sizeof (SYMBOL_INFO) + 256];
		SYMBOL_INFO *sym = (SYMBOL_INFO *) buf;
		DWORD64 disp = 0;
		HMODULE module = NULL;
		std::string name;

		memset (buf, 0, sizeof (buf));
		sym->SizeOfStruct = sizeof (SYMBOL_INFO);
		sym->MaxNameLen = 255;
		if (SymFromAddr (process, kv.first, &disp, sym))
			name = sym->Name;
		else if (GetModuleHandleExA (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                             (LPCSTR) kv.first, &module))
			name = "(a DLL)";
		else
			name = "(the SH-4s' translations)";
		by_name[name] += kv.second;
		all += kv.second;
	}
	for (auto &kv : by_name)
		busy.push_back (std::make_pair (kv.second, kv.first));
	std::sort (busy.begin (), busy.end (), std::greater<> ());
	HL_PRINT ("samples: %llu in %s, the busiest:", (unsigned long long) all, title);
	for (i = 0; i < busy.size () && i < 32; i++)
		HL_PRINT ("  %5.1f%%  %s", 100.0 * (double) busy[i].first / (double) (all ? all : 1),
		          busy[i].second.c_str ());
}

/* The other thread's samples: its busiest functions */
static void
report_other (HANDLE process)
{
	std::unordered_map<std::string, uint64_t> by_name;
	std::vector<std::pair<uint64_t, std::string>> busy;
	uint64_t all = 0;
	size_t i;

	for (auto &kv : other_samples) {
		char buf[sizeof (SYMBOL_INFO) + 256];
		SYMBOL_INFO *sym = (SYMBOL_INFO *) buf;
		DWORD64 disp = 0;
		HMODULE module = NULL;
		std::string name;

		memset (buf, 0, sizeof (buf));
		sym->SizeOfStruct = sizeof (SYMBOL_INFO);
		sym->MaxNameLen = 255;
		if (SymFromAddr (process, kv.first, &disp, sym))
			name = sym->Name;
		else if (GetModuleHandleExA (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                             (LPCSTR) kv.first, &module)) {
			char path[MAX_PATH] = "";
			const char *file;

			GetModuleFileNameA (module, path, MAX_PATH);
			file = strrchr (path, '\\');
			name = std::string ("(") + (file ? file + 1 : path) + ")";
		} else
			name = "(unknown)";
		by_name[name] += kv.second;
		all += kv.second;
	}
	for (auto &kv : by_name)
		if (kv.second)
			busy.push_back (std::make_pair (kv.second, kv.first));
	std::sort (busy.begin (), busy.end (), std::greater<> ());
	HL_PRINT ("samples: %llu of the drawing thread, the busiest:", (unsigned long long) all);
	for (i = 0; i < busy.size () && i < (size_t) hl_util_get_int_option ("HR_SAMPLE_TOP", 48); i++)
		HL_PRINT ("  %5.1f%%  %s", 100.0 * (double) busy[i].first / (double) (all ? all : 1),
		          busy[i].second.c_str ());
}

static void
sample_start ()
{
	if (!hl_util_get_bool_option ("HR_SAMPLE", false))
		return;
	DuplicateHandle (GetCurrentProcess (), GetCurrentThread (), GetCurrentProcess (), &sampled,
	                 THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
	{
		const uint8_t *base = (const uint8_t *) GetModuleHandleA (NULL);
		const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *) (base + ((const IMAGE_DOS_HEADER *) base)->e_lfanew);

		program_lo = (uint64_t) base;
		program_hi = program_lo + nt->OptionalHeader.SizeOfImage;
	}
	sampling = true;
	Profile::sampling = true;
	sampler = std::thread (sample_loop);
}

static void
sample_report ()
{
	HANDLE process = GetCurrentProcess ();
	HMODULE program = GetModuleHandleA (NULL);
	std::unordered_map<std::string, uint64_t> by_name, by_block, by_caller;
	/* Each block's start, its samples by address (HR_SAMPLE_DUMP) */
	std::unordered_map<std::string, uint64_t> block_start;
	std::unordered_map<std::string, std::map<uint64_t, uint64_t>> block_at;
	std::vector<std::pair<uint64_t, std::string>> busy, blocks, callers;
	uint64_t all = 0, translated = 0;
	size_t i;

	if (!sampling)
		return;
	sampling = false;
	sampler.join ();
	SymSetOptions (SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
	SymInitialize (process, NULL, TRUE);
	for (auto &kv : samples) {
		char buf[sizeof (SYMBOL_INFO) + 256];
		SYMBOL_INFO *sym = (SYMBOL_INFO *) buf;
		DWORD64 disp = 0;
		HMODULE module = NULL;
		std::string name;

		memset (buf, 0, sizeof (buf));
		sym->SizeOfStruct = sizeof (SYMBOL_INFO);
		sym->MaxNameLen = 255;
		GetModuleHandleExA (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR) kv.first, &module);
		if (SymFromAddr (process, kv.first, &disp, sym)) {
			name = sym->Name;
			/* Where in the program, for the disassembly (dumpbin
			 * /disasm /range) */
			if (hl_util_get_bool_option ("HR_SAMPLE_AT", false)) {
				char at[64];

				snprintf (at, sizeof (at), " +%llX (RVA %llX)", (unsigned long long) disp,
				          (unsigned long long) (kv.first - sym->ModBase));
				name += at;
			}
			/* Far past its symbol: code the PDB does not name */
			if (disp > 0x4000) {
				char at[64];

				snprintf (at, sizeof (at), " +%llX", (unsigned long long) disp & ~0xFFFull);
				name += at;
			}
		}
		else if (GetModuleHandleExA (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                             (LPCSTR) kv.first, &module)) {
			char path[MAX_PATH] = "";
			const char *file;

			GetModuleFileNameA (module, path, MAX_PATH);
			file = strrchr (path, '\\');
			name = std::string ("(") + (file ? file + 1 : path) + ")";
		} else {
			auto w = written.upper_bound (kv.first);

			name = "(the SH-4s' translations)";
			translated += kv.second;
			if (w != written.begin () && kv.first < (--w)->second.end) {
				char at[64];
				std::string label;

				snprintf (at, sizeof (at), "%c @%08X:", w->second.cpu, w->second.pc);
				label = at;
				for (uint16_t word : w->second.words) {
					snprintf (at, sizeof (at), " %04X", word);
					label += at;
				}
				by_block[label] += kv.second;
				block_start[label] = w->first;
				block_at[label][kv.first] += kv.second;
			}
		}
		by_name[name] += kv.second;
		all += kv.second;
		/* In a DLL: its callers in the program, from the stack */
		if (module && module != program)
			for (auto &t : tops[kv.first]) {
				std::string label = name;

				for (uint64_t ret : t.first ? chains[t.first - 1] : Chain ()) {
					char cbuf[sizeof (SYMBOL_INFO) + 256];
					SYMBOL_INFO *csym = (SYMBOL_INFO *) cbuf;
					DWORD64 cdisp = 0;

					if (!ret)
						break;
					memset (cbuf, 0, sizeof (cbuf));
					csym->SizeOfStruct = sizeof (SYMBOL_INFO);
					csym->MaxNameLen = 255;
					label += " <- " + (SymFromAddr (process, ret, &cdisp, csym) ?
					                   std::string (csym->Name) : std::string ("?"));
				}
				by_caller[label] += t.second;
			}
	}
	if (other.load ())
		report_other (process);
	report_section (process, "the master SH-4", master_samples);
	report_section (process, "the slave SH-4", slave_samples);
	SymCleanup (process);
	for (auto &kv : by_name)
		busy.push_back (std::make_pair (kv.second, kv.first));
	std::sort (busy.begin (), busy.end (), std::greater<> ());
	HL_PRINT ("samples: %llu of the machine's thread, the busiest:", (unsigned long long) all);
	for (i = 0; i < busy.size () && i < (size_t) hl_util_get_int_option ("HR_SAMPLE_TOP", 48); i++)
		HL_PRINT ("  %5.1f%%  %s", 100.0 * (double) busy[i].first / (double) (all ? all : 1),
		          busy[i].second.c_str ());
	for (auto &kv : by_block)
		blocks.push_back (std::make_pair (kv.second, kv.first));
	std::sort (blocks.begin (), blocks.end (), std::greater<> ());
	for (auto &kv : by_caller)
		callers.push_back (std::make_pair (kv.second, kv.first));
	std::sort (callers.begin (), callers.end (), std::greater<> ());
	HL_PRINT ("samples: in the DLLs, by their callers in the program (the first returns into it on the stack):");
	for (i = 0; i < callers.size () && i < (size_t) hl_util_get_int_option ("HR_SAMPLE_TOP", 48) / 2; i++)
		HL_PRINT ("  %5.2f%%  %s", 100.0 * (double) callers[i].first / (double) (all ? all : 1),
		          callers[i].second.c_str ());
	HL_PRINT ("samples: the busiest translated blocks, of all the samples (their words):");
	for (i = 0; i < blocks.size () && i < 40; i++)
		HL_PRINT ("  %5.2f%%  %s", 100.0 * (double) blocks[i].first / (double) (all ? all : 1),
		          blocks[i].second.c_str ());

	/* HR_SAMPLE_DUMP=dir: the busiest blocks' host code, block<rank>.bin,
	 * and its samples by offset, block<rank>.txt (the first line, the
	 * block's words) */
	if (const char *dir = getenv ("HR_SAMPLE_DUMP"))
		for (i = 0; i < blocks.size () && i < 40; i++) {
			const std::string &label = blocks[i].second;
			uint64_t start = block_start[label];
			auto w = written.find (start);
			char path[1024];
			FILE *f;

			if (w == written.end ())
				continue;
			snprintf (path, sizeof (path), "%s/block%02u.bin", dir, (unsigned) i);
			if ((f = fopen (path, "wb")) != NULL) {
				fwrite ((const void *) (uintptr_t) start, 1, (size_t) (w->second.end - start), f);
				fclose (f);
			}
			snprintf (path, sizeof (path), "%s/block%02u.txt", dir, (unsigned) i);
			if ((f = fopen (path, "w")) != NULL) {
				fprintf (f, "%s\n", label.c_str ());
				for (auto &at : block_at[label])
					fprintf (f, "%llX %llu\n", (unsigned long long) (at.first - start),
					         (unsigned long long) at.second);
				fclose (f);
			}
		}
}
#endif

void
Profile::code_written (const void *start, size_t size, char cpu, uint32_t pc, const uint16_t *words,
                       unsigned num_words)
{
#ifdef _WIN32
	uint64_t a = (uint64_t) (uintptr_t) start, b = a + size;
	auto it = written.lower_bound (a);

	/* What it overwrites (the arena started again) goes */
	while (it != written.end () && it->first < b)
		it = written.erase (it);
	it = written.lower_bound (a);
	if (it != written.begin () && (--it)->second.end > a)
		written.erase (it);
	written[a] = Written { b, cpu, pc, std::vector<uint16_t> (words, words + num_words) };
#else
	(void) start; (void) size; (void) cpu; (void) pc; (void) words; (void) num_words;
#endif
}

/* The frames' own CPU time, in the counter's ticks */
static uint64_t frame_cpu_total, frame_cpu_start;

void
Profile::frame_cpu (bool start)
{
#ifdef _WIN32
	ULONG64 cycles = 0;

	QueryThreadCycleTime (GetCurrentThread (), &cycles);
	if (start)
		frame_cpu_start = cycles;
	else
		frame_cpu_total += cycles - frame_cpu_start;
#else
	(void) start;
#endif
}

void
Profile::init ()
{
	enabled = hl_util_get_bool_option ("HR_PROFILE", false);
	start_ticks = now ();
	start_time = std::chrono::steady_clock::now ();
#ifdef _WIN32
	sample_start ();
#endif
}

/* A section's own time: its total less its children's */
static uint64_t
own (const uint64_t *total, int s)
{
	uint64_t t = total[s], kids = 0;
	int k;

	for (k = 0; k < Profile::NUM_SECTIONS; k++)
		if (sections[k].parent == s)
			kids += total[k];
	return (t > kids) ? t - kids : 0;
}

static int
depth (int s)
{
	int d = 0;

	while (sections[s].parent >= 0) {
		s = sections[s].parent;
		d++;
	}
	return d;
}

static void
print_tree (const uint64_t *total, const uint64_t *units, int parent, double ms_per_tick, uint64_t frames)
{
	double whole = (double) total[Profile::FRAME];
	int s;

	for (s = 0; s < Profile::NUM_SECTIONS; s++) {
		if (sections[s].parent != parent || !total[s])
			continue;
		HL_PRINT ("  %*s%-*s %8.3f ms/frame %5.1f%%", depth (s) * 2, "",
		         22 - depth (s) * 2, sections[s].name,
		         total[s] * ms_per_tick / (double) frames, 100.0 * total[s] / whole);
		if (sections[s].unit && units[s])
			HL_PRINT ("  %*s%.1f ns a %s, %.0f a frame", depth (s) * 2 + 2, "",
			          total[s] * ms_per_tick * 1e6 / (double) units[s], sections[s].unit,
			          (double) units[s] / (double) frames);
		print_tree (total, units, s, ms_per_tick, frames);
		/* The rest of a parent, when it has children */
		if (own (total, s) != total[s])
			HL_PRINT ("  %*s%-*s %8.3f ms/frame %5.1f%%", depth (s) * 2 + 2, "",
			         20 - depth (s) * 2, "(the rest)",
			         own (total, s) * ms_per_tick / (double) frames, 100.0 * own (total, s) / whole);
	}
}

void
Profile::report (uint64_t frames)
{
	double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now () - start_time).count ();
	double ticks_per_s = (double) (now () - start_ticks) / seconds;
	double ms_per_tick = 1000.0 / ticks_per_s;

#ifdef _WIN32
	sample_report ();
#endif
	if (!enabled || !frames || !total[FRAME])
		return;
	HL_PRINT ("profile: %llu frames, %.3f ms each (%.1f a second without the limiter), %.2f GHz counter",
	         (unsigned long long) frames, total[FRAME] * ms_per_tick / (double) frames,
	         1000.0 * (double) frames / (total[FRAME] * ms_per_tick), ticks_per_s / 1e9);
	print_tree (total, units, -1, ms_per_tick, frames);
	if (frame_cpu_total)
		HL_PRINT ("profile: the frames' own CPU time on their thread, %.3f ms each (without other programs')",
		          frame_cpu_total * ms_per_tick / (double) frames);
}

} /* namespace hl */
