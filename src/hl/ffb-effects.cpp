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

/*
 * The effects, from what the decoder reads (hl/telemetry-games.cpp), scaled
 * to the game's vehicle (telemetry::FfbScale: Star Wars: Racer Arcade's pods
 * by default, Crazy Taxi's taxi):
 *
 * - Centring (spring) and damping (friction), stiffer with the speed up to
 *   the vehicle's top speed: a pod at 600 km/h holds its line. Lighter in
 *   the air.
 * - The turn's weight (a constant force): the lateral acceleration pushes
 *   the wheel back out of the turn, as a car's self-aligning torque, at its
 *   most in the vehicle's hard turns.
 * - A hit (the game's own, against a wall or a pod, or a change of speed
 *   past 8 g): a kick of the wheel away from the side hit (an acceleration to
 *   the right, the wall on the left: the wheel kicked to the right), as
 *   strong as the speed lost, then a shake; scraping a wall, a rough shake
 *   with the speed.
 * - The engines: a rumble with their thrust and the speed, the deluxe
 *   cabinet's bass shaker's level (the option sound board's low
 *   frequencies) over it, stronger while boosting.
 * - The centring's middle follows the engines' tilt from the cockpit
 *   ("engines_roll"), to the side they bank.
 * - The ground: a bump or a landing, the vertical acceleration leaving its
 *   own average, shakes.
 *
 * On a controller, nothing at rest nor cruising: both motors for a boost at
 * high speed, the hits, the scraping and the bumps.
 */

#include <algorithm>
#include <cmath>

#include "hl/ffb-effects.h"
#include "hl/config.h"
#include "hl/controls.h"
#include "hl/gamecontrols.h"
#include "hl/ffb.h"
#include "hl/input.h"
#include "hl/outputs.h"

