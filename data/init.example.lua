-- Hikaruloader (formerly Valkyrie)
-- Copyright (C) 2026, Benjamin Maurin (njz3)
--
-- Hikaruloader is free software: you can redistribute it and/or modify
-- it under the terms of the GNU General Public License as published by
-- the Free Software Foundation, either version 3 of the License, or
-- (at your option) any later version.
--
-- Hikaruloader is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU General Public License for more details.
--
-- You should have received a copy of the GNU General Public License
-- along with Hikaruloader.  If not, see <http://www.gnu.org/licenses/>.

-- An example of init.lua. Copied beside hikaruloader.exe as init.lua, it runs
-- once the game is loaded, before the first frame, in the Lua shell (see
-- README.md): the place for the hooks. What it prints goes to hikaruloader.log,
-- and to the shell's clients when one is open (-s PORT). Uncomment what you
-- need.

print ("init.lua: " .. select (2, emu.game ()))

-- Once, before the first frame, and when the game ends
emu.hook ("start", function ()
	print ("start, frame " .. emu.frame ())
end)

emu.hook ("exit", function ()
	print ("exit after " .. emu.frame () .. " frames")
end)

-- Around each frame: its number
--emu.hook ("frame_begin", function (frame) end)

--emu.hook ("frame_end", function (frame)
--	if frame % 600 == 0 then
--		print (string.format ("frame %d: master at %s", frame, hex (machine.cpus.master.pc)))
--	end
--end)

-- A chip asks a CPU for an interrupt: e.g. ("gpu", "master", "IRL2", true)
--emu.hook ("irq", function (from, cpu, line, raised)
--	print (string.format ("frame %d: %s -> %s %s", emu.frame (), from, cpu, line))
--end)

-- A CPU takes an interrupt, its handler at handler
--emu.hook ("interrupt", function (cpu, line, handler)
--	print (string.format ("%s takes %s at %s", cpu, line, hex (handler)))
--end)

-- What stopped the machine: a breakpoint, an abort (the message). With the
-- shell open, an abort leaves the machine paused as it failed: save what
-- helps to understand it.
--emu.hook ("abort", function (message)
--	machine.memory.ram_m:dump ("abort_ram_m.bin")
--	machine.memory.cmdram:dump ("abort_cmdram.bin")
--	emu.screenshot ("abort.bmp")
--end)

-- A breakpoint, set from here: the master stops before this address, and the
-- emulator pauses at the end of the frame
--machine.cpus.master:bpset (0x0C001000)
