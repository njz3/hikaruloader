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

/* The Lua shell. Lua is built as C++ (see CMakeLists.txt): its errors are C++
 * exceptions, which unwind the bindings' frames, and its headers have C++
 * linkage. */

#include <algorithm>
#include <climits>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "hl/shell.h"
#include "hl/emulator.h"
#include "hl/cpu.h"
#include "hl/input.h"
#include "hl/window.h"

namespace hl {

/* The shell of a Lua state, kept in its extra space */
static Shell *
get_shell (lua_State *L)
{
	return *(Shell **) lua_getextraspace (L);
}

static Emulator *
get_emu (lua_State *L)
{
	return get_shell (L)->emu;
}

/* Runs fn, which runs the machine: an abort stops it, and becomes a Lua
 * error. Not while the machine runs already: from an irq or interrupt hook */
template <typename F>
static void
run_machine (lua_State *L, F fn)
{
	if (get_emu (L)->in_machine)
		luaL_error (L, "not while the machine runs (from an irq or interrupt hook)");
	try {
		fn ();
	} catch (const Abort &e) {
		get_emu (L)->aborted (e.what ());
		luaL_error (L, "abort: %s", e.what ());
	}
}

/* print (): to the client whose command runs */
static int
shell_print (lua_State *L)
{
	Shell *shell = get_shell (L);
	int i, num = lua_gettop (L);

	for (i = 1; i <= num; i++) {
		size_t length;
		const char *text = luaL_tolstring (L, i, &length);
		if (i > 1)
			shell->output += '\t';
		shell->output.append (text, length);
		lua_pop (L, 1);
	}
	shell->output += '\n';
	return 0;
}

/* Machine objects */

/* The CPUs, memories and devices, as Lua sees them: full userdata of one
 * metatable */
struct Object {
	enum Kind {
		CPU,
		MEMORY,
		DEVICE
	} kind;
	const char *name;
	Cpu *cpu;
	Buffer *buf;
	Device *dev;
};

static const char object_type[] = "hl.object";
static const char *const kind_names[] = { "cpu", "memory", "device" };

static Object *
push_object (lua_State *L, Object::Kind kind, const char *name)
{
	Object *obj = (Object *) lua_newuserdatauv (L, sizeof (Object), 0);

	memset (obj, 0, sizeof (*obj));
	obj->kind = kind;
	obj->name = name;
	luaL_setmetatable (L, object_type);
	return obj;
}

/* Any object, or one of kind when kind >= 0 */
static Object *
check_object (lua_State *L, int arg, int kind = -1)
{
	Object *obj = (Object *) luaL_checkudata (L, arg, object_type);

	if (kind >= 0 && obj->kind != kind)
		luaL_typeerror (L, arg, kind_names[kind]);
	return obj;
}

/* Reads or writes size bytes at addr: an address of the CPU, an offset in
 * the memory, an address of the device's registers. False when nothing
 * answers there. An abort, an unhandled register most often, becomes a Lua
 * error, and the machine goes on. */
static bool
access (lua_State *L, Object *obj, bool write, unsigned size, uint32_t addr, uint64_t *val)
{
	int ret = -1;

	try {
		switch (obj->kind) {
		case Object::CPU:
			obj->cpu->mach->set_current_cpu (obj->cpu);
			ret = write ? obj->cpu->debug_write (size, addr, *val) :
			              obj->cpu->debug_read (size, addr, val);
			break;
		case Object::MEMORY:
			if ((uint64_t) addr + size > obj->buf->size ())
				break;
			if (write)
				obj->buf->put (size, addr, *val);
			else
				*val = obj->buf->get (size, addr);
			ret = 0;
			break;
		case Object::DEVICE:
			ret = write ? obj->dev->put (size, addr, *val) :
			              obj->dev->get (size, addr, val);
			break;
		}
	} catch (const Abort &e) {
		luaL_error (L, "abort: %s", e.what ());
	}
	return ret == 0;
}

static int
access_error (lua_State *L, Object *obj, bool write, unsigned size, uint32_t addr)
{
	char text[128];

	snprintf (text, sizeof (text), "%s: cannot %s %u bits at %08X",
	          obj->name, write ? "write" : "read", size * 8, addr);
	return luaL_error (L, "%s", text);
}

/* obj:read_u8 (addr) ... obj:read_i64 (addr); the upvalues are the size, and
 * whether the value is signed */
static int
object_read_int (lua_State *L)
{
	Object *obj = check_object (L, 1);
	uint32_t addr = (uint32_t) luaL_checkinteger (L, 2);
	unsigned size = (unsigned) lua_tointeger (L, lua_upvalueindex (1));
	uint64_t val = 0;

	if (!access (L, obj, false, size, addr, &val))
		return access_error (L, obj, false, size, addr);
	if (lua_toboolean (L, lua_upvalueindex (2))) {
		unsigned shift = 64 - size * 8;
		lua_pushinteger (L, (lua_Integer) ((int64_t) (val << shift) >> shift));
	} else
		lua_pushinteger (L, (lua_Integer) val);
	return 1;
}

/* obj:write_u8 (addr, value) ... obj:write_i64 (addr, value); the upvalue is
 * the size */
static int
object_write_int (lua_State *L)
{
	Object *obj = check_object (L, 1);
	uint32_t addr = (uint32_t) luaL_checkinteger (L, 2);
	uint64_t val = (uint64_t) luaL_checkinteger (L, 3);
	unsigned size = (unsigned) lua_tointeger (L, lua_upvalueindex (1));

	if (!access (L, obj, true, size, addr, &val))
		return access_error (L, obj, true, size, addr);
	return 0;
}

/* obj:read (addr, length): the bytes, as a string */
static int
object_read (lua_State *L)
{
	Object *obj = check_object (L, 1);
	uint32_t addr = (uint32_t) luaL_checkinteger (L, 2);
	lua_Integer i, length = luaL_checkinteger (L, 3);
	luaL_Buffer b;
	char *bytes;

	luaL_argcheck (L, length >= 0 && length <= 64*MB, 3, "out of range");
	if (obj->kind == Object::MEMORY) {
		luaL_argcheck (L, (uint64_t) addr + length <= obj->buf->size (), 3, "past the end");
		lua_pushlstring (L, length ? (const char *) obj->buf->ptr (addr) : "", (size_t) length);
		return 1;
	}
	bytes = luaL_buffinitsize (L, &b, (size_t) length);
	for (i = 0; i < length; i++) {
		uint64_t val = 0;
		if (!access (L, obj, false, 1, addr + (uint32_t) i, &val))
			return access_error (L, obj, false, 1, addr + (uint32_t) i);
		bytes[i] = (char) val;
	}
	luaL_pushresultsize (&b, (size_t) length);
	return 1;
}

/* obj:write (addr, bytes) */
static int
object_write (lua_State *L)
{
	Object *obj = check_object (L, 1);
	uint32_t addr = (uint32_t) luaL_checkinteger (L, 2);
	size_t i, length;
	const char *bytes = luaL_checklstring (L, 3, &length);

	if (obj->kind == Object::MEMORY) {
		luaL_argcheck (L, (uint64_t) addr + length <= obj->buf->size (), 3, "past the end");
		if (length)
			memcpy (obj->buf->ptr (addr), bytes, length);
		return 0;
	}
	for (i = 0; i < length; i++) {
		uint64_t val = (uint8_t) bytes[i];
		if (!access (L, obj, true, 1, addr + (uint32_t) i, &val))
			return access_error (L, obj, true, 1, addr + (uint32_t) i);
	}
	return 0;
}

/* mem:dump (path [, offset [, length]]): to a file, raw */
static int
memory_dump (lua_State *L)
{
	Object *obj = check_object (L, 1, Object::MEMORY);
	const char *path = luaL_checkstring (L, 2);
	lua_Integer size = obj->buf->size ();
	lua_Integer offs = luaL_optinteger (L, 3, 0);
	lua_Integer length = luaL_optinteger (L, 4, size - offs);
	FILE *fp;
	bool ok;

	luaL_argcheck (L, offs >= 0 && offs <= size, 3, "out of range");
	luaL_argcheck (L, length >= 0 && offs + length <= size, 4, "past the end");
	fp = fopen (path, "wb");
	if (!fp)
		return luaL_fileresult (L, 0, path);
	ok = !length || fwrite (obj->buf->ptr ((unsigned) offs), 1, (size_t) length, fp) == (size_t) length;
	ok = (fclose (fp) == 0) && ok;
	return luaL_fileresult (L, ok, path);
}

/* mem:load (path [, offset]): from a file, raw; returns the bytes loaded */
static int
memory_load (lua_State *L)
{
	Object *obj = check_object (L, 1, Object::MEMORY);
	const char *path = luaL_checkstring (L, 2);
	lua_Integer size = obj->buf->size ();
	lua_Integer offs = luaL_optinteger (L, 3, 0);
	size_t got = 0;
	FILE *fp;

	luaL_argcheck (L, offs >= 0 && offs <= size, 3, "out of range");
	fp = fopen (path, "rb");
	if (!fp)
		return luaL_fileresult (L, 0, path);
	if (offs < size)
		got = fread (obj->buf->ptr ((unsigned) offs), 1, (size_t) (size - offs), fp);
	fclose (fp);
	lua_pushinteger (L, (lua_Integer) got);
	return 1;
}

static int
find_register (Cpu *cpu, const char *name)
{
	unsigned i;

	for (i = 0; i < cpu->num_registers (); i++)
		if (!strcasecmp (cpu->register_name (i), name))
			return (int) i;
	return -1;
}

/* cpu:step ([count]): runs count (1) instructions of this CPU alone, from the
 * breakpoint it stopped at; the other CPUs and the devices wait. Returns the
 * instructions run (a delay slot is one more), and the breakpoint reached */
static int
cpu_step (lua_State *L)
{
	Cpu *cpu = check_object (L, 1, Object::CPU)->cpu;
	lua_Integer count = luaL_optinteger (L, 2, 1);

	luaL_argcheck (L, count > 0 && count <= INT_MAX, 2, "out of range");
	cpu->resume ();
	run_machine (L, [&] {
		cpu->mach->set_current_cpu (cpu);
		cpu->run ((int) count);
	});
	lua_pushinteger (L, count - cpu->remaining);
	if (cpu->break_hit) {
		char text[64];
		snprintf (text, sizeof (text), "break at %08X", cpu->break_pc);
		lua_pushstring (L, text);
		return 2;
	}
	return 1;
}

/* cpu:bpset (addr), cpu:bpclear ([addr]), all of them without addr;
 * cpu:bplist () */
static int
cpu_bpset (lua_State *L)
{
	Cpu *cpu = check_object (L, 1, Object::CPU)->cpu;
	uint32_t addr = (uint32_t) luaL_checkinteger (L, 2) & cpu->addr_mask;

	if (std::find (cpu->breakpoints.begin (), cpu->breakpoints.end (), addr) ==
	    cpu->breakpoints.end ())
		cpu->breakpoints.push_back (addr);
	return 0;
}

static int
cpu_bpclear (lua_State *L)
{
	Cpu *cpu = check_object (L, 1, Object::CPU)->cpu;
	std::vector<uint32_t> &bps = cpu->breakpoints;

	if (lua_isnoneornil (L, 2))
		bps.clear ();
	else {
		uint32_t addr = (uint32_t) luaL_checkinteger (L, 2) & cpu->addr_mask;
		bps.erase (std::remove (bps.begin (), bps.end (), addr), bps.end ());
	}
	return 0;
}

static int
cpu_bplist (lua_State *L)
{
	Cpu *cpu = check_object (L, 1, Object::CPU)->cpu;
	size_t i;

	lua_createtable (L, (int) cpu->breakpoints.size (), 0);
	for (i = 0; i < cpu->breakpoints.size (); i++) {
		lua_pushinteger (L, cpu->breakpoints[i]);
		lua_rawseti (L, -2, (lua_Integer) i + 1);
	}
	return 1;
}

/* cpu:wpset (addr [, length]): its writes there go to the "write" hooks;
 * cpu:wpclear ([addr]), all of them without addr; cpu:wplist () */
static int
cpu_wpset (lua_State *L)
{
	Cpu *cpu = check_object (L, 1, Object::CPU)->cpu;
	uint32_t addr = (uint32_t) luaL_checkinteger (L, 2) & cpu->addr_mask;
	lua_Integer i, length = luaL_optinteger (L, 3, 1);

	luaL_argcheck (L, length > 0 && length <= 0x10000, 3, "out of range");
	for (i = 0; i < length; i++)
		if (std::find (cpu->watchpoints.begin (), cpu->watchpoints.end (), addr + i) ==
		    cpu->watchpoints.end ())
			cpu->watchpoints.push_back (addr + (uint32_t) i);
	return 0;
}

static int
cpu_wpclear (lua_State *L)
{
	Cpu *cpu = check_object (L, 1, Object::CPU)->cpu;
	std::vector<uint32_t> &wps = cpu->watchpoints;

	if (lua_isnoneornil (L, 2))
		wps.clear ();
	else {
		uint32_t addr = (uint32_t) luaL_checkinteger (L, 2) & cpu->addr_mask;
		wps.erase (std::remove (wps.begin (), wps.end (), addr), wps.end ());
	}
	return 0;
}

static int
cpu_wplist (lua_State *L)
{
	Cpu *cpu = check_object (L, 1, Object::CPU)->cpu;
	size_t i;

	lua_createtable (L, (int) cpu->watchpoints.size (), 0);
	for (i = 0; i < cpu->watchpoints.size (); i++) {
		lua_pushinteger (L, cpu->watchpoints[i]);
		lua_rawseti (L, -2, (lua_Integer) i + 1);
	}
	return 1;
}

/* cpu:profile (true) counts the instructions run at each address, from now
 * on; cpu:profile () stops, and returns the counts: address = count */
static int
cpu_profile (lua_State *L)
{
	Cpu *cpu = check_object (L, 1, Object::CPU)->cpu;

	if (lua_toboolean (L, 2)) {
		if (cpu->profile)
			cpu->profile->clear ();
		else
			cpu->profile = new std::unordered_map<uint32_t, uint64_t> ();
		return 0;
	}
	lua_newtable (L);
	if (cpu->profile) {
		for (auto &count : *cpu->profile) {
			lua_pushinteger (L, (lua_Integer) count.second);
			lua_rawseti (L, -2, count.first);
		}
		delete cpu->profile;
		cpu->profile = NULL;
	}
	return 1;
}

/* cpu:registers (): their names in order, and name = value */
static int
cpu_registers (lua_State *L)
{
	Cpu *cpu = check_object (L, 1, Object::CPU)->cpu;
	unsigned i, num = cpu->num_registers ();

	lua_createtable (L, num, num);
	for (i = 0; i < num; i++) {
		lua_pushstring (L, cpu->register_name (i));
		lua_rawseti (L, -2, i + 1);
		lua_pushinteger (L, (lua_Integer) cpu->get_register (i));
		lua_setfield (L, -2, cpu->register_name (i));
	}
	return 1;
}

/* obj:info (): the CPU's or the device's state in a line, or nil */
static int
object_info (lua_State *L)
{
	Object *obj = check_object (L, 1);
	const char *text = NULL;

	if (obj->kind == Object::CPU)
		text = obj->cpu->debug_string ();
	else if (obj->kind == Object::DEVICE)
		text = obj->dev->debug_string ();
	if (text)
		lua_pushstring (L, text);
	else
		lua_pushnil (L);
	return 1;
}

/* dev:option (name, value): a debugging option of the device, e.g. the GPU's
 * "log_cp" (to the log, with emu.verbose (1)); a CPU's too, e.g. the SH-4's
 * "idle"; false when it has none */
static int
device_option (lua_State *L)
{
	Object *obj = check_object (L, 1);
	const char *name = luaL_checkstring (L, 2);
	int value = lua_isboolean (L, 3) ? lua_toboolean (L, 3) : (int) luaL_checkinteger (L, 3);
	Device *dev = obj->kind == Object::CPU ? obj->cpu :
	              obj->kind == Object::DEVICE ? obj->dev : NULL;

	if (!dev)
		luaL_typeerror (L, 1, "device or CPU");
	lua_pushboolean (L, dev->set_option (name, value));
	return 1;
}

/* The methods (the upvalue), then the fields: name, kind, a memory's size, a
 * CPU's registers, state and break_pc */
static int
object_index (lua_State *L)
{
	static const char *const states[] = { "run", "stop", "sleep", "standby" };
	Object *obj = check_object (L, 1);
	const char *key;
	int reg;

	lua_pushvalue (L, 2);
	if (lua_rawget (L, lua_upvalueindex (1)) != LUA_TNIL)
		return 1;
	if (lua_type (L, 2) != LUA_TSTRING)
		return 1;
	key = lua_tostring (L, 2);

	if (!strcmp (key, "name"))
		lua_pushstring (L, obj->name);
	else if (!strcmp (key, "kind"))
		lua_pushstring (L, kind_names[obj->kind]);
	else if (obj->kind == Object::MEMORY && !strcmp (key, "size"))
		lua_pushinteger (L, obj->buf->size ());
	else if (obj->kind == Object::CPU && (reg = find_register (obj->cpu, key)) >= 0)
		lua_pushinteger (L, (lua_Integer) obj->cpu->get_register (reg));
	else if (obj->kind == Object::CPU && !strcmp (key, "state"))
		lua_pushstring (L, (obj->cpu->state < NUM_CPU_STATES) ? states[obj->cpu->state] : "?");
	else if (obj->kind == Object::CPU && !strcmp (key, "break_pc")) {
		if (obj->cpu->break_hit)
			lua_pushinteger (L, obj->cpu->break_pc);
		else
			lua_pushnil (L);
	} else
		lua_pushnil (L);
	return 1;
}

/* cpu.<register> = value */
static int
object_newindex (lua_State *L)
{
	Object *obj = check_object (L, 1);
	const char *key = luaL_checkstring (L, 2);
	int reg = (obj->kind == Object::CPU) ? find_register (obj->cpu, key) : -1;

	if (reg < 0)
		return luaL_error (L, "%s has no register '%s'", obj->name, key);
	obj->cpu->set_register (reg, (uint64_t) luaL_checkinteger (L, 3));
	return 0;
}

static int
object_tostring (lua_State *L)
{
	Object *obj = check_object (L, 1);
	char text[256];

	switch (obj->kind) {
	case Object::CPU:
		snprintf (text, sizeof (text), "cpu %s: %s%s", obj->name, obj->cpu->debug_string (),
		          obj->cpu->break_hit ? " (at a breakpoint)" : "");
		break;
	case Object::MEMORY:
		snprintf (text, sizeof (text), "memory %s: %u bytes", obj->name, obj->buf->size ());
		break;
	default:
		snprintf (text, sizeof (text), "device %s%s%s", obj->name,
		          obj->dev->debug_string () ? ": " : "",
		          obj->dev->debug_string () ? obj->dev->debug_string () : "");
		break;
	}
	lua_pushstring (L, text);
	return 1;
}

static void
register_objects (lua_State *L)
{
	static const struct {
		const char *name;
		unsigned size;
		bool is_signed;
	} ints[] = {
		{ "u8", 1, false }, { "u16", 2, false }, { "u32", 4, false }, { "u64", 8, false },
		{ "i8", 1, true }, { "i16", 2, true }, { "i32", 4, true }, { "i64", 8, true },
	};
	static const luaL_Reg methods[] = {
		{ "read",	object_read },
		{ "write",	object_write },
		{ "dump",	memory_dump },
		{ "load",	memory_load },
		{ "step",	cpu_step },
		{ "bpset",	cpu_bpset },
		{ "bpclear",	cpu_bpclear },
		{ "bplist",	cpu_bplist },
		{ "wpset",	cpu_wpset },
		{ "wpclear",	cpu_wpclear },
		{ "wplist",	cpu_wplist },
		{ "registers",	cpu_registers },
		{ "profile",	cpu_profile },
		{ "info",	object_info },
		{ "option",	device_option },
		{ NULL, NULL }
	};
	char name[16];
	unsigned i;

	luaL_newmetatable (L, object_type);

	lua_newtable (L);
	luaL_setfuncs (L, methods, 0);
	for (i = 0; i < NUMELEM (ints); i++) {
		lua_pushinteger (L, ints[i].size);
		lua_pushboolean (L, ints[i].is_signed);
		lua_pushcclosure (L, object_read_int, 2);
		snprintf (name, sizeof (name), "read_%s", ints[i].name);
		lua_setfield (L, -2, name);

		lua_pushinteger (L, ints[i].size);
		lua_pushcclosure (L, object_write_int, 1);
		snprintf (name, sizeof (name), "write_%s", ints[i].name);
		lua_setfield (L, -2, name);
	}
	lua_pushcclosure (L, object_index, 1);
	lua_setfield (L, -2, "__index");

	lua_pushcfunction (L, object_newindex);
	lua_setfield (L, -2, "__newindex");
	lua_pushcfunction (L, object_tostring);
	lua_setfield (L, -2, "__tostring");
	lua_pop (L, 1);
}

/* machine: its name, cpus (by name, and in order), memory and devices */
static void
push_machine (lua_State *L, Machine *mach)
{
	lua_Integer i = 0;

	lua_newtable (L);
	lua_pushstring (L, mach->name);
	lua_setfield (L, -2, "name");

	lua_newtable (L);
	for (Cpu *cpu : mach->cpus) {
		Object *obj = push_object (L, Object::CPU, cpu->name ? cpu->name : "cpu");
		obj->cpu = cpu;
		lua_pushvalue (L, -1);
		lua_rawseti (L, -3, ++i);
		lua_setfield (L, -2, obj->name);
	}
	lua_setfield (L, -2, "cpus");

	lua_newtable (L);
	for (auto &memory : mach->memories) {
		Object *obj = push_object (L, Object::MEMORY, memory.first);
		obj->buf = memory.second;
		lua_setfield (L, -2, obj->name);
	}
	lua_setfield (L, -2, "memory");

	/* The CPUs are devices too, already in cpus */
	lua_newtable (L);
	for (Device *dev : mach->devices) {
		if (!dev->name ||
		    std::find (mach->cpus.begin (), mach->cpus.end (), dev) != mach->cpus.end ())
			continue;
		Object *obj = push_object (L, Object::DEVICE, dev->name);
		obj->dev = dev;
		lua_setfield (L, -2, obj->name);
	}
	lua_setfield (L, -2, "devices");
}

/* emu */

static int
emu_pause (lua_State *L)
{
	get_emu (L)->paused = true;
	return 0;
}

static int
emu_resume (lua_State *L)
{
	Emulator *emu = get_emu (L);

	if (!emu->resume ())
		return luaL_error (L, "%s; reset the machine, or load a state", emu->stop_message);
	return 0;
}

static int
emu_paused (lua_State *L)
{
	lua_pushboolean (L, get_emu (L)->paused);
	return 1;
}

/* emu.step ([frames]): runs frames (1) now, then the emulator stays paused,
 * or running, as it was. Returns the frames run, and what stopped them
 * early: a breakpoint, an abort */
static int
emu_step (lua_State *L)
{
	Emulator *emu = get_emu (L);
	lua_Integer num = luaL_optinteger (L, 1, 1);
	int done;

	luaL_argcheck (L, num > 0 && num <= INT_MAX, 1, "out of range");
	if (emu->in_frame)
		return luaL_error (L, "not during a frame (from a hook)");
	done = emu->step ((int) num);
	if (done < 0)
		return luaL_error (L, "%s; reset the machine, or load a state", emu->stop_message);
	lua_pushinteger (L, done);
	if (emu->last_stop == Emulator::STOP_NONE)
		return 1;
	lua_pushstring (L, emu->stop_message);
	return 2;
}

static int
emu_frame (lua_State *L)
{
	lua_pushinteger (L, (lua_Integer) get_emu (L)->mach->frame);
	return 1;
}

/* "paused, frame 1234; break: master at 0C001000, frame 1234" */
static int
emu_status (lua_State *L)
{
	Emulator *emu = get_emu (L);
	bool why = emu->crashed || emu->last_stop != Emulator::STOP_NONE;
	char text[1280];

	snprintf (text, sizeof (text), "%s, frame %llu%s%s",
	          emu->crashed ? "aborted" : emu->paused ? "paused" : "running",
	          (unsigned long long) emu->mach->frame, why ? "; " : "", why ? emu->stop_message : "");
	lua_pushstring (L, text);
	return 1;
}

/* emu.reset ([soft]) */
static int
emu_reset (lua_State *L)
{
	Emulator *emu = get_emu (L);
	ResetType type = lua_toboolean (L, 1) ? RESET_SOFT : RESET_HARD;

	run_machine (L, [&] { emu->reset (type); });
	return 0;
}

/* True, or nil and the error */
static int
result (lua_State *L, bool ok, const char *what, const char *path)
{
	if (ok) {
		lua_pushboolean (L, 1);
		return 1;
	}
	lua_pushnil (L);
	lua_pushfstring (L, "cannot %s '%s'", what, path);
	return 2;
}

static int
emu_save_state (lua_State *L)
{
	const char *path = luaL_checkstring (L, 1);
	int ret = -1;

	run_machine (L, [&] { ret = get_emu (L)->save_state (path); });
	return result (L, !ret, "save a state to", path);
}

static int
emu_load_state (lua_State *L)
{
	const char *path = luaL_checkstring (L, 1);
	int ret = -1;

	run_machine (L, [&] { ret = get_emu (L)->load_state (path); });
	return result (L, !ret, "load the state", path);
}

static int
emu_screenshot (lua_State *L)
{
	const char *path = luaL_checkstring (L, 1);

	return result (L, Window::screenshot (path), "save the last frame to", path);
}

/* emu.input (name [, held]): holds (true, by default) or releases an input,
 * e.g. "TEST" */
static int
emu_input (lua_State *L)
{
	const char *name = luaL_checkstring (L, 1);
	int action = Input::find_action (name);

	if (action < 0)
		return luaL_error (L, "no input named '%s'", name);
	Input::set_action ((Action) action, lua_isnone (L, 2) || lua_toboolean (L, 2));
	return 0;
}

/* emu.game (): the set's name, and the game's */
static int
emu_game (lua_State *L)
{
	Emulator *emu = get_emu (L);
	GameEntry *entry = emu->game_list->find (emu->game->name);

	lua_pushstring (L, emu->game->name);
	lua_pushstring (L, entry ? entry->description : emu->game->name);
	return 2;
}

static int
emu_fps (lua_State *L)
{
	lua_pushnumber (L, Window::fps ());
	return 1;
}

static int
emu_version (lua_State *L)
{
	lua_pushstring (L, HL_VERSION_FULL);
	return 1;
}

/* emu.verbose (level): the log's verbosity, as -v sets it to 1 */
static int
emu_verbose (lua_State *L)
{
	hl_verbosity = (unsigned) luaL_checkinteger (L, 1);
	return 0;
}

static int
emu_log (lua_State *L)
{
	HL_PRINT ("shell: %s", luaL_checkstring (L, 1));
	return 0;
}

static int
emu_quit (lua_State *L)
{
	get_emu (L)->quit = true;
	return 0;
}

/* emu.snapshots (every [, keep [, dir]]): a state every `every` frames (never
 * when 0), the last `keep` (20) kept in dir ("snapshots"); emu.snapshots ():
 * the frames that have one */
static int
emu_snapshots (lua_State *L)
{
	Emulator *emu = get_emu (L);
	lua_Integer i = 0;

	if (lua_isnone (L, 1)) {
		lua_createtable (L, (int) emu->snapshots.size (), 0);
		for (auto &snapshot : emu->snapshots) {
			lua_pushinteger (L, (lua_Integer) snapshot.first);
			lua_rawseti (L, -2, ++i);
		}
		return 1;
	}
	emu->set_snapshots ((unsigned) luaL_checkinteger (L, 1),
	                    (unsigned) luaL_optinteger (L, 2, 20),
	                    luaL_optstring (L, 3, "snapshots"));
	return 0;
}

/* emu.seek (frame): goes to frame, from the last snapshot before it (or from
 * a hard reset); returns the frame, or nil and what stopped it */
static int
emu_seek (lua_State *L)
{
	Emulator *emu = get_emu (L);
	lua_Integer target = luaL_checkinteger (L, 1);
	bool ok = false;

	luaL_argcheck (L, target >= 0, 1, "out of range");
	if (emu->in_frame)
		return luaL_error (L, "not during a frame (from a hook)");
	run_machine (L, [&] { ok = emu->seek ((uint64_t) target); });
	if (!ok) {
		lua_pushnil (L);
		lua_pushstring (L, emu->last_stop != Emulator::STOP_NONE || emu->crashed ?
		                   emu->stop_message : "cannot load the snapshot");
		return 2;
	}
	lua_pushinteger (L, (lua_Integer) emu->mach->frame);
	return 1;
}

static const char *const event_names[Shell::NUM_EVENTS] = {
	"start", "exit", "frame_begin", "frame_end", "break", "abort", "irq", "interrupt", "write"
};

/* emu.hook (event, fn): fn runs on event; returns the hook's number, for
 * emu.unhook */
static int
emu_hook (lua_State *L)
{
	const char *name = luaL_checkstring (L, 1);
	int event;

	luaL_checktype (L, 2, LUA_TFUNCTION);
	for (event = 0; event < Shell::NUM_EVENTS; event++)
		if (!strcmp (event_names[event], name))
			break;
	if (event == Shell::NUM_EVENTS)
		return luaL_error (L, "no event '%s': start, exit, frame_begin, frame_end, "
		                      "break, abort, irq, interrupt, write", name);
	lua_pushvalue (L, 2);
	lua_pushinteger (L, get_shell (L)->add_hook ((Shell::Event) event,
	                                             luaL_ref (L, LUA_REGISTRYINDEX)));
	return 1;
}

static int
emu_unhook (lua_State *L)
{
	lua_pushboolean (L, get_shell (L)->remove_hook ((int) luaL_checkinteger (L, 1)));
	return 1;
}

static const luaL_Reg emu_functions[] = {
	{ "pause",	emu_pause },
	{ "resume",	emu_resume },
	{ "paused",	emu_paused },
	{ "step",	emu_step },
	{ "frame",	emu_frame },
	{ "status",	emu_status },
	{ "reset",	emu_reset },
	{ "save_state",	emu_save_state },
	{ "load_state",	emu_load_state },
	{ "screenshot",	emu_screenshot },
	{ "input",	emu_input },
	{ "game",	emu_game },
	{ "fps",	emu_fps },
	{ "version",	emu_version },
	{ "verbose",	emu_verbose },
	{ "log",	emu_log },
	{ "quit",	emu_quit },
	{ "hook",	emu_hook },
	{ "unhook",	emu_unhook },
	{ "snapshots",	emu_snapshots },
	{ "seek",	emu_seek },
	{ NULL, NULL }
};

/* The helpers written in Lua, and help () */
static const char prelude[] = R"lua(
function hex (value, digits)
	return string.format ("%0" .. (digits or 8) .. "X", value)
end

-- The float whose bits are bits (an FPU register), and back
function f32 (bits)
	return (string.unpack ("<f", string.pack ("<I4", bits & 0xFFFFFFFF)))
end

function bits32 (number)
	return (string.unpack ("<I4", string.pack ("<f", number)))
end

function hexdump (object, addr, length)
	local data = object:read (addr, length or 256)
	for row = 0, #data - 1, 16 do
		local bytes, text = {}, {}
		for i = row + 1, math.min (row + 16, #data) do
			local byte = data:byte (i)
			bytes[#bytes + 1] = string.format ("%02X", byte)
			text[#text + 1] = (byte >= 32 and byte < 127) and string.char (byte) or "."
		end
		print (string.format ("%08X  %-47s  %s", addr + row,
		                      table.concat (bytes, " "), table.concat (text)))
	end
end

function regs (cpu)
	cpu = cpu or machine.cpus[1]
	local values, line = cpu:registers (), {}
	for _, name in ipairs (values) do
		line[#line + 1] = string.format ("%-6s %08X", name, values[name])
		if #line == 4 then
			print (table.concat (line, "   "))
			line = {}
		end
	end
	if #line > 0 then
		print (table.concat (line, "   "))
	end
end

-- The n (20) addresses of a profile (cpu:profile ()) that ran the most
function hot (profile, n)
	local list, total = {}, 0
	for addr, count in pairs (profile) do
		list[#list + 1] = { addr, count }
		total = total + count
	end
	table.sort (list, function (a, b) return a[2] > b[2] end)
	for i = 1, math.min (n or 20, #list) do
		print (string.format ("%08X %12d %5.1f%%", list[i][1], list[i][2],
		                      100 * list[i][2] / total))
	end
end

local function sorted (objects)
	local names = {}
	for name in pairs (objects) do
		names[#names + 1] = name
	end
	table.sort (names)
	return names
end

function ls ()
	for _, cpu in ipairs (machine.cpus) do
		print (cpu)
	end
	for _, name in ipairs (sorted (machine.memory)) do
		print (machine.memory[name])
	end
	for _, name in ipairs (sorted (machine.devices)) do
		print (machine.devices[name])
	end
end

function help ()
	print ([=[
emu, the emulator:
  emu.pause ()  emu.resume ()  emu.paused ()
  emu.step ([frames])     runs frames (1) now; returns the frames run, and what
                          stopped them early (a breakpoint, an abort)
  emu.frame ()            the frames run since the game started
  emu.status ()           running, paused or aborted, and why
  emu.reset ([soft])  emu.save_state (path)  emu.load_state (path)
                          a state holds every chip, and its frame number
  emu.snapshots (every [, keep [, dir]])  a state every `every` frames, the
                          last `keep` (20) kept in dir ("snapshots");
                          emu.snapshots (): the frames that have one
  emu.seek (frame)        goes to frame: from the last snapshot before it, or
                          from a hard reset, running the frames up to it
  emu.screenshot (path)   the last frame, as a BMP file
  emu.input (name [, held])  holds or releases an input: "TEST", "SERVICE"
  emu.game ()  emu.fps ()  emu.version ()  emu.verbose (level)  emu.log (text)
  emu.quit ()
  emu.hook (event, fn)    fn runs on event; returns a number for emu.unhook (n)
    "start", "exit"       once, before the first frame and when the game ends
    "frame_begin", "frame_end"   fn (frame) around each frame
    "break", "abort"      fn (message) when the machine stops
    "irq"                 fn (from, cpu, line, raised): a chip asks a CPU for
                          an interrupt, e.g. ("gpu", "master", "IRL2", true)
    "interrupt"           fn (cpu, line, handler): a CPU takes one
    "write"               fn (cpu, addr, size, value, pc): a CPU writes an
                          address of cpu:wpset (addr [, length])
    init.lua, beside the program, runs at startup: the place for the hooks
machine, the emulated machine (ls () lists its objects):
  machine.cpus.master, machine.cpus.slave (machine.cpus[1], [2])
    cpu.pc, cpu.r0 ... cpu.r15, cpu.sr, cpu.fr0 ...  the registers, to read
                          and write; cpu:registers (), regs ([cpu]) prints them
    cpu:step ([count])    runs count (1) instructions of this CPU alone
    cpu:profile (true)    counts the instructions run per address; then
                          p = cpu:profile () stops, hot (p [, n]) prints them
    cpu:bpset (addr)  cpu:bpclear ([addr])  cpu:bplist ()
                          the CPU stops before addr; the emulator pauses at the
                          end of the frame (the rest of the machine finishes it)
    cpu:option ('idle', 0)  the SH-4's idle loops run as they are, not
                          forward (1, the default)
    cpu:option ('blocks', 0)  the SH-4 fetches each instruction, without
                          its decoded blocks (1, the default)
    cpu:option ('jit', 0)  the blocks run without their x86-64 translation
                          (1, the default); 'dynarec' sets both, as the
                          settings do
    cpu:option ('pages', 0)  its memory map's regions searched one after
                          the other, without the page table
    cpu:option ('fastmem', 1)  its RAM and ROM shown in the host's address
                          space (false when the system cannot)
  machine.memory.ram_m, ram_s, cmdram, fb, texram0, texram1, aica_ram_m,
    aica_ram_s, mie_ram, bram, bootrom, eprom, maskrom, iregs_m, iregs_s
    mem.size  mem:dump (path [, offset [, length]])  mem:load (path [, offset])
  machine.devices.gpu, memctl_m, memctl_s, mie, mscomm, aica_m, aica_s ...
    dev:info (), cpu:info ()  the state in a line
    dev:option (name, value)  a debugging option: the GPU's "log_cp" (the
                          command lists, disassembled), "log_dma", "log_idma";
                          to the log, with emu.verbose (1)
  The CPUs (their address space), the memories (an offset) and the devices
  (their registers' addresses) all have
    obj:read_u8 (addr) ... obj:read_u64 (addr), obj:read_i8 (addr) ...
    obj:write_u8 (addr, value) ... obj:write_u64 (addr, value)
    obj:read (addr, length), the bytes as a string; obj:write (addr, bytes)
Helpers: hex (value [, digits]), hexdump (obj, addr [, length]), f32 (bits),
  bits32 (number), regs ([cpu]), ls ()]=])
end
)lua";

/* Shell */

Shell::Shell (Emulator *emu) :
	emu (emu),
	L (NULL),
	depth (0),
	next_hook (1)
{
	memset (num_hooks, 0, sizeof (num_hooks));
	L = luaL_newstate ();
	HL_ASSERT (L);
	*(Shell **) lua_getextraspace (L) = this;
	luaL_openlibs (L);

	lua_pushcfunction (L, shell_print);
	lua_setglobal (L, "print");
	register_objects (L);
	luaL_newlib (L, emu_functions);
	lua_setglobal (L, "emu");
	push_machine (L, emu->mach);
	lua_setglobal (L, "machine");

	if (luaL_dostring (L, prelude)) {
		HL_ERROR ("shell: %s", lua_tostring (L, -1));
		lua_pop (L, 1);
	}
}

Shell::~Shell ()
{
	emu->mach->hooks = NULL;
	lua_close (L);
}

/* Hooks */

int
Shell::add_hook (Event event, int ref)
{
	hooks.push_back ({ next_hook, event, ref });
	num_hooks[event]++;
	/* The machine calls the shell only for these */
	if (event == EVENT_IRQ || event == EVENT_INTERRUPT || event == EVENT_WRITE)
		emu->mach->hooks = this;
	return next_hook++;
}

bool
Shell::remove_hook (int id)
{
	size_t i;

	for (i = 0; i < hooks.size (); i++)
		if (hooks[i].id == id) {
			luaL_unref (L, LUA_REGISTRYINDEX, hooks[i].ref);
			num_hooks[hooks[i].event]--;
			hooks.erase (hooks.begin () + i);
			if (!num_hooks[EVENT_IRQ] && !num_hooks[EVENT_INTERRUPT] && !num_hooks[EVENT_WRITE])
				emu->mach->hooks = NULL;
			return true;
		}
	return false;
}

void
Shell::call_hooks (Event event, const std::function<int (lua_State *)> &push)
{
	std::vector<Hook> called;

	if (!num_hooks[event])
		return;

	/* A copy: the hooks may add or remove hooks */
	for (const Hook &hook : hooks)
		if (hook.event == event)
			called.push_back (hook);

	depth++;
	for (const Hook &hook : called) {
		int base = lua_gettop (L), num;

		if (std::none_of (hooks.begin (), hooks.end (),
		                  [&] (const Hook &h) { return h.id == hook.id; }))
			continue;
		lua_rawgeti (L, LUA_REGISTRYINDEX, hook.ref);
		num = push ? push (L) : 0;
		if (lua_pcall (L, num, 0, 0) != LUA_OK) {
			char text[1024];
			const char *msg = lua_tostring (L, -1);
			/* Not again at each frame */
			snprintf (text, sizeof (text), "error in the %s hook %d, removed: %s",
			          event_names[event], hook.id, msg ? msg : "(not a string)");
			HL_ERROR ("shell: %s", text);
			output += text;
			output += '\n';
			remove_hook (hook.id);
		}
		lua_settop (L, base);
	}
	depth--;

	/* Outside a command, what they print goes to the log and the clients */
	if (!depth && !output.empty ()) {
		std::string text;
		text.swap (output);
		server.broadcast (text.c_str ());
		if (text.back () == '\n')
			text.pop_back ();
		HL_PRINT ("%s", text.c_str ());
	}
}

void
Shell::hook_number (Event event, uint64_t number)
{
	call_hooks (event, [&] (lua_State *L) {
		lua_pushinteger (L, (lua_Integer) number);
		return 1;
	});
}

void
Shell::hook_text (Event event, const char *text)
{
	call_hooks (event, [&] (lua_State *L) {
		lua_pushstring (L, text);
		return 1;
	});
}

void
Shell::irq (Device *from, Cpu *cpu, const char *line, bool raised)
{
	call_hooks (EVENT_IRQ, [&] (lua_State *L) {
		lua_pushstring (L, from->name ? from->name : "?");
		lua_pushstring (L, cpu->name ? cpu->name : "?");
		lua_pushstring (L, line);
		lua_pushboolean (L, raised);
		return 4;
	});
}

void
Shell::write (Cpu *cpu, uint32_t addr, unsigned size, uint64_t val, uint32_t pc)
{
	call_hooks (EVENT_WRITE, [&] (lua_State *L) {
		lua_pushstring (L, cpu->name ? cpu->name : "?");
		lua_pushinteger (L, addr);
		lua_pushinteger (L, size);
		lua_pushinteger (L, (lua_Integer) val);
		lua_pushinteger (L, pc);
		return 5;
	});
}

void
Shell::interrupt (Cpu *cpu, const char *line, uint32_t handler)
{
	call_hooks (EVENT_INTERRUPT, [&] (lua_State *L) {
		lua_pushstring (L, cpu->name ? cpu->name : "?");
		lua_pushstring (L, line);
		lua_pushinteger (L, handler);
		return 3;
	});
}

bool
Shell::listen (unsigned port)
{
	if (!server.open (port))
		return false;
	HL_PRINT ("shell: %s, listening on 127.0.0.1:%u", LUA_RELEASE, port);
	return true;
}

void
Shell::poll ()
{
	server.poll ([this] (int client) {
		char banner[256];
		snprintf (banner, sizeof (banner),
		          "Hikaruloader %s, %s shell; help () lists the commands\n> ",
		          HL_VERSION_FULL, LUA_RELEASE);
		server.send (client, banner, strlen (banner));
	}, [this] (int client, const char *line) {
		run_line (client, line);
	});
}

void
Shell::notify (const char *text)
{
	std::string line = std::string ("\n*** ") + text + "\n> ";
	server.broadcast (line.c_str ());
}

/* A syntax error at the end of the chunk: it goes on */
static bool
is_incomplete (lua_State *L)
{
	static const char mark[] = "<eof>";
	size_t length;
	const char *msg = lua_tolstring (L, -1, &length);

	return msg && length >= sizeof (mark) - 1 &&
	       !strcmp (msg + length - (sizeof (mark) - 1), mark);
}

bool
Shell::evaluate (const std::string &chunk)
{
	std::string expression = "return " + chunk + ";";
	int base = lua_gettop (L), status, num;

	/* An expression, whose values the shell prints; or statements */
	status = luaL_loadbuffer (L, expression.data (), expression.size (), "=shell");
	if (status != LUA_OK) {
		lua_pop (L, 1);
		status = luaL_loadbuffer (L, chunk.data (), chunk.size (), "=shell");
		if (status == LUA_ERRSYNTAX && is_incomplete (L)) {
			lua_pop (L, 1);
			return true;
		}
	}
	if (status == LUA_OK)
		status = lua_pcall (L, 0, LUA_MULTRET, 0);
	num = lua_gettop (L) - base;
	if (status == LUA_OK && num > 0) {
		lua_pushcfunction (L, shell_print);
		lua_insert (L, base + 1);
		status = lua_pcall (L, num, 0, 0);
	}
	if (status != LUA_OK) {
		const char *msg = lua_tostring (L, -1);
		output += "error: ";
		output += msg ? msg : "(not a string)";
		output += '\n';
	}
	lua_settop (L, base);
	return false;
}

void
Shell::run_line (int client, const char *line)
{
	std::string &chunk = pending[client];
	const char *prompt = "> ";

	if (!chunk.empty ())
		chunk += '\n';
	chunk += line;

	output.clear ();
	depth++;
	if (evaluate (chunk))
		prompt = ">> ";
	else
		chunk.clear ();
	depth--;
	if (!output.empty () && output.back () != '\n')
		output += '\n';
	output += prompt;
	server.send (client, output.data (), output.size ());
	output.clear ();
}

bool
Shell::run_file (const char *path)
{
	int status;

	output.clear ();
	depth++;
	status = luaL_loadfile (L, path);
	if (status == LUA_OK)
		status = lua_pcall (L, 0, 0, 0);
	depth--;
	if (!output.empty ()) {
		if (output.back () == '\n')
			output.pop_back ();
		HL_PRINT ("%s", output.c_str ());
		output.clear ();
	}
	if (status != LUA_OK) {
		HL_ERROR ("shell: %s", lua_tostring (L, -1));
		lua_pop (L, 1);
		return false;
	}
	HL_PRINT ("shell: ran '%s'", path);
	return true;
}

std::string
Shell::run (const char *chunk)
{
	std::string text;

	output.clear ();
	depth++;
	if (evaluate (chunk))
		output += "error: the command is not complete\n";
	depth--;
	text.swap (output);
	return text;
}

} /* namespace hl */
