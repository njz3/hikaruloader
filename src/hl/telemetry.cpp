/*
 * Hikaruloader
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from Supermodel's Src/Telemetry/Telemetry.cpp and
 * Src/OSD/SDL/TelemetryServer.cpp, Copyright 2003-2026 The Supermodel Team
 * (GPL v3 or later).
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

/* The frames' derived values, their JSON and their Forza datagrams; the TCP
 * server and the UDP datagrams, in the emulation's thread, never waiting. */

/* Before windows.h, which the other headers may bring */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <unistd.h>
#define closesocket close
#define ioctlsocket ioctl
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>

#include "hl/telemetry.h"
#include "hl/ffb-effects.h"
#include "hl/config.h"
#include "hl/machine.h"
#include "hl/games.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace hl {
namespace telemetry {

const char *
state_name (State state)
{
	switch (state) {
	case State::ATTRACT:	return "attract";
	case State::SELECT:	return "select";
	case State::RACE:	return "race";
	case State::PAUSE:	return "pause";
	case State::OVER:	return "over";
	case State::REPLAY:	return "replay";
	default:		return "unknown";
	}
}

void
set_orientation (Vehicle &v, const float right[3], const float up[3], const float forward[3])
{
	for (int r = 0; r < 3; r++) {
		v.rotation[r * 3 + 0] = right[r];
		v.rotation[r * 3 + 1] = up[r];
		v.rotation[r * 3 + 2] = forward[r];
	}
	v.heading = std::atan2 (forward[0], forward[2]);
	v.pitch = std::asin (std::clamp (forward[1], -1.0f, 1.0f));
	v.roll = std::atan2 (-right[1], up[1]);
}

/****************************************************************************
 Derived values
****************************************************************************/

static float
wrap (float a)
{
	const float pi = 3.14159265f;
	while (a > pi)
		a -= 2 * pi;
	while (a < -pi)
		a += 2 * pi;
	return a;
}

void
Deriver::derive (Frame &out)
{
	const size_t n = 1 + out.others.size ();

	if (last.size () < n)
		last.resize (n);
	for (size_t i = 0; i < n; i++) {
		Vehicle &v = i == 0 ? out.player : out.others[i - 1];
		Last &l = last[i];

		/* A game that moves a vehicle every other frame leaves it where it
		 * was in between: a position the same to the bit holds the last
		 * velocity and acceleration, up to 2 frames, and the next move is
		 * taken over the frames since the last one. Longer, it stands */
		bool same_place = l.valid && v.x == l.x && v.y == l.y && v.z == l.z && out.frame > l.frame;
		if (same_place && l.place_held < 2) {
			l.place_held++;
			if (!v.has_velocity) {
				v.vx = l.vx; v.vy = l.vy; v.vz = l.vz;
			}
			v.ax = l.ax; v.ay = l.ay; v.az = l.az;
			if (std::isnan (v.impact))
				v.impact = 0;
		} else {
			float dt = l.valid && out.frame > l.frame ? float ((out.frame - l.frame) / FPS) : 0.0f;
			/* Faster than 150 m/s, or than twice the game's own
			 * velocity (a pod races at 170 m/s): put elsewhere (a crash
			 * put back, a new race), the velocity kept, no
			 * acceleration */
			float moved = std::sqrt ((v.x - l.x) * (v.x - l.x) + (v.y - l.y) * (v.y - l.y) +
			                         (v.z - l.z) * (v.z - l.z));
			float limit = 150.0f;
			if (v.has_velocity)
				limit = std::max (limit, 2.0f * std::sqrt (v.vx * v.vx + v.vy * v.vy + v.vz * v.vz) + 50.0f);
			bool teleported = dt > 0 && moved / dt > limit;
			bool impact_known = !std::isnan (v.impact);
			if (dt > 0 && dt < 0.5f && !teleported) {
				if (!v.has_velocity) {
					v.vx = (v.x - l.x) / dt;
					v.vy = (v.y - l.y) / dt;
					v.vz = (v.z - l.z) / dt;
				}
				v.ax = (v.vx - l.vx) / dt;
				v.ay = (v.vy - l.vy) / dt;
				v.az = (v.vz - l.vz) / dt;
				/* An impact: more than 8 g, and not a jump of the speed
				 * itself (a vehicle put back on the track); the game's
				 * own when it tells */
				float a = std::sqrt (v.ax * v.ax + v.ay * v.ay + v.az * v.az);
				float dv = a * dt;
				if (!impact_known)
					v.impact = a > 80.0f && dv < 50.0f ? dv : 0.0f;
			} else {
				if (!v.has_velocity) {
					v.vx = teleported ? l.vx : 0.0f;
					v.vy = teleported ? l.vy : 0.0f;
					v.vz = teleported ? l.vz : 0.0f;
				}
				v.ax = v.ay = v.az = 0;
				if (!impact_known)
					v.impact = 0;
			}
			l.x = v.x; l.y = v.y; l.z = v.z;
			l.vx = v.vx; l.vy = v.vy; l.vz = v.vz;
			l.ax = v.ax; l.ay = v.ay; l.az = v.az;
			l.frame = out.frame;
			l.place_held = 0;
		}

		/* The angles the same way */
		bool same_angles = l.valid && v.heading == l.heading && v.pitch == l.pitch && v.roll == l.roll &&
		                   out.frame > l.angle_frame;
		if (same_angles && l.angles_held < 2) {
			l.angles_held++;
			v.yaw_rate = l.yaw_rate; v.pitch_rate = l.pitch_rate; v.roll_rate = l.roll_rate;
		} else {
			float dt = l.valid && out.frame > l.angle_frame ? float ((out.frame - l.angle_frame) / FPS) : 0.0f;
			if (dt > 0 && dt < 0.5f) {
				/* An angle turning more than 0.5 rad a frame is not a
				 * rotation (a heading flipping at a standstill) */
				float frames = float (dt * FPS);
				auto rate = [&] (float now, float before) {
					float d = wrap (now - before);
					return std::fabs (d) > 0.5f * frames ? 0.0f : d / dt;
				};
				v.yaw_rate = rate (v.heading, l.heading);
				v.pitch_rate = rate (v.pitch, l.pitch);
				v.roll_rate = rate (v.roll, l.roll);
			} else
				v.yaw_rate = v.pitch_rate = v.roll_rate = 0;
			l.heading = v.heading; l.pitch = v.pitch; l.roll = v.roll;
			l.yaw_rate = v.yaw_rate; l.pitch_rate = v.pitch_rate; l.roll_rate = v.roll_rate;
			l.angle_frame = out.frame;
			l.angles_held = 0;
		}
		l.valid = true;

		/* In the vehicle's frame: the rotation's columns are its right,
		 * up and forward */
		const auto &R = v.rotation;
		v.alat = v.ax * R[0] + v.ay * R[3] + v.az * R[6];
		v.avert = v.ax * R[1] + v.ay * R[4] + v.az * R[7];
		v.along = v.ax * R[2] + v.ay * R[5] + v.az * R[8];
		if (!std::isnan (v.ground) && v.airborne == UNKNOWN)
			v.airborne = v.ground > 0.25f ? 1 : 0;
	}

	/* The gaps along the track, from the laps driven */
	if (!std::isnan (out.lap_length)) {
		for (size_t i = 0; i < n; i++) {
			Vehicle &v = i == 0 ? out.player : out.others[i - 1];
			float ahead = NAN, behind = NAN;
			for (size_t j = 0; j < n; j++) {
				if (j == i)
					continue;
				const Vehicle &w = j == 0 ? out.player : out.others[j - 1];
				float d = (w.progress - v.progress) * out.lap_length;
				if (d >= 0 && (std::isnan (ahead) || d < ahead))
					ahead = d;
				if (d < 0 && (std::isnan (behind) || -d < behind))
					behind = -d;
			}
			float speed = std::max (std::isnan (v.speed) ? std::sqrt (v.vx * v.vx + v.vz * v.vz) : v.speed, 1.0f);
			v.dist_ahead = ahead;
			v.dist_behind = behind;
			v.gap_ahead = std::isnan (ahead) ? NAN : ahead / speed;
			v.gap_behind = std::isnan (behind) ? NAN : behind / speed;
		}
	}
}

/****************************************************************************
 JSON
****************************************************************************/

namespace {

class Json {
public:
	explicit Json (const Selection &selection) : sel (selection) {}

