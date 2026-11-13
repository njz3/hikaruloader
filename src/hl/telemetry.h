/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Supermodel's Src/Telemetry/Telemetry.h, Copyright 2003-2026
 * The Supermodel Team (GPL v3 or later).
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

#ifndef __HL_TELEMETRY_H__
#define __HL_TELEMETRY_H__

/*
 * What a racing game knows about its vehicles and the race, read out of its
 * RAM each frame: for a motion simulator, a dashboard on the wheel, the force
 * feedback, an autopilot. Served as one JSON line a frame, with the keys
 * sm2-emu (Model 2) and Supermodel (Model 3) serve, so that one client reads
 * the three; and as Forza Motorsport 7's "Data Out" datagrams (the Dash
 * format), which motion and dashboard software reads. Each game has its own
 * decoder, beside its machine (hikaru-telemetry.cpp, gaelco-telemetry.cpp).
 *
 * Units: metres, seconds, radians, m/s, m/s^2. The world: y up, the
 * vehicle's right is up x forward (a decoder mirrors a game whose world is
 * the other way), so that the heading, atan2 (dx, dz) of the nose, grows as
 * it turns right; pitch > 0 nose up, roll > 0 leaning right. A value the game
 * does not give is NaN (or UNKNOWN): null in the JSON.
 */

#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hl {

class Machine;
class FfbEffects;

