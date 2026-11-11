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

#ifndef __HL_DEVICEPROFILES_H__
#define __HL_DEVICEPROFILES_H__

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace hl {

/* A device's profile: its bindings to the controls' rows (controls.ini's,
 * the games' roles), made once for a model of wheel, pedals, shifter,
 * stick or pad, and shared. A file of its own, controls.ini's syntax:
 *
 *	[Device]
 *	Name = <the device's name, as SDL and the Controls tab give it>
 *	VidPid = <its USB vendor and product, hex: 046D:C24F> (optional)
 *	[Driving]
 *	Steering = JOY_AXIS_0
 *	Accelerator = JOY_AXIS_2_INVERTED
 *	...
 *
 * its inputs without a player's number (JOY_ a joystick's, numbered as
 * SDL numbers them; GC_ a game controller's, by SDL's names), put on the
 * player the device is (JOY<n>_, GC<n>_). The shipped ones in data/devices
 * beside the program, the user's in devices/ beside controls.ini (over
 * them). */
struct DeviceProfile {
	std::string	 file, name;
	uint16_t	 vendor = 0, product = 0;
	bool		 user = false;
	/* Its sections ([Device] excepted), each its rows */
	std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> sections;

	/* A row's inputs, by the device's own names; "" none */
	std::string	 value (const char *section, const char *key) const;
};

class DeviceProfiles {
public:
	static DeviceProfiles &get ();

	/* The shipped profiles, then the user's in user_dir */
	void		 load (const std::string &user_dir);
	const std::string &user_dir () const { return user; }
	const std::vector<DeviceProfile> &profiles () const { return all; }

	/* The device's: its vendor and product first, else its name (case
	 * ignored); the user's over the shipped; NULL none */
	const DeviceProfile *match (const std::string &name, uint16_t vendor, uint16_t product) const;

	/* A profile's inputs ("JOY_AXIS_0, GC_BUTTON_GUIDE + GC_BUTTON_A") on
	 * player n ("JOY1_AXIS_0, GC1_BUTTON_GUIDE + GC1_BUTTON_A"), and one
	 * of player n's back */
	static std::string to_player (const std::string &value, int player);
	static std::string from_player (const std::string &value, int player);

	/* Written to the user's folder (devices/<name>.ini), the profiles read
	 * again; its path, "" when it could not be written */
	std::string	 save (const DeviceProfile &profile);

private:
	DeviceProfiles () = default;
	void		 read_dir (const std::string &dir, bool user);

	std::vector<DeviceProfile> all;
	std::string	 user;
};

} /* namespace hl */

#endif /* __HL_DEVICEPROFILES_H__ */