	/* The key at path is sent: everything is, or a field names it or one
	 * of its parents */
	bool wants (const std::string &path) const
	{
		if (sel.fields.empty ())
			return true;
		for (auto &f : sel.fields)
			if (f == path || (path.size () > f.size () && path.compare (0, f.size (), f) == 0 &&
			                  path[f.size ()] == '.'))
				return true;
		return false;
	}

	/* Something under path is sent */
	bool wants_under (const std::string &path) const
	{
		if (wants (path))
			return true;
		for (auto &f : sel.fields)
			if (f.size () > path.size () && f.compare (0, path.size (), path) == 0 && f[path.size ()] == '.')
				return true;
		return false;
	}

	void key (const char *k)
	{
		separator ();
		s += '"';
		s += k;
		s += "\":";
	}

	void open (char c) { s += c; first.push_back (true); }
	void close (char c) { s += c; first.pop_back (); }

	void separator ()
	{
		if (!first.empty ()) {
			if (!first.back ())
				s += ',';
			first.back () = false;
		}
	}

	void number (float x)
	{
		if (!std::isfinite (x))
			s += "null";
		else {
			char buf[32];
			snprintf (buf, sizeof (buf), "%.7g", double (x));
			s += buf;
		}
	}

	void integer (int x)
	{
		if (x == UNKNOWN)
			s += "null";
		else
			s += std::to_string (x);
	}

