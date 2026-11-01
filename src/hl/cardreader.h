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

#ifndef __HL_CARDREADER_H__
#define __HL_CARDREADER_H__

#include <deque>
#include <string>
#include <vector>

#include "hl/core.h"
#include "hl/device.h"

namespace hl {

/* A Sanwa CRP-1231 magnetic card reader/writer, as Sega's cabinets have it
 * on a serial line: see cardreader.cpp. The host sends it bytes and takes
 * its own, one at a time; the player's card is a file, its three tracks. */
class CardReader : public Device {
public:
	/* A track's bytes */
	static const unsigned TRACK = 0x45;
	static const unsigned TRACKS = 3;

	/* card_path: the card's file, made when the game writes one */
	CardReader (Machine *mach, const std::string &card_path);

	/* The host's side of the line */
	void	 receive (uint8_t b);
	bool	 has_byte () const { return !out.empty (); }
	uint8_t	 send ();

	/* The player: the card put in the slot (its file read, or a blank
	 * card when there is none), or taken back once ejected */
	void	 insert_card ();
	void	 take_card ();
	/* The control: inserts the card, or takes it back */
	void	 toggle_card ();
	/* At each frame: the control (hl::ACTION_CARD) pressed; a card the
	 * reader gave back is taken after a few seconds */
	void	 frame ();

	void	 reset (ResetType type) override;
	int	 save_state (State *state) override;
	int	 load_state (State *state) override;
	const char *debug_string () override;
	/* "log": the frames, to the log; "insert" / "take": the card */
	bool	 set_option (const char *name, int value) override;

private:
	void	 command ();
	void	 answer ();
	uint8_t	 sensors () const;
	void	 keep_old_card ();
	void	 save_card ();

	std::string	 path;
	bool		 log;
	/* The frame coming in; the bytes going out */
	std::vector<uint8_t> in;
	std::deque<uint8_t>  out;
	/* The last command acknowledged, for the host's ENQ */
	std::vector<uint8_t> pending;
	/* Where the card is: none, at the slot (the player's hand), inside,
	 * or given back at the slot */
	enum Where : uint8_t { NO_CARD, AT_SLOT, INSIDE, EJECTED };
	Where		 where;
	bool		 shutter_open;
	/* The control, as it was at the last frame; the frames a card has
	 * been given back */
	bool		 held;
	uint32_t	 ejected_frames;
	uint8_t		 card[TRACK * TRACKS];
	char		 text[96];
};

} /* namespace hl */

#endif /* __HL_CARDREADER_H__ */
