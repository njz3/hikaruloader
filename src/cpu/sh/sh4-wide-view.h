/*
 * Hikaruloader
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

#ifndef __SH4_WIDE_VIEW_H__
#define __SH4_WIDE_VIEW_H__

namespace hl { class Cpu; }

/* WIDESCREEN: the view's sides widened in the code a game runs on this SH-4
 * (sh4-wide-view.cpp). Nothing is installed with WIDESCREEN off */
void sh4_wide_view_install (hl::Cpu *cpu);

#endif /* __SH4_WIDE_VIEW_H__ */
