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
 * The effects, from what the decoder reads (hl/telemetry-games.cpp):
 *
 * - Centring (spring) and damping (friction), stiffer with the speed: a pod
 *   at 600 km/h holds its line. Lighter in the air.
 * - The turn's weight (a constant force): the lateral acceleration pushes
 *   the wheel back out of the turn, as a car's self-aligning torque.
 * - A hit (the game's own, against a wall or a pod, or a change of speed
 *   past 8 g): a kick of the wheel away from the side hit (an acceleration to
 *   the right, the wall on the left: the wheel kicked to the right), as
 *   strong as the speed lost, then a shake; scraping a wall, a rough shake
 *   with the speed.
 * - The engines: a rumble with their thrust and the speed, the deluxe
 *   cabinet's bass shaker's level (the option sound board's low
 *   frequencies) over it, stronger while boosting.
 * - The ground: a bump or a landing, the vertical acceleration leaving its
 *   own average, shakes.
 *
 * On a controller: its low motor the engines, the shaker, the bumps and the
 * hits; its high one the boost, the scraping, the hits.
 */

#include <algorithm>
#include <cmath>

#include "hl/ffb-effects.h"
#include "hl/config.h"
#include "hl/ffb.h"

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

	/* A new race, or frames skipped (a state loaded): from rest */
	if (!playing || f.frame != last_frame + 1) {
		corner = kick = jolt = 0;
		avert_slow = finite (p.avert);
		alat_slow = finite (p.alat);
	}
	playing = true;
	last_frame = f.frame;

	float speed = finite (p.speed);
	float fast = std::min (speed / 170.0f, 1.0f);
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
		jolt = std::max (jolt, std::clamp (0.4f + impact / 15.0f, 0.4f, 1.0f));
	}
	/* A bump or a landing: the vertical acceleration off its average */
	float bump = std::fabs (avert - avert_slow);
	avert_slow += (avert - avert_slow) * 0.1f;
	if (bump > 15.0f)
		jolt = std::max (jolt, terrain * std::clamp ((bump - 15.0f) / 40.0f + 0.2f, 0.0f, 0.8f));
	float scrape = wall && speed > 5.0f ? std::min (speed / 80.0f, 1.0f) : 0.0f;
	/* The turn's weight, smoothed over some 70 ms; held through a hit */
	if (!hit)
		corner += (std::clamp (alat / 60.0f, -0.35f, 0.35f) - corner) * 0.25f;

	/* The wheel */
	float spring = steering * (0.10f + 0.40f * fast) * (air ? 0.3f : 1.0f);
	float friction = steering * (0.08f + 0.22f * fast);
	float constant = std::clamp (steering * corner * (air ? 0.3f : 1.0f) + hits * kick, -1.0f, 1.0f);
	float vibrate = engines * (0.04f + 0.12f * throttle * fast + 0.35f * shaker + (boost ? 0.20f : 0.0f)) +
	                hits * (0.35f * scrape + 0.6f * jolt);
	ForceFeedback::set (ForceFeedback::SPRING, step (std::clamp (spring, 0.0f, 1.0f)), f.frame);
	ForceFeedback::set (ForceFeedback::FRICTION, step (std::clamp (friction, 0.0f, 1.0f)), f.frame);
	ForceFeedback::set (ForceFeedback::CONSTANT, step (constant), f.frame);
	ForceFeedback::set (ForceFeedback::VIBRATE, step (std::clamp (vibrate, 0.0f, 1.0f)), f.frame);

	/* A controller's motors */
	float low = engines * (0.10f * throttle * fast + 0.5f * shaker) + hits * 0.9f * jolt;
	float high = engines * ((boost ? 0.35f : 0.0f) + 0.04f * throttle) + hits * (0.5f * scrape + 0.6f * jolt);
	ForceFeedback::rumble (std::clamp (low * rumble, 0.0f, 1.0f), std::clamp (high * rumble, 0.0f, 1.0f), f.frame);

	/* The kick fades in some 90 ms, the shake in 200 */
	kick *= std::exp (-dt / 0.09f);
	jolt *= std::exp (-dt / 0.2f);
	was_wall = wall;
}

void
FfbEffects::stop (uint64_t frame)
{
	if (!playing)
		return;
	playing = false;
	corner = kick = jolt = 0;
	ForceFeedback::set (ForceFeedback::STOP, 0.0f, frame);
}

} /* namespace hl */