namespace telemetry {

/* An integer the game does not give */
constexpr int UNKNOWN = INT_MIN;

/* The machines' frames a second */
constexpr double FPS = 60.0;

/* One wheel: front left, front right, rear left, rear right */
struct Wheel {
	float susp = NAN;		/* suspension travel, m, > 0 compressed */
	int contact = UNKNOWN;		/* 1 on the ground */
	std::string surface;		/* "asphalt", "dirt"...; empty unknown */
	float slip = NAN;		/* 0 gripping, 1 sliding */
	float spin = NAN;		/* rad/s */
};

/* One vehicle */
struct Vehicle {
	float x = 0, y = 0, z = 0;
	float heading = 0;		/* atan2 (dx, dz) of the nose, + turning right */
	float pitch = 0;		/* + nose up */
	float roll = 0;			/* + leaning right */
	/* World from vehicle, row-major: its columns right, up, forward */
	std::array<float, 9> rotation { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
	float speed = NAN;		/* m/s, the game's own */
	float vx = 0, vy = 0, vz = 0;
	float ax = 0, ay = 0, az = 0;
	/* In the vehicle's frame: speeding up, towards its right, up */
	float along = NAN, alat = NAN, avert = NAN;
	/* Turning right, nose rising, rolling right (rad/s) */
	float yaw_rate = NAN, pitch_rate = NAN, roll_rate = NAN;
	float ground = NAN;		/* height above the ground, m */
	int airborne = UNKNOWN;
	float impact = NAN;		/* a collision's change of speed this frame, m/s */
	std::array<Wheel, 4> wheels {};
	int gear = UNKNOWN;		/* -1 reverse, 0 neutral, 1..N */
	int gears = UNKNOWN;
	float rpm = NAN, rpm_max = NAN, rpm_idle = NAN;
	int lap = 0;			/* 1 on the first lap */
	int sector = 0;
	float progress = 0;		/* laps driven, fractional */
	int position = 0;		/* 1 leading */
	float damage = NAN;		/* 0..1 */
	float gap_ahead = NAN, gap_behind = NAN, dist_ahead = NAN, dist_behind = NAN;
	/* As the game reads them: 0..1; steer -1..1, +1 left */
	float throttle = NAN, brake = NAN, clutch = NAN, handbrake = NAN, steer = NAN;
	/* A game's own values, sent under their names in the vehicle's object
	 * (Star Wars: Racer Arcade's engines' thrust...): NaN null */
	std::vector<std::pair<std::string, float>> extra;

	/* The decoder read vx, vy, vz from the game (else from the
	 * positions); not sent */
	bool has_velocity = false;
};

/* What the game is doing: a motion platform parks unless it is RACE */
enum class State { UNKNOWN, ATTRACT, SELECT, RACE, PAUSE, OVER, REPLAY };

const char *state_name (State state);

struct Frame {
	uint64_t frame = 0;
	std::string game;
	std::string track;
	int sectors = 0;
	int laps = 0;			/* the race's laps; 0 unknown */
	std::string transmission;	/* "AT", "MT", or empty */
	bool racing = false;
	bool finished = false;
	State state = State::UNKNOWN;
	/* Clocks as the game shows them, s */
	float time_left = NAN, lap_time = NAN, last_lap = NAN, best_lap = NAN, race_time = NAN;
	int checkpoint = UNKNOWN;
	int stage = UNKNOWN;
	std::string car, difficulty;
	float lap_length = NAN;		/* m, for the gaps; not sent */
	Vehicle player;
	std::vector<Vehicle> others;
	std::array<float, 9> camera { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
};

/* Fills a vehicle's rotation, heading, pitch and roll from its right, up and
 * forward vectors (world, y up) */
void set_orientation (Vehicle &v, const float right[3], const float up[3], const float forward[3]);

/* How the force feedback made from a game's frames scales with its vehicle
 * (hl::FfbEffects); the defaults are Star Wars: Racer Arcade's pods */
struct FfbScale {
	float top_speed = 170.0f;	/* m/s: the centring at its stiffest */
	float hard_turn = 21.0f;	/* m/s^2 sideways: the turn's weight at its most */
	float bump = 15.0f;		/* m/s^2 off the vertical average: a bump */
};

/* A game's decoder: reads one frame from the machine, false when the game is
 * in no state to say anything */
class Decoder {
public:
	virtual ~Decoder () {}
	virtual bool decode (Machine *mach, uint64_t frame, Frame &out) = 0;
	/* The game's cabinet has no motor: its force feedback is made from
	 * the frames (hl::FfbEffects) */
	virtual bool makes_ffb () const { return false; }
	virtual FfbScale ffb_scale () const { return FfbScale (); }
};

/* The decoder for the machine's game, or NULL for a game not decoded */
std::unique_ptr<Decoder> make_decoder (Machine *mach);

/* What is worked out from the decoded frames: velocities and accelerations
 * from the positions (unless the game gives them), the accelerations in the
 * vehicle's frame, the rates, the impacts, airborne, the gaps */
class Deriver {
public:
	void derive (Frame &out);

private:
	/* A vehicle's last move and turn, the reference of the next (held while
	 * it does not move: a game at 30 Hz) */
	struct Last {
		float x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0, ax = 0, ay = 0, az = 0;
		float heading = 0, pitch = 0, roll = 0, yaw_rate = 0, pitch_rate = 0, roll_rate = 0;
		uint64_t frame = 0, angle_frame = 0;
		int place_held = 0, angles_held = 0;
		bool valid = false;
	};
	std::vector<Last> last;
};

/* What a client asks for: the keys (dotted paths, "player.speed",
 * "others.x", "player.wheels.0.susp"; empty for all) and the other vehicles
 * (the nearest along the track, -1 all) */
struct Selection {
	std::vector<std::string> fields;
	int others = -1;
};

/* One frame as a JSON object on one line, without the line feed */
std::string to_json (const Frame &frame, const Selection &selection = Selection ());

/* The keys this frame fills (their values known), as a JSON array */
std::string known_keys (const Frame &frame);

/* One frame as Forza Motorsport 7's "Car Dash" datagram, little-endian */
constexpr size_t FORZA_DASH_SIZE = 311;
std::array<uint8_t, FORZA_DASH_SIZE> forza_dash (const Frame &frame);

/* The service: the game's decoder, its derived values, the TCP server and
 * the Forza datagrams ([Telemetry] PORT and FORZA), and the agent's tel */
class Service {
public:
	static Service	&get ();

	/* A game started, or none (NULL) */
	void		 start (Machine *mach);
	void		 stop ();
	/* After each frame run; paused, the last frame again as PAUSE */
	void		 on_frame (uint64_t frame);
	void		 on_pause ();
	/* The last frame's JSON, "tel none" without a decoder */
	std::string	 json (const Selection &selection) const;
	bool		 decoded () const { return decoder != NULL; }

	~Service ();

private:
	Service () = default;

	struct Server;
	Machine		*mach = NULL;
	std::unique_ptr<Decoder> decoder;
	std::unique_ptr<Server> server;
	std::unique_ptr<FfbEffects> effects;
	Deriver		 deriver;
	Frame		 last;
	bool		 have_last = false;
	uint64_t	 pause_ticks = 0;
};

} /* namespace telemetry */
} /* namespace hl */

#endif /* __HL_TELEMETRY_H__ */