namespace hl {

static float
extra (const telemetry::Vehicle &v, const char *name, float fallback = 0)
{
	for (const auto &e : v.extra)
		if (e.first == name)
			return std::isnan (e.second) ? fallback : e.second;
	return fallback;
}

static float
finite (float x, float fallback = 0)
{
	return std::isfinite (x) ? x : fallback;
}

/* The spring's centre for the engines' tilt: half the wheel's travel at their
 * 15 degrees, the most they reach */
#define SPRING_OFFSET_PER_RADIAN	(0.5f / 0.2618f)

/* Steps of 1/64: an update to the device at each change that can be felt,
 * some 100 a second at most over all the effects */
static float
step (float x)
{
	return roundf (x * 64.0f) / 64.0f;
}

void
FfbEffects::frame (const telemetry::Frame &f)
{
	const Config *cfg = Config::get ();
	const telemetry::Vehicle &p = f.player;
	const float dt = 1.0f / (float) telemetry::FPS;

	if (!cfg->ffb_effects || f.state != telemetry::State::RACE) {
		stop (f.frame);
		return;
	}
	float steering = cfg->ffb_steering / 100.0f, engines = cfg->ffb_engines / 100.0f;
	float hits = cfg->ffb_hits / 100.0f, terrain = cfg->ffb_terrain / 100.0f;
	float rumble = cfg->ffb_rumble / 100.0f;
	/* On the wheel alone, each effect's own */
	float wheel_spring = cfg->ffb_wheel_spring / 100.0f, wheel_friction = cfg->ffb_wheel_friction / 100.0f;
	float wheel_turn = cfg->ffb_wheel_turn / 100.0f, wheel_kick = cfg->ffb_wheel_kick / 100.0f;
	float wheel_engines = cfg->ffb_wheel_engines / 100.0f, wheel_boost = cfg->ffb_wheel_boost / 100.0f;
	float wheel_scrape = cfg->ffb_wheel_scrape / 100.0f, wheel_impact = cfg->ffb_wheel_impact / 100.0f;
	float wheel_bumps = cfg->ffb_wheel_bumps / 100.0f;

	/* A new race, or frames skipped (a state loaded): from rest */
	if (!playing || f.frame != last_frame + 1) {
		corner = kick = jolt = wheel_jolt = 0;
		avert_slow = finite (p.avert);
		alat_slow = finite (p.alat);
	}
	playing = true;
	last_frame = f.frame;

	float speed = finite (p.speed);
	float fast = std::min (speed / scale.top_speed, 1.0f);
	bool air = p.airborne == 1;
	float throttle = std::clamp (finite (p.throttle), 0.0f, 1.0f);
	float alat = finite (p.alat), avert = finite (p.avert), impact = finite (p.impact);
	bool hit = extra (p, "hit") > 0.5f || impact > 3.0f;
	bool wall = extra (p, "wall") > 0.5f || extra (p, "colliding") > 0.5f;
	bool boost = extra (p, "boost") > 0.5f;
	float shaker = std::clamp (extra (p, "shaker"), 0.0f, 1.0f);

	/* A hit: a kick away from the side hit, as strong as the speed lost;
	 * its side from the lateral acceleration's jump off its average, the
	 * turn's own left out */
	float jerk = alat - alat_slow;
	alat_slow += (alat - alat_slow) * 0.1f;
	if (hit) {
		float strength = std::clamp (0.35f + impact / 20.0f, 0.35f, 1.0f);
		/* Its side taken at its start (a head-on hit, none: a shake),
		 * stronger as the hit grows */
		float side = jerk >= 0 ? -1.0f : 1.0f;
		if (std::fabs (jerk) > 20.0f &&
		    (std::fabs (kick) < 0.15f || (side * kick > 0 && strength > std::fabs (kick))))
			kick = side * strength;
		float shake = std::clamp (0.4f + impact / 15.0f, 0.4f, 1.0f);
		jolt = std::max (jolt, shake);
		wheel_jolt = std::max (wheel_jolt, wheel_impact * shake);
	}
	/* A bump or a landing: the vertical acceleration off its average */
	float bump = std::fabs (avert - avert_slow);
	avert_slow += (avert - avert_slow) * 0.1f;
	if (bump > scale.bump) {
		float shake = terrain * std::clamp ((bump - scale.bump) / 40.0f + 0.2f, 0.0f, 0.8f);
		jolt = std::max (jolt, shake);
		wheel_jolt = std::max (wheel_jolt, wheel_bumps * shake);
	}
	float scrape = wall && speed > 5.0f ? std::min (speed / 80.0f, 1.0f) : 0.0f;
	/* The turn's weight, smoothed over some 70 ms; held through a hit */
	if (!hit)
		corner += (0.35f * std::clamp (alat / scale.hard_turn, -1.0f, 1.0f) - corner) * 0.25f;

	/* The wheel */
	float spring = steering * wheel_spring * (0.10f + 0.40f * fast) * (air ? 0.3f : 1.0f);
	float friction = steering * wheel_friction * (0.08f + 0.22f * fast);
	float constant = std::clamp (steering * wheel_turn * corner * (air ? 0.3f : 1.0f) + hits * wheel_kick * kick, -1.0f, 1.0f);
	float vibrate = engines * (wheel_engines * (0.04f + 0.12f * throttle * fast + 0.35f * shaker) +
	                           wheel_boost * (boost ? 0.20f : 0.0f)) +
	                hits * (wheel_scrape * 0.35f * scrape + 0.6f * wheel_jolt);
	float offset = steering * extra (p, "engines_roll") * SPRING_OFFSET_PER_RADIAN;
	ForceFeedback::spring_offset (step (std::clamp (offset, -0.5f, 0.5f)));
	ForceFeedback::set (ForceFeedback::SPRING, step (std::clamp (spring, 0.0f, 1.0f)), f.frame);
	ForceFeedback::set (ForceFeedback::FRICTION, step (std::clamp (friction, 0.0f, 1.0f)), f.frame);
	ForceFeedback::set (ForceFeedback::CONSTANT, step (constant), f.frame);
	ForceFeedback::set (ForceFeedback::VIBRATE, step (std::clamp (vibrate, 0.0f, 1.0f)), f.frame);

	/* A controller's motors */
	float boosting = boost ? std::clamp ((fast - 0.5f) / 0.5f, 0.0f, 1.0f) : 0.0f;
	float low = engines * boosting * (0.20f + 0.5f * shaker) + hits * 0.9f * jolt;
	float high = engines * boosting * 0.35f + hits * (0.5f * scrape + 0.6f * jolt);
	ForceFeedback::rumble (std::clamp (low * rumble, 0.0f, 1.0f), std::clamp (high * rumble, 0.0f, 1.0f), f.frame);

	/* The kick fades in some 90 ms, the shake in 200 */
	kick *= std::exp (-dt / 0.09f);
	jolt *= std::exp (-dt / 0.2f);
	wheel_jolt *= std::exp (-dt / 0.2f);
	was_wall = wall;
}

void
FfbEffects::stop (uint64_t frame)
{
	if (!playing)
		return;
	playing = false;
	corner = kick = jolt = wheel_jolt = 0;
	ForceFeedback::set (ForceFeedback::STOP, 0.0f, frame);
}

/* Planet Harriers (its Rev A, the set "pharrier"), found by comparing the same
 * frames with and without a bomb. A byte blinks while the bomb charges, 13 to
 * 50 frames after the button. Another strobes with the screen's flashes: one
 * frame at 11 and at 35, then from 51 on, some 35 frames, the beams of the
 * explosion. So the explosion is that strobe once the charge has lasted a
 * while. Both stay at 0 otherwise (1800 frames of shooting) */
#define HARRIERS_BOMB_CHARGE	0x0C216679
#define HARRIERS_BOMB_STROBE	0x0C19C39D
#define HARRIERS_CHARGE_FRAMES	30
#define HARRIERS_CHARGE_GAP	20
#define HARRIERS_BLAST_HOLD	12

static uint8_t
master_u8 (Machine *mach, uint32_t addr)
{
	for (const auto &m : mach->memories)
		if (!strcmp (m.first, "ram_m"))
			return addr - 0x0C000000 < m.second->size () ?
			       (uint8_t) m.second->get (1, addr - 0x0C000000) : 0;
	return 0;
}

/* A shot while the button is held: Planet Harriers' guns repeat */
#define SHOT_EVERY_FRAMES	10
/* The nozzle's vibration held over the gaps of the game's output */
#define NOZZLE_HOLD_FRAMES	12

static bool game_rumble_on;
/* A jolt of each weapon, fading: bullets, missiles, bombs */
static float shot, missile_jolt, bomb_jolt;
static bool was_bullet, was_missile, was_bomb;
static unsigned blast_hold, charge_age, charge_gap;
static uint64_t last_shot;
static unsigned nozzle_held[2];
/* Who last held each player's water button */
static int water_holder[2] = { Controls::NOBODY, Controls::NOBODY };

/* A button of Planet Harriers' stick, 1 to 3: the player's, or a mouse's
 * (the roles on its switches: data/games.json) */
static bool
harriers_button (unsigned n)
{
	static const char *player[3] = { "P1_Button1", "P1_Button2", "P1_Button3" };
	static const char *pointer[3] = { "P1_Trigger", "P1_Fire2", "P1_Fire3" };
	GameControls &g = GameControls::get ();

	return g.pressed (player[n - 1]) || g.pressed (pointer[n - 1]);
}

/* shot1, shot2, nozzle1, nozzle2 for the outputs' clients: whatever the
 * force feedback's settings, and on a device or not */
static void
send_signals (bool harriers, bool firefighters)
{
	Outputs *out = Outputs::get ();

	if (harriers) {
		bool bullet = harriers_button (1);
		bool missile = harriers_button (2);

		out->signal ("shot1", bullet || missile);
		out->signal ("bullet1", bullet);
		out->signal ("missile1", missile);
		out->signal ("bomb1", harriers_button (3));
	}
	if (firefighters) {
		/* NOZZLE VIBRATION: outputs 2 and 5, held over their gaps */
		static const unsigned outputs[2] = { 2, 5 };
		static const char *const names[2] = { "nozzle1", "nozzle2" };

		for (unsigned i = 0; i < 2; i++) {
			if (out->jvs_output (outputs[i]))
				nozzle_held[i] = NOZZLE_HOLD_FRAMES;
			else if (nozzle_held[i])
				nozzle_held[i]--;
			out->signal (names[i], nozzle_held[i] != 0);
		}
	}
}

void
GameRumble::frame (Machine *mach, uint64_t frame)
{
	const Config *cfg = Config::get ();
	const char *game = mach->game->name, *parent = mach->game->parent;
	bool harriers = !strcmp (game, "pharrier") || !strcmp (parent, "pharrier");
	bool firefighters = !strcmp (game, "braveff") || !strcmp (parent, "braveff");
	float low = 0.0f, high = 0.0f, wheel = 0.0f;

	if (!harriers && !firefighters)
		return;
	send_signals (harriers, firefighters);
	if (!cfg->ffb_effects) {
		stop (frame);
		return;
	}
	if (harriers) {
		bool bullet = harriers_button (1);
		bool missile = harriers_button (2);
		bool bomb = harriers_button (3);
		float shots = cfg->ffb_shots / 100.0f * shot;
		float missiles, bombs;

		if (bullet && (!was_bullet || frame - last_shot >= SHOT_EVERY_FRAMES)) {
			shot = 1.0f;
			shots = cfg->ffb_shots / 100.0f;
			last_shot = frame;
		}
		if (missile && !was_missile)
			missile_jolt = 1.0f;
		if (bomb && !was_bomb)
			bomb_jolt = 1.0f;
		was_bullet = bullet;
		was_missile = missile;
		was_bomb = bomb;
		missiles = cfg->ffb_missiles / 100.0f * missile_jolt;
		bombs = cfg->ffb_bombs / 100.0f * bomb_jolt;
		/* Bullets short and sharp, on the high motor; a missile heavier
		 * and longer; a bomb the heaviest and the longest */
		low = std::max ({ 0.20f * shots, 0.85f * missiles, 0.45f * bombs });
		high = std::max ({ 0.55f * shots, 0.70f * missiles, 0.35f * bombs });
		wheel = std::max ({ 0.25f * shots, 0.70f * missiles, 0.40f * bombs });
		/* The bomb's explosion, as hard as the device goes */
		if (!strcmp (game, "pharrier")) {
			bool strobe = master_u8 (mach, HARRIERS_BOMB_STROBE) != 0;

			if (master_u8 (mach, HARRIERS_BOMB_CHARGE)) {
				charge_gap = 0;
				if (!charge_age)
					charge_age = 1;
			} else if (charge_age && ++charge_gap > HARRIERS_CHARGE_GAP)
				charge_age = 0;
			if (charge_age)
				charge_age++;
			if (strobe && (blast_hold || charge_age >= HARRIERS_CHARGE_FRAMES))
				blast_hold = HARRIERS_BLAST_HOLD;
			else if (blast_hold)
				blast_hold--;
			if (blast_hold) {
				float blast = cfg->ffb_bombs / 100.0f;

				low = std::max (low, blast);
				high = std::max (high, blast);
				wheel = std::max (wheel, blast);
			}
		}
		shot = shot * 0.72f < 0.02f ? 0.0f : shot * 0.72f;
		missile_jolt = missile_jolt * 0.88f < 0.02f ? 0.0f : missile_jolt * 0.88f;
		bomb_jolt = bomb_jolt * 0.92f < 0.02f ? 0.0f : bomb_jolt * 0.92f;
	} else {
		float strength = cfg->ffb_nozzle / 100.0f;
		Controls &c = Controls::get ();
		int pads[2] = { -1, -1 };

		/* For each player, the device holding the button of the nozzle
		 * that sprays (send_signals) takes it: a controller's motors, a
		 * wheel's vibration; a mouse or a key, nothing */
		for (unsigned i = 0; i < 2; i++) {
			int who = c.holder ("Pointer", i ? "P2_Trigger" : "P1_Trigger");

			if (who == Controls::NOBODY && i == 0)
				who = c.holder ("Player1", "P1_Button1");
			if (who != Controls::NOBODY)
				water_holder[i] = who;
			if (!nozzle_held[i] || water_holder[i] < 0 || water_holder[i] >= Controls::MOUSE_PLAYER)
				continue;
			if (ForceFeedback::is_force_device (water_holder[i]))
				wheel = strength * 0.15f;
			else if (pads[0] < 0)
				pads[0] = water_holder[i];
			else if (pads[0] != water_holder[i])
				pads[1] = water_holder[i];
		}
		/* None any more: the last ones kept, for the motors' stop below */
		if (pads[0] >= 0) {
			ForceFeedback::rumble_for (pads[0], pads[1]);
			low = strength * 0.25f;
			high = strength * 0.08f;
		}
	}
	game_rumble_on = true;
	float rumble = cfg->ffb_rumble / 100.0f;
	ForceFeedback::rumble (std::clamp (low * rumble, 0.0f, 1.0f), std::clamp (high * rumble, 0.0f, 1.0f), frame);
	ForceFeedback::set (ForceFeedback::VIBRATE, step (std::clamp (wheel, 0.0f, 1.0f)), frame);
}

void
GameRumble::stop (uint64_t frame)
{
	/* Paused or gone: nobody fires */
	for (const char *name : { "shot1", "bullet1", "missile1", "bomb1", "nozzle1", "nozzle2" })
		Outputs::get ()->signal (name, 0);
	nozzle_held[0] = nozzle_held[1] = 0;
	if (!game_rumble_on)
		return;
	game_rumble_on = false;
	shot = missile_jolt = bomb_jolt = 0.0f;
	was_bullet = was_missile = was_bomb = false;
	blast_hold = charge_age = charge_gap = 0;
	water_holder[0] = water_holder[1] = Controls::NOBODY;
	ForceFeedback::rumble_for (-1);
	ForceFeedback::set (ForceFeedback::STOP, 0.0f, frame);
}

} /* namespace hl */