	void string (const std::string &t)
	{
		s += '"';
		for (char c : t) {
			if (c == '"' || c == '\\')
				s += '\\';
			if (uint8_t (c) >= 0x20)
				s += c;
		}
		s += '"';
	}

	void boolean (bool b) { s += b ? "true" : "false"; }

	/* The leaf key at prefix + key, if sent */
	template <typename F> void leaf (const std::string &prefix, const char *k, F write)
	{
		if (wants (prefix + k)) {
			key (k);
			write ();
		}
	}

	std::string s;

private:
	const Selection &sel;
	std::vector<bool> first;
};

void
write_matrix (Json &j, const std::array<float, 9> &m)
{
	j.open ('[');
	for (float x : m) {
		j.separator ();
		j.number (x);
	}
	j.close (']');
}

void
write_vehicle (Json &j, const Vehicle &v, const std::string &p)
{
	j.open ('{');
	j.leaf (p, "x", [&] { j.number (v.x); });
	j.leaf (p, "y", [&] { j.number (v.y); });
	j.leaf (p, "z", [&] { j.number (v.z); });
	j.leaf (p, "heading", [&] { j.number (v.heading); });
	j.leaf (p, "pitch", [&] { j.number (v.pitch); });
	j.leaf (p, "roll", [&] { j.number (v.roll); });
	j.leaf (p, "rotation", [&] { write_matrix (j, v.rotation); });
	j.leaf (p, "speed", [&] { j.number (v.speed); });
	j.leaf (p, "vx", [&] { j.number (v.vx); });
	j.leaf (p, "vy", [&] { j.number (v.vy); });
	j.leaf (p, "vz", [&] { j.number (v.vz); });
	j.leaf (p, "ax", [&] { j.number (v.ax); });
	j.leaf (p, "ay", [&] { j.number (v.ay); });
	j.leaf (p, "az", [&] { j.number (v.az); });
	j.leaf (p, "along", [&] { j.number (v.along); });
	j.leaf (p, "alat", [&] { j.number (v.alat); });
	j.leaf (p, "avert", [&] { j.number (v.avert); });
	j.leaf (p, "yaw_rate", [&] { j.number (v.yaw_rate); });
	j.leaf (p, "pitch_rate", [&] { j.number (v.pitch_rate); });
	j.leaf (p, "roll_rate", [&] { j.number (v.roll_rate); });
	j.leaf (p, "ground", [&] { j.number (v.ground); });
	j.leaf (p, "airborne", [&] { if (v.airborne == UNKNOWN) j.s += "null"; else j.boolean (v.airborne != 0); });
	j.leaf (p, "impact", [&] { j.number (v.impact); });
	if (j.wants_under (p + "wheels")) {
		j.key ("wheels");
		j.open ('[');
		for (int w = 0; w < 4; w++) {
			std::string wp = p + "wheels." + std::to_string (w) + ".";
			const Wheel &wh = v.wheels[w];
			j.separator ();
			j.open ('{');
			j.leaf (wp, "susp", [&] { j.number (wh.susp); });
			j.leaf (wp, "contact", [&] { if (wh.contact == UNKNOWN) j.s += "null"; else j.boolean (wh.contact != 0); });
			j.leaf (wp, "surface", [&] { j.string (wh.surface.empty () ? "unknown" : wh.surface); });
			j.leaf (wp, "slip", [&] { j.number (wh.slip); });
			j.leaf (wp, "spin", [&] { j.number (wh.spin); });
			j.close ('}');
		}
		j.close (']');
	}
	j.leaf (p, "gear", [&] { j.integer (v.gear); });
	j.leaf (p, "gears", [&] { j.integer (v.gears); });
	j.leaf (p, "rpm", [&] { j.number (v.rpm); });
	j.leaf (p, "rpm_max", [&] { j.number (v.rpm_max); });
	j.leaf (p, "rpm_idle", [&] { j.number (v.rpm_idle); });
	j.leaf (p, "lap", [&] { j.integer (v.lap); });
	j.leaf (p, "sector", [&] { j.integer (v.sector); });
	j.leaf (p, "progress", [&] { j.number (v.progress); });
	j.leaf (p, "position", [&] { j.integer (v.position); });
	j.leaf (p, "damage", [&] { j.number (v.damage); });
	j.leaf (p, "gap_ahead", [&] { j.number (v.gap_ahead); });
	j.leaf (p, "gap_behind", [&] { j.number (v.gap_behind); });
	j.leaf (p, "dist_ahead", [&] { j.number (v.dist_ahead); });
	j.leaf (p, "dist_behind", [&] { j.number (v.dist_behind); });
	j.leaf (p, "throttle", [&] { j.number (v.throttle); });
	j.leaf (p, "brake", [&] { j.number (v.brake); });
	j.leaf (p, "clutch", [&] { j.number (v.clutch); });
	j.leaf (p, "handbrake", [&] { j.number (v.handbrake); });
	j.leaf (p, "steer", [&] { j.number (v.steer); });
	for (const auto &e : v.extra)
		j.leaf (p, e.first.c_str (), [&] { j.number (e.second); });
	j.close ('}');
}

/* The other vehicles, the nearest first (along the track, or in a straight
 * line when the lap's length is unknown) */
std::vector<const Vehicle *>
nearest (const Frame &frame, int count)
{
	std::vector<const Vehicle *> cars;

	for (auto &v : frame.others)
		cars.push_back (&v);
	if (count < 0 || size_t (count) >= cars.size ())
		return cars;
	const Vehicle &p = frame.player;
	auto distance = [&] (const Vehicle *v) {
		float d = !std::isnan (frame.lap_length) ? std::fabs (v->progress - p.progress) * frame.lap_length
		                                         : std::hypot (v->x - p.x, v->z - p.z);
		return std::isnan (d) ? INFINITY : d;
	};
	std::stable_sort (cars.begin (), cars.end (),
	                  [&] (const Vehicle *a, const Vehicle *b) { return distance (a) < distance (b); });
	cars.resize (size_t (count));
	return cars;
}

} /* namespace */

std::string
to_json (const Frame &frame, const Selection &selection)
{
	Json j (selection);
	const std::string p;

	j.open ('{');
	j.leaf (p, "frame", [&] { j.s += std::to_string (frame.frame); });
	j.leaf (p, "game", [&] { j.string (frame.game); });
	j.leaf (p, "track", [&] { j.string (frame.track); });
	j.leaf (p, "sectors", [&] { j.integer (frame.sectors); });
	j.leaf (p, "laps", [&] { j.integer (frame.laps); });
	j.leaf (p, "transmission", [&] { j.string (frame.transmission); });
	j.leaf (p, "racing", [&] { j.boolean (frame.racing); });
	j.leaf (p, "finished", [&] { j.boolean (frame.finished); });
	j.leaf (p, "state", [&] { j.string (state_name (frame.state)); });
	j.leaf (p, "time_left", [&] { j.number (frame.time_left); });
	j.leaf (p, "lap_time", [&] { j.number (frame.lap_time); });
	j.leaf (p, "last_lap", [&] { j.number (frame.last_lap); });
	j.leaf (p, "best_lap", [&] { j.number (frame.best_lap); });
	j.leaf (p, "race_time", [&] { j.number (frame.race_time); });
	j.leaf (p, "checkpoint", [&] { j.integer (frame.checkpoint); });
	j.leaf (p, "stage", [&] { j.integer (frame.stage); });
	j.leaf (p, "car", [&] { j.string (frame.car); });
	j.leaf (p, "difficulty", [&] { j.string (frame.difficulty); });
	j.leaf (p, "camera", [&] { write_matrix (j, frame.camera); });
	if (j.wants_under ("player")) {
		j.key ("player");
		write_vehicle (j, frame.player, "player.");
	}
	if (selection.others != 0 && j.wants_under ("others")) {
		j.key ("others");
		j.open ('[');
		for (const Vehicle *v : nearest (frame, selection.others)) {
			j.separator ();
			write_vehicle (j, *v, "others.");
		}
		j.close (']');
	}
	j.close ('}');
	return j.s;
}

std::string
known_keys (const Frame &frame)
{
	std::vector<std::string> keys = { "frame", "game", "racing", "finished", "state" };
	auto add = [&] (const std::string &key, bool known) { if (known) keys.push_back (key); };

	add ("track", !frame.track.empty ());
	add ("sectors", frame.sectors != 0);
	add ("laps", frame.laps != 0);
	add ("transmission", !frame.transmission.empty ());
	add ("time_left", !std::isnan (frame.time_left));
	add ("lap_time", !std::isnan (frame.lap_time));
	add ("last_lap", !std::isnan (frame.last_lap));
	add ("best_lap", !std::isnan (frame.best_lap));
	add ("race_time", !std::isnan (frame.race_time));
	add ("checkpoint", frame.checkpoint != UNKNOWN);
	add ("stage", frame.stage != UNKNOWN);
	add ("car", !frame.car.empty ());
	add ("difficulty", !frame.difficulty.empty ());
	const Vehicle &v = frame.player;
	for (const char *k : { "player.x", "player.y", "player.z", "player.heading", "player.pitch", "player.roll",
	                       "player.rotation", "player.vx", "player.vy", "player.vz", "player.ax", "player.ay",
	                       "player.az", "player.along", "player.alat", "player.avert", "player.yaw_rate",
	                       "player.pitch_rate", "player.roll_rate", "player.impact", "player.lap",
	                       "player.progress", "player.position" })
		keys.push_back (k);
	add ("player.speed", !std::isnan (v.speed));
	add ("player.ground", !std::isnan (v.ground));
	add ("player.airborne", v.airborne != UNKNOWN);
	add ("player.gear", v.gear != UNKNOWN);
	add ("player.gears", v.gears != UNKNOWN);
	add ("player.rpm", !std::isnan (v.rpm));
	add ("player.rpm_max", !std::isnan (v.rpm_max));
	add ("player.rpm_idle", !std::isnan (v.rpm_idle));
	add ("player.damage", !std::isnan (v.damage));
	add ("player.gap_ahead", !std::isnan (frame.lap_length));
	add ("player.gap_behind", !std::isnan (frame.lap_length));
	add ("player.dist_ahead", !std::isnan (frame.lap_length));
	add ("player.dist_behind", !std::isnan (frame.lap_length));
	add ("player.throttle", !std::isnan (v.throttle));
	add ("player.brake", !std::isnan (v.brake));
	add ("player.steer", !std::isnan (v.steer));
	add ("player.wheels.0.susp", !std::isnan (v.wheels[0].susp));
	add ("player.wheels.0.contact", v.wheels[0].contact != UNKNOWN);
	add ("player.wheels.0.surface", !v.wheels[0].surface.empty ());
	for (const auto &e : v.extra)
		keys.push_back ("player." + e.first);
	add ("others", !frame.others.empty ());
	std::string s = "[";
	for (size_t i = 0; i < keys.size (); i++)
		s += (i ? ",\"" : "\"") + keys[i] + "\"";
	return s + "]";
}

/****************************************************************************
 Forza Motorsport 7's "Car Dash" datagram
****************************************************************************/

namespace {

class Packet {
public:
	void f32 (float x)
	{
		uint32_t u;
		if (!std::isfinite (x))
			x = 0;
		memcpy (&u, &x, 4);
		u32 (u);
	}
	void s32 (int32_t x) { u32 (uint32_t (x)); }
	void u32 (uint32_t u)
	{
		for (int i = 0; i < 4; i++)
			data[pos++] = uint8_t (u >> (8 * i));
	}
	void u16 (uint16_t u)
	{
		data[pos++] = uint8_t (u);
		data[pos++] = uint8_t (u >> 8);
	}
	void u8 (uint8_t u) { data[pos++] = u; }

