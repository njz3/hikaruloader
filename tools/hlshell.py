#!/usr/bin/env python3
#
# Hikaruloader (formerly Valkyrie)
# Copyright (C) 2026, Benjamin Maurin (njz3)
#
# Hikaruloader is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Hikaruloader is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Hikaruloader.  If not, see <http://www.gnu.org/licenses/>.

"""A client of Hikaruloader's Lua shell (hikaruloader -s PORT).

    hlshell.py [-p PORT] [COMMAND ...]

With commands, runs each and prints its output; without, reads commands from
the terminal. As a module:

    with Shell(7070) as sh:
        print(sh.run("machine.cpus.master.pc"))
"""

import argparse
import socket
import sys

PROMPTS = ("> ", ">> ")


class Shell:
    def __init__(self, port=7070, host="127.0.0.1", timeout=60.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buffer = b""
        self.banner = self._answer()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        self.sock.close()

    def _answer(self):
        """What the shell says up to its prompt, without the prompt. The
        notices it sends between commands ("*** break: ...") come first."""
        while True:
            text = self.buffer.decode("utf-8", "replace")
            for prompt in PROMPTS:
                if text == prompt or text.endswith("\n" + prompt):
                    self.buffer = b""
                    self.prompt = prompt
                    return text[: -len(prompt)]
            data = self.sock.recv(65536)
            if not data:
                raise ConnectionError("the shell closed the connection")
            self.buffer += data

    def run(self, command):
        """Runs one line; returns its output (an unfinished command answers
        with the ">> " prompt: self.prompt says which came)."""
        self.sock.sendall(command.encode("utf-8") + b"\n")
        return self._answer().rstrip("\n")


def main():
    parser = argparse.ArgumentParser(description="Hikaruloader's Lua shell")
    parser.add_argument("-p", "--port", type=int, default=7070)
    parser.add_argument("commands", nargs="*", help="Lua lines to run")
    args = parser.parse_args()

    with Shell(args.port) as sh:
        if args.commands:
            for command in args.commands:
                out = sh.run(command)
                if out:
                    print(out)
            return
        print(sh.banner.rstrip("\n"))
        while True:
            try:
                line = input(sh.prompt if hasattr(sh, "prompt") else "> ")
            except EOFError:
                break
            out = sh.run(line)
            if out:
                print(out)


if __name__ == "__main__":
    sys.exit(main())