	std::array<uint8_t, FORZA_DASH_SIZE> data {};
	size_t pos = 0;
};

uint8_t
byte01 (float x)
{
	return std::isfinite (x) ? uint8_t (std::lround (std::clamp (x, 0.0f, 1.0f) * 255)) : 0;
}

/* A game's own value, by name: NaN when it has none */
float
extra (const Vehicle &v, const char *name)
{
	for (const auto &e : v.extra)
		if (e.first == name)
			return e.second;
	return NAN;
}

} /* namespace */

std::array<uint8_t, FORZA_DASH_SIZE>
forza_dash (const Frame &frame)
{
	const Vehicle &v = frame.player;
	const auto &R = v.rotation;
	/* In the vehicle's frame: X its right, Y up, Z forward */
	float bx = v.vx * R[0] + v.vy * R[3] + v.vz * R[6];
	float by = v.vx * R[1] + v.vy * R[4] + v.vz * R[7];
	float bz = v.vx * R[2] + v.vy * R[5] + v.vz * R[8];
	Packet p;

	/* The "Sled" part */
	p.s32 (frame.state == State::RACE ? 1 : 0);
	p.u32 (uint32_t (double (frame.frame) * 1000.0 / FPS));
	p.f32 (v.rpm_max);
	p.f32 (v.rpm_idle);
	p.f32 (v.rpm);
	p.f32 (v.alat); p.f32 (v.avert); p.f32 (v.along);
	p.f32 (bx); p.f32 (by); p.f32 (bz);
	p.f32 (v.pitch_rate); p.f32 (v.yaw_rate); p.f32 (v.roll_rate);
	p.f32 (v.heading); p.f32 (v.pitch); p.f32 (v.roll);
	for (auto &w : v.wheels)		/* NormalizedSuspensionTravel: 0.5 at rest */
		p.f32 (std::isnan (w.susp) ? 0.0f : std::clamp (0.5f + w.susp / 0.2f, 0.0f, 1.0f));
	for (auto &w : v.wheels)		/* TireSlipRatio */
		p.f32 (w.slip);
	for (auto &w : v.wheels)		/* WheelRotationSpeed */
		p.f32 (w.spin);
	for (int i = 0; i < 4; i++)		/* WheelOnRumbleStrip */
		p.s32 (0);
	for (int i = 0; i < 4; i++)		/* WheelInPuddleDepth */
		p.f32 (0);
	for (int i = 0; i < 4; i++)		/* SurfaceRumble */
		p.f32 (0);
	for (int i = 0; i < 4; i++)		/* TireSlipAngle */
		p.f32 (0);
	for (auto &w : v.wheels)		/* TireCombinedSlip */
		p.f32 (w.slip);
	for (auto &w : v.wheels)		/* SuspensionTravelMeters */
		p.f32 (w.susp);
	for (int i = 0; i < 5; i++)		/* CarOrdinal, CarClass, CarPerformanceIndex, DrivetrainType, NumCylinders */
		p.s32 (0);
	/* The "Dash" part */
	p.f32 (v.x); p.f32 (v.y); p.f32 (v.z);
	p.f32 (std::isnan (v.speed) ? std::sqrt (v.vx * v.vx + v.vy * v.vy + v.vz * v.vz) : v.speed);
	p.f32 (0); p.f32 (0);			/* Power, Torque */
	for (int i = 0; i < 4; i++)		/* TireTemp */
		p.f32 (0);
	p.f32 (extra (v, "boost"));		/* Boost */
	p.f32 (1);				/* Fuel */
	p.f32 (std::isnan (frame.lap_length) ? 0.0f : v.progress * frame.lap_length);
	p.f32 (frame.best_lap);
	p.f32 (frame.last_lap);
	p.f32 (frame.lap_time);
	p.f32 (frame.race_time);
	p.u16 (uint16_t (std::max (v.lap - 1, 0)));
	p.u8 (uint8_t (std::clamp (v.position, 0, 255)));
	p.u8 (byte01 (v.throttle));
	p.u8 (byte01 (v.brake));
	p.u8 (byte01 (v.clutch));
	p.u8 (byte01 (v.handbrake));
	/* 0 reverse, 11 neutral or unknown */
	p.u8 (v.gear == -1 ? 0 : v.gear >= 1 && v.gear <= 10 ? uint8_t (v.gear) : 11);
	p.u8 (uint8_t (int8_t (std::isnan (v.steer) ? 0 : std::lround (-std::clamp (v.steer, -1.0f, 1.0f) * 127))));
	p.u8 (0);				/* NormalizedDrivingLine */
	p.u8 (0);				/* NormalizedAIBrakeDifference */
	return p.data;
}

/****************************************************************************
 The server: TCP clients, the Forza datagrams
****************************************************************************/

static const uintptr_t NO_SOCKET = (uintptr_t) -1;
static const size_t MAX_CLIENTS = 16;
/* A client this far behind reads nothing: dropped */
static const size_t MAX_PENDING = 256 * 1024;

static void
set_nonblocking (uintptr_t sock)
{
#ifdef _WIN32
	u_long mode = 1;
#else
	int mode = 1;
#endif
	ioctlsocket (sock, FIONBIO, &mode);
}

static bool
would_block ()
{
#ifdef _WIN32
	return WSAGetLastError () == WSAEWOULDBLOCK;
#else
	return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

static std::string
trim (const std::string &s)
{
	size_t a = s.find_first_not_of (" \t\r");
	size_t b = s.find_last_not_of (" \t\r");
	return a == std::string::npos ? std::string () : s.substr (a, b - a + 1);
}

struct Service::Server {
	struct Client {
		uintptr_t sock = NO_SOCKET;
		std::string input, pending;
		Selection selection;
		unsigned every = 1;
	};

	bool winsock = false;
	uintptr_t listener = NO_SOCKET;
	unsigned port = 0;
	std::vector<Client> clients;
	uintptr_t udp = NO_SOCKET;
	struct sockaddr_in forza {};
	bool forza_on = false;

	~Server () { close (); }

	bool net ()
	{
#ifdef _WIN32
		if (!winsock) {
			WSADATA wsa;
			if (WSAStartup (MAKEWORD (2, 2), &wsa)) {
				HL_ERROR ("telemetry: Winsock cannot start");
				return false;
			}
			winsock = true;
		}
#endif
		return true;
	}

	bool open (unsigned p)
	{
		struct sockaddr_in addr;

		if (!net ())
			return false;
		listener = (uintptr_t) socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listener == NO_SOCKET)
			return false;
#ifndef _WIN32
		int yes = 1;
		setsockopt (listener, SOL_SOCKET, SO_REUSEADDR, (const char *) &yes, sizeof (yes));
#endif
		memset (&addr, 0, sizeof (addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl (INADDR_ANY);
		addr.sin_port = htons ((uint16_t) p);
		if (bind (listener, (struct sockaddr *) &addr, sizeof (addr)) || listen (listener, 4)) {
			HL_ERROR ("telemetry: cannot listen on TCP port %u (in use?): [Telemetry] PORT to change it", p);
			closesocket (listener);
			listener = NO_SOCKET;
			return false;
		}
		set_nonblocking (listener);
		port = p;
		HL_PRINT ("telemetry: on TCP port %u", p);
		return true;
	}

	void close ()
	{
		for (Client &c : clients)
			if (c.sock != NO_SOCKET)
				closesocket (c.sock);
		clients.clear ();
		if (listener != NO_SOCKET)
			closesocket (listener);
		listener = NO_SOCKET;
		if (udp != NO_SOCKET)
			closesocket (udp);
		udp = NO_SOCKET;
		forza_on = false;
#ifdef _WIN32
		if (winsock)
			WSACleanup ();
		winsock = false;
#endif
	}

	/* HOST:PORT, a name or an address */
	bool set_forza (const std::string &host_port)
	{
		size_t colon = host_port.rfind (':');
		struct addrinfo hints, *found = NULL;
		int p;

		if (colon == std::string::npos || !net ())
			return false;
		p = atoi (host_port.c_str () + colon + 1);
		if (p <= 0 || p > 65535)
			return false;
		memset (&hints, 0, sizeof (hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		if (getaddrinfo (host_port.substr (0, colon).c_str (), NULL, &hints, &found) || !found)
			return false;
		memcpy (&forza, found->ai_addr, sizeof (forza));
		freeaddrinfo (found);
		forza.sin_port = htons ((uint16_t) p);
		if (udp == NO_SOCKET) {
			udp = (uintptr_t) socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			if (udp == NO_SOCKET)
				return false;
			set_nonblocking (udp);
		}
		forza_on = true;
		HL_PRINT ("telemetry: Forza Data Out to %s", host_port.c_str ());
		return true;
	}

	bool wanted () const
	{
		return forza_on || std::any_of (clients.begin (), clients.end (),
		                                [] (const Client &c) { return c.sock != NO_SOCKET; });
	}

	void flush (Client &c)
	{
		while (c.sock != NO_SOCKET && !c.pending.empty ()) {
			int sent = ::send (c.sock, c.pending.data (), (int) c.pending.size (), MSG_NOSIGNAL);
			if (sent <= 0) {
				if (sent < 0 && would_block ())
					break;
				closesocket (c.sock);
				c.sock = NO_SOCKET;
				HL_PRINT ("telemetry: a client left");
				return;
			}
			c.pending.erase (0, sent);
		}
		if (c.sock != NO_SOCKET && c.pending.size () > MAX_PENDING) {
			closesocket (c.sock);
			c.sock = NO_SOCKET;
			HL_ERROR ("telemetry: a client that read nothing was dropped");
		}
	}

	/* A client's line: the commands sm2-emu and Supermodel take */
	std::string command (Client &c, const std::string &line, const Frame *last)
	{
		std::istringstream in (line);
		std::string cmd, arg;

		in >> cmd;
		std::getline (in, arg);
		arg = trim (arg);
		if (cmd == "fields") {
			c.selection.fields.clear ();
			if (arg != "all") {
				std::stringstream list (arg);
				std::string f;
				while (std::getline (list, f, ',')) {
					f = trim (f);
					if (!f.empty ())
						c.selection.fields.push_back (f);
				}
			}
			return "{\"ok\":\"fields\"}";
		}
		if (cmd == "others") {
			if (arg == "all")
				c.selection.others = -1;
			else {
				char *end = NULL;
				long n = strtol (arg.c_str (), &end, 10);
				if (arg.empty () || *end || n < 0)
					return "{\"error\":\"usage: others N | others all\"}";
				c.selection.others = int (n);
			}
			return "{\"ok\":\"others\"}";
		}
		if (cmd == "every") {
			long n = atol (arg.c_str ());
			if (n < 1)
				return "{\"error\":\"usage: every N, N at least 1\"}";
			c.every = unsigned (n);
			return "{\"ok\":\"every\"}";
		}
		if (cmd == "udp") {
			std::istringstream a (arg);
			std::string format, target;
			a >> format >> target;
			if (format == "off") {
				forza_on = false;
				return "{\"ok\":\"udp\"}";
			}
			if (format != "forza" || !set_forza (target))
				return "{\"error\":\"usage: udp forza HOST:PORT | udp off\"}";
			return "{\"ok\":\"udp\"}";
		}
		if (cmd == "list")
			return std::string ("{\"ok\":\"list\",\"keys\":") + (last ? known_keys (*last) : "[]") + "}";
		return "{\"error\":\"unknown command: " + cmd + "\"}";
	}

	void poll (const Frame *last)
	{
		struct timeval none = { 0, 0 };
		fd_set readable;
		uintptr_t top;

		if (listener == NO_SOCKET)
			return;
		FD_ZERO (&readable);
		FD_SET (listener, &readable);
		top = listener;
		for (Client &c : clients)
			if (c.sock != NO_SOCKET) {
				FD_SET (c.sock, &readable);
				top = std::max (top, c.sock);
			}
		if (select ((int) top + 1, &readable, NULL, NULL, &none) <= 0)
			return;
		if (FD_ISSET (listener, &readable)) {
			uintptr_t sock = (uintptr_t) accept (listener, NULL, NULL);
			if (sock != NO_SOCKET) {
				if (clients.size () >= MAX_CLIENTS)
					closesocket (sock);
				else {
					int yes = 1;
					set_nonblocking (sock);
					setsockopt (sock, IPPROTO_TCP, TCP_NODELAY, (const char *) &yes, sizeof (yes));
					Client c;
					c.sock = sock;
					clients.push_back (c);
					HL_PRINT ("telemetry: a client connected");
				}
			}
		}
		for (Client &c : clients) {
			char buf[512];
			int got;
			size_t lf;

			if (c.sock == NO_SOCKET || !FD_ISSET (c.sock, &readable))
				continue;
			got = recv (c.sock, buf, sizeof (buf), 0);
			if (got <= 0) {
				if (got < 0 && would_block ())
					continue;
				closesocket (c.sock);
				c.sock = NO_SOCKET;
				HL_PRINT ("telemetry: a client left");
				continue;
			}
			c.input.append (buf, size_t (got));
			while ((lf = c.input.find ('\n')) != std::string::npos) {
				std::string line = trim (c.input.substr (0, lf));
				c.input.erase (0, lf + 1);
				/* An acknowledgement */
				if (line.empty () || line == "go")
					continue;
				c.pending += command (c, line, last) + "\n";
				flush (c);
			}
			if (c.input.size () > 4096)
				c.input.clear ();
		}
		clients.erase (std::remove_if (clients.begin (), clients.end (),
		                               [] (const Client &c) { return c.sock == NO_SOCKET; }),
		               clients.end ());
	}

	void publish (const Frame &frame)
	{
		/* Clients with the same selection share a line */
		std::map<std::string, std::string> lines;

		for (Client &c : clients) {
			if (c.sock == NO_SOCKET || frame.frame % c.every != 0)
				continue;
			std::string key = std::to_string (c.selection.others);
			for (auto &f : c.selection.fields)
				key += "," + f;
			auto it = lines.find (key);
			if (it == lines.end ())
				it = lines.emplace (key, to_json (frame, c.selection) + "\n").first;
			c.pending += it->second;
			flush (c);
		}
		if (forza_on && udp != NO_SOCKET) {
			auto data = forza_dash (frame);
			sendto (udp, (const char *) data.data (), (int) data.size (), 0,
			        (const struct sockaddr *) &forza, sizeof (forza));
		}
	}
};

/****************************************************************************
 The service
****************************************************************************/

Service &
Service::get ()
{
	static Service service;
	return service;
}

Service::~Service ()
{
	stop ();
}

void
Service::start (Machine *m)
{
	Config *cfg = Config::get ();

	stop ();
	mach = m;
	decoder = m ? make_decoder (m) : NULL;
	if (!decoder) {
		if (m && m->game && (cfg->telemetry_port || cfg->telemetry_forza[0]))
			HL_PRINT ("telemetry: no decoder for %s", m->game->name);
		return;
	}
	HL_PRINT ("telemetry: %s decoded", m->game ? m->game->name : "the game");
	if (decoder->makes_ffb ()) {
		effects.reset (new FfbEffects (decoder->ffb_scale ()));
		HL_PRINT ("ffb: effects made from the telemetry ([ForceFeedback] EFFECTS)");
	}
	if (!cfg->telemetry_port && !cfg->telemetry_forza[0])
		return;
	server.reset (new Server);
	if (cfg->telemetry_port)
		server->open (cfg->telemetry_port);
	if (cfg->telemetry_forza[0] && !server->set_forza (cfg->telemetry_forza))
		HL_ERROR ("telemetry: cannot send Forza Data Out to %s ([Telemetry] FORZA)", cfg->telemetry_forza);
}

void
Service::stop ()
{
	if (effects)
		effects->stop (mach ? mach->frame : 0);
	effects.reset ();
	server.reset ();
	decoder.reset ();
	mach = NULL;
	have_last = false;
	deriver = Deriver ();
}

void
Service::on_frame (uint64_t frame)
{
	if (!decoder)
		return;
	if (server)
		server->poll (have_last ? &last : NULL);
	Frame f;
	if (!decoder->decode (mach, frame, f))
		return;
	f.frame = frame;
	if (f.game.empty () && mach->game)
		f.game = mach->game->name;
	f.racing = f.state == State::RACE;
	deriver.derive (f);
	last = std::move (f);
	have_last = true;
	if (server)
		server->publish (last);
	if (effects)
		effects->frame (last);
}

void
Service::on_pause ()
{
	if (effects && mach)
		effects->stop (mach->frame);
	if (!decoder || !server)
		return;
	server->poll (have_last ? &last : NULL);
	if (!have_last)
		return;
	/* At the frame rate, however fast the paused loop goes */
	uint64_t now = (uint64_t) std::chrono::duration_cast<std::chrono::milliseconds> (
		std::chrono::steady_clock::now ().time_since_epoch ()).count ();
	if (now - pause_ticks < 17)
		return;
	pause_ticks = now;
	Frame f = last;
	f.state = State::PAUSE;
	f.racing = false;
	server->publish (f);
}

std::string
Service::json (const Selection &selection) const
{
	if (!decoder || !have_last)
		return "tel none";
	return to_json (last, selection);
}

} /* namespace telemetry */
} /* namespace hl */
