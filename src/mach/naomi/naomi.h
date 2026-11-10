/*
 * Hikaruloader (formerly Valkyrie)
 * Copyright (C) 2026, Benjamin Maurin (njz3)
 *
 * Derived from MAME's src/mame/sega/naomi.cpp, dc.cpp, maple-dc.cpp, mie.cpp
 * and naomi*.cpp (license BSD-3-Clause, copyright-holders Samuele Zannoli,
 * R. Belmont, ElSemi, Olivier Galibert, Angelo Salese, Andreas Naive).
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

#ifndef __HL_NAOMI_H__
#define __HL_NAOMI_H__

#include "hl/driveboard.h"
#include "hl/cardreader.h"
#include <deque>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "hl/mmap.h"
#include "hl/cpu.h"
#include "hl/eeprom.h"
#include "hl/jvs.h"
#include "hl/machine.h"
#include "hl/nvram.h"
#include "hl/x76f100.h"

#include "cpu/sh/sh4.h"
#include "cpu/z80/z80.h"
#include "mach/hikaru/hikaru-5881.h"

namespace hikaru {
class Aica;
}

namespace naomi {

class Holly;
class Elan;
class Board;
class Naomi;

/* The screen's vertices, as the renderer draws them */
struct Vertex {
	float x, y, z;		/* z: 1/w */
	float u, v;
	uint32_t base;		/* A8R8G8B8 */
	uint32_t offset;
};

enum List {
	LIST_OPAQUE,
	LIST_PUNCH,
	LIST_TRANSLUCENT,
	NUM_LISTS
};

struct Batch {
	uint32_t isp, tsp, tcw;
	uint32_t first, count;
	/* The user's tile clip, in pixels: drawn only inside when clip_on,
	 * only outside when clip_out */
	uint16_t x0, y0, x1, y1;
	bool clip_on, clip_out;
	bool shadow;		/* a modifier volume changes it */
	/* "With two volumes": inside a modifier volume, these words instead,
	 * and the vertices from first1 of Scene::vertices1 */
	bool two_volume;
	uint32_t tsp1, tcw1;
	uint32_t first1;
};

/* A modifier volume's triangles: three xyz each */
struct ModVol {
	uint32_t isp;
	uint32_t first, count;
};

/* What the TA made of a list of parameters */
struct Scene {
	std::vector<Vertex> vertices;
	std::vector<Vertex> vertices1;
	std::vector<Batch> batches[NUM_LISTS];
	std::vector<float> mod_vertices;
	std::vector<ModVol> mod_volumes;
	/* Those of the translucent modifier volume list, in the same vertices */
	std::vector<ModVol> mod_volumes_tr;
	float max_z = 1e-6f;
};

/* A texture's bytes as the VRAM had them at a STARTRENDER: what the core draws
 * then; the game may write the VRAM's next picture as soon as it starts */
struct TextureData {
	std::vector<uint8_t> bytes;
	uint64_t hash;
};

typedef std::unordered_map<uint64_t, std::shared_ptr<const TextureData>> TextureMap;

static inline uint64_t
texture_key (uint32_t tcw, uint32_t tsp)
{
	return ((uint64_t) tcw << 32) | (tsp & 0x3F);
}

/* The bytes of a texture in the VRAM (naomi-renderer.cpp) */
uint32_t texture_size (uint32_t tcw, uint32_t tsp, uint32_t text_control);

/* Holly's registers as a drawing wants them, at its STARTRENDER */
struct Draw {
	std::shared_ptr<Scene> scene;
	TextureMap textures;
	uint32_t fb_w_sof1, fb_w_ctrl, fb_w_linestride;
	uint32_t fb_x_clip, fb_y_clip;
	uint32_t text_control, pal_ctrl;
	uint32_t pt_alpha_ref;
	uint32_t shad_scale;
	float background_z;
	bool presort;
	bool to_texture;	/* FB_W_SOF1 bit 24 */
	float scale_x;		/* SCALER_CTL's */
	float scale_y;
	uint32_t y_coeff;	/* its filter's coefficients */
	uint32_t fog_color, fog_color_vertex, fog_density;
	uint32_t clamp_max, clamp_min;
	uint16_t fog_table[128];
	uint32_t palette[1024];
	/* The background polygon's: its colours, its corners */
	Vertex background[3];
	uint32_t background_isp, background_tsp, background_tcw;
	bool background_shadow;
};

/* The cartridge board on the G1 bus: its registers at 005F7000; see
 * naomi-cart.cpp */
class Cart {
public:
	enum Type { TYPE_ROM, TYPE_M1, TYPE_M2, TYPE_M4 };

	Cart (hl::Buffer *rom, Type type, uint32_t key, uint32_t id, const uint8_t *m4_key);
	virtual ~Cart () {}

	virtual void	 reset ();
	virtual uint32_t get (unsigned reg);
	virtual void	 put (unsigned reg, uint32_t val);
	/* The G1's DMA: size bytes from where DMA_OFFSET points */
	virtual void	 dma (uint8_t *out, uint32_t size);
	virtual int	 save_state (hl::State *state);
	virtual int	 load_state (hl::State *state);

	hl::X76F100 eeprom;

	/* For the 315-5881 */
	uint16_t crypt_read (uint32_t addr);

private:
	void	 setup (uint32_t address, bool is_dma);
	/* What the board reads next, into out; its address moved on */
	void	 read (uint8_t *out, uint32_t size);
	uint16_t rom_u16 (uint32_t addr) const;

	/* M1 */
	uint32_t m1_word ();
	uint32_t m1_look (int bits);
	uint32_t m1_get (int bits);
	void	 m1_reset ();
	void	 m1_put (uint8_t byte);
	void	 m1_fill ();
	void	 m1_advance (uint32_t size);
	/* M4 */
	uint16_t m4_round (uint16_t word, uint16_t subkey) const;
	void	 m4_fill ();
	void	 m4_advance (uint32_t size);

	hl::Buffer *rom_buffer;
	const uint8_t *rom;
	uint32_t rom_size;
	Type	 type;
	uint32_t m1_key;
	uint32_t m_id;

	hikaru::Crypt5881 crypt;
	std::vector<uint8_t> m2_ram;
	std::vector<uint16_t> m4_table;
	uint16_t subkey1, subkey2;

	enum { BUFFER_SIZE = 32768 };
	std::vector<uint8_t> buffer;
	struct State {
		uint32_t rom_offset, dma_offset, dma_count;
		uint32_t cur;		/* the address being read */
		uint8_t	 pio_ready, dma_ready, last_is_dma;
		uint8_t	 encryption;
		uint32_t buffer_size;
		/* M1's */
		uint8_t	 dict[111], hist[2];
		uint8_t	 stream_ended, has_history;
		uint64_t avail_val;
		int32_t	 avail_bits;
		/* M4's */
		uint8_t	 cfi;
		uint16_t iv;
		uint32_t counter;
	} s;
};

/* The Atomiswave's ROM board; see naomi-aw.cpp */
class AwCart : public Cart {
public:
	AwCart (hl::Buffer *rom, uint8_t key);

	void	 reset () override;
	uint32_t get (unsigned reg) override;
	void	 put (unsigned reg, uint32_t val) override;
	void	 dma (uint8_t *out, uint32_t size) override;
	int	 save_state (hl::State *state) override;
	int	 load_state (hl::State *state) override;

private:
	uint16_t decrypted (uint32_t address) const;
	void	 recalc (int mode);

	const uint16_t *data;
	uint32_t bytes, mpr_offset;
	uint8_t	 key;
	struct {
		uint32_t epr_offset, record_index, first_file_index, file_offset;
		uint32_t dma_offset, dma_limit, bank;
	} r;
};

/* A GD-ROM's game on its DIMM board, shown as a cartridge; see naomi-gd.cpp */
class GdCart : public Cart {
public:
	GdCart (Board *board, hl::Buffer *dimm);

	/* The disc's file read and decrypted: the board's memory; NULL else */
	static hl::Buffer *load (hl::Machine *machine, hl::Game *game, const uint8_t *pic, uint32_t pic_size);

	void	 reset () override;
	uint32_t get (unsigned reg) override;
	void	 put (unsigned reg, uint32_t val) override;
	int	 save_state (hl::State *state) override;
	int	 load_state (hl::State *state) override;
	/* As each frame ends */
	void	 frame ();

private:
	void	 poke (uint32_t addr, uint32_t val);
	uint32_t peek (uint32_t addr);
	void	 interrupt (bool on);
	void	 reply (uint16_t command, bool failed, uint16_t offset, uint32_t parameter);
	void	 command ();

	Board	*board;
	uint32_t bytes;
	struct {
		uint16_t command, offset, parameterl, parameterh;
		uint32_t frames;
		uint8_t	 irq, started;
	} d;
};

/* The Atomiswave's BIOS flash: the writes (read as a ROM in the map) */
class AwFlash : public hl::Device {
public:
	AwFlash (hl::Machine *mach, hl::Buffer *rom);

	int	 get (unsigned size, uint32_t addr, void *val) override;
	int	 put (unsigned size, uint32_t addr, uint64_t val) override;
	void	 reset (hl::ResetType type) override;
	int	 save_state (hl::State *state) override;
	int	 load_state (hl::State *state) override;

	/* Programmed or erased since the last save */
	bool	 changed = false;

private:
	hl::Buffer *rom;
	unsigned step;
};

/* The Atomiswave's controllers on the Maple bus */
class AwControllers {
public:
	/* How many are plugged */
	unsigned count = 2;
	/* The reply to a frame at port, in out (at most 64 words): its words, 0 for none */
	unsigned reply (unsigned port, const uint32_t *request, uint32_t *out);

private:
	uint16_t switches (unsigned port) const;
};

/* The Atomiswave's "modem" area: coins, digital outputs */
class AwModem : public hl::Device {
public:
	explicit AwModem (Board *board);

	int	 get (unsigned size, uint32_t addr, void *val) override;
	int	 put (unsigned size, uint32_t addr, uint64_t val) override;
	void	 reset (hl::ResetType type) override;
	int	 save_state (hl::State *state) override;
	int	 load_state (hl::State *state) override;

private:
	uint8_t	 coins ();

	Board	*board;
	struct {
		uint8_t devices, output, chute[2];
	} s;
};

/* The MIE on the Maple bus: a Z80 and its ports; see naomi-mie.cpp */
class Mie : public hl::Device, public Z80::Bus {
public:
	Mie (Board *board, hl::Buffer *rom);
	~Mie ();

	void	 reset (hl::ResetType type) override;
	/* One line's T-states, and its timer */
	int	 exec (int t_states) override;
	int	 save_state (hl::State *state) override;
	int	 load_state (hl::State *state) override;

	uint8_t	 read (uint16_t addr) override;
	void	 write (uint16_t addr, uint8_t val) override;
	uint8_t	 in (uint16_t port) override;
	void	 out (uint16_t port, uint8_t val) override;
	uint8_t	 acknowledge () override;

	/* From the Maple bus */
	void	 maple_write (const uint32_t *data, uint32_t words);
	void	 maple_read (uint32_t *data, uint32_t *words, bool *partial);
	void	 maple_reset ();

	hl::Eeprom93C46 eeprom;
	bool	 monitor_15khz = false;
	/* The board's switches SW1 (bits 0-3, 1 off): where it is in a cabinet
	 * of several screens; 0F for a board alone */
	uint8_t	 switches = 0x0F;

private:
	void	 reply_timer ();
	void	 raise_irq (int level);
	void	 update_irq ();

	Board	*board;
	std::unique_ptr<Z80> z80;
	uint8_t	 rom[2048];
	uint8_t	 ram[0x8800];

	enum { TBUF_SIZE = 8 };
	struct State {
		uint32_t control, lreg;
		uint32_t tbuf[TBUF_SIZE];
		uint32_t reply[TBUF_SIZE];
		uint32_t reply_size;
		uint8_t	 reply_partial;
		int32_t	 timer;		/* lines until reply_timer () */
		uint8_t	 gpio[8], gpio_dir;
		uint8_t	 irq_enable, irq_pending, irq_level;
		uint8_t	 jvs_lcr, jvs_control;
		int32_t	 owed;
	} s;
};

/* The Holly: the system bus, the G1 and G2 buses, the Maple bus, the PowerVR 2
 * with its Tile Accelerator. See naomi-holly.cpp */
class Holly : public hl::Device {
public:
	Holly (Board *board, hl::Buffer *vram);

	/* 005F6800-005F9FFF */
	int	 get (unsigned size, uint32_t addr, void *val) override;
	int	 put (unsigned size, uint32_t addr, uint64_t val) override;
	void	 reset (hl::ResetType type) override;
	int	 save_state (hl::State *state) override;
	int	 load_state (hl::State *state) override;
	const char *debug_string () override;

	/* Area 4: the TA's FIFO, the YUV converter, the direct path */
	void	 area4_put (unsigned size, uint32_t addr, uint64_t val);
	/* The VRAM's 32-bit path */
	static uint32_t map32 (uint32_t addr)
	{
		uint32_t bank = (addr >> 22) & 1;
		return (addr & 0x00800003) | ((addr & 0x003FFFFC) << 1) | (bank << 2);
	}

	/* The frame's lines; line: as each one ends */
	unsigned lines () const;
	void	 line (unsigned n);
	/* The SH-4's interrupt level again */
	void	 update_irq ();
	void	 set_external (uint32_t bit, bool on);
	void	 maple_end_of_reply ();
	/* The words of a TA's 32 bytes (the Elan's too) */
	void	 ta_packet (const uint32_t *w);
	/* The scene the TA is making */
	Scene	*ta_scene ();

	/* SB_SFRES written: the board restarts as the line ends */
	bool	 reset_requested = false;
	/* What the renderer takes at each frame */
	std::vector<Draw> take_draws ();
	/* The drawings waiting: none, a board that shows nothing */
	size_t	 pending_draws () const { return draws.size (); }

	uint32_t fb_r_ctrl () const { return pvr[0x44 / 4]; }
	uint32_t fb_r_sof1 () const { return pvr[0x50 / 4]; }
	uint32_t fb_r_size () const { return pvr[0x5C / 4]; }

	hl::Buffer *vram;
	uint32_t frames;
	/* The Elan's end of a list asks for this */
	void	 raise_ist (uint32_t bits);

	/* The Elan's way in: a TA's 32 bytes, whose ends of list say nothing */
	void	 elan_packet (const uint32_t *w);
	/* The second half of a TA's 64 bytes is awaited */
	bool	 elan_wants_half () const { return ta.half != 0; }
	/* The list open, else -1 */
	int	 elan_list () const { return ta.list; }
	/* A polygon's words, as the TA's header would have left them */
	void	 elan_polygon (unsigned list, uint32_t isp, uint32_t tsp, uint32_t tcw, bool shadow);
	void	 elan_triangle (const Vertex &a, const Vertex &b, const Vertex &c);
	/* A modifier volume, and its triangles: three xyz on the screen */
	void	 elan_modifier (unsigned list, uint32_t isp);
	void	 elan_mod_triangle (const float *xyz);
	void	 elan_clip_mode (unsigned mode);
	/* A wait on the end of a list: bits of ISTNRM */
	void	 elan_end_of_list (uint32_t bits);

private:
	void	 sb_write (unsigned reg, uint32_t val, uint32_t old);
	void	 pvr_write (unsigned reg, uint32_t val, uint32_t old);
	void	 start_render ();
	void	 ch2_dma ();
	void	 g1_dma ();
	void	 g2_dma (unsigned ch);
	void	 pvr_dma ();
	void	 maple_step ();
	void	 maple_start ();
	void	 after (unsigned lines_, uint32_t ist);

	void	 ta_reset_list ();
	void	 keep_textures (Draw &d);
	void	 ta_polygon_header (const uint32_t *w);
	void	 ta_vertex (const uint32_t *w);
	void	 ta_sprite (const uint32_t *w);
	void	 ta_modifier (const uint32_t *w);
	void	 ta_store (Vertex v, const Vertex &v1, bool end);
	void	 ta_triangle (Vertex a, Vertex b, Vertex c, const Vertex *second = NULL);
	Batch	&ta_batch ();
	void	 yuv_data (const uint32_t *w, unsigned words);
	void	 yuv_block ();
	void	 fifo_word (uint32_t word);

	Board	*board;
	char	 debug_text[96];
	/* The IRL pins driven, -1 none */
	int	 irl_pins;
	/* A TA's packet from the Elan is being taken */
	bool	 from_elan;
	/* ISTNRM of the NAOMI 2's second CLX2, read at 025F6900 */
	uint32_t istnrm_b;

	struct DelayedIrq {
		unsigned lines;
		uint32_t ist;
	};
	std::vector<DelayedIrq> delayed;

	/* The scenes by the address they were built at */
	struct Context {
		uint32_t base;
		std::shared_ptr<Scene> scene;
	};
	std::vector<Context> contexts;
	std::shared_ptr<Scene> building;
	std::vector<Draw> draws;
	/* The textures' copies, for those that did not change at the next
	 * STARTRENDER */
	TextureMap kept_textures;

	/* Plain data, for the states */
	struct {
		uint32_t fifo[16];
		uint32_t fifo_words;
		uint8_t	 half;		/* the first half of 64 bytes is in fifo */
		int8_t	 list;		/* the list open, else -1 */
		uint8_t	 mod;		/* a modifier volume's triangles */
		uint8_t	 sprite;
		uint32_t vertex_type;
		uint32_t pcw, isp, tsp, tcw, tsp1, tcw1;
		float	 face[4], face1[4], face_offset[4];
		uint32_t sprite_base, sprite_offset;
		uint8_t	 clip_mode;
		uint16_t clip[4];
		uint8_t	 new_batch;
		uint8_t	 two_volume;
		Vertex	 strip[2], strip1[2];
		uint32_t strip_n;
	} ta;
	struct {
		uint32_t data[384 / 4];
		uint32_t bytes;
		uint32_t x, y;
	} yuv;
	uint32_t sb[0x200 / 4];
	uint32_t pvr[0x2000 / 4];
	uint32_t g1[0x100 / 4];
	uint32_t g2[0x100 / 4];
	uint32_t pd[0x100 / 4];
	uint32_t maple[0x100 / 4];
	struct {
		uint32_t state, address, port, dest;
		uint8_t	 end;
		int32_t	 timer;
	} mp;
	uint32_t line_now;
	/* For the log */
	uint32_t logged[32];
};

/* The NAOMI 2's transform and lighting chip: its registers (08800000) and its
 * command port (09000000). See naomi-elan.cpp */
class Elan : public hl::Device {
public:
	Elan (Board *board, hl::Buffer *ram);

	int	 get (unsigned size, uint32_t addr, void *val) override;
	int	 put (unsigned size, uint32_t addr, uint64_t val) override;
	void	 reset (hl::ResetType type) override;
	int	 save_state (hl::State *state) override;
	int	 load_state (hl::State *state) override;

	/* As each line ends */
	void	 line ();

private:
	enum { MAX_LIGHTS = 16 };

	struct Transformed;
	struct Light {
		float	 color[3];
		float	 direction[3];	/* to the light */
		float	 position[3];
		bool	 parallel, diffuse, specular, dist_mode;
		unsigned routing, dmode, smode;
		float	 dist_a, dist_b, angle_a, angle_b;
	};

	void	 command_word (unsigned index, uint32_t data);
	/* False when it stopped on an error */
	bool	 run (const uint8_t *data, int64_t size, unsigned depth);
	bool	 fail ();
	unsigned dma_duration (uint32_t bytes) const;
	uint32_t ram_offset (const uint8_t *p, uint32_t bytes) const;

	void	 reset_projection ();
	void	 clear_state ();
	void	 update ();
	void	 update_matrix ();
	void	 update_gmp ();
	void	 update_lights ();

	void	 polygon_list (const uint32_t *ich);
	void	 modifier_list (const uint32_t *ich, const uint32_t *w, unsigned words);
	void	 modifier_triangle (const float *a, const float *b, const float *c);
	bool	 in_depth_range (const uint32_t *w, uint32_t count, unsigned words, bool *clip) const;
	void	 view_position (const uint32_t *w, float *out) const;
	void	 light_vertex (Transformed &v, const float *normal) const;
	void	 project (Transformed &v) const;
	void	 clip_point (const Transformed &a, float da, const Transformed &b, float db, Transformed &out) const;
	void	 triangle (const Transformed &a, const Transformed &b, const Transformed &c, bool clip);

	Board	*board;
	hl::Buffer *ram;
	const uint8_t *ram_base;

	/* Plain data, for the states: objects by their offset in the RAM */
	struct {
		uint32_t reg10, reg74, reg78, reg30;
		uint32_t cmd[8];
		uint32_t gmp, instance, light_model;
		uint32_t lights[MAX_LIGHTS];
		float	 proj[4];	/* fx, tx, fy, ty */
		uint32_t dma_lines;	/* until a texture's DMA is done */
		uint32_t dma_end;	/* then channel 2's source, if it fed it */
	} s;

	/* What they say, read again after a state */
	float	 matrix[12];		/* to the view's space, by rows */
	float	 normal_matrix[9];	/* by columns */
	float	 near_plane, far_plane;
	float	 env_u, env_v;
	uint32_t gmp_params;
	float	 gmp_diffuse[4], gmp_specular[4];
	float	 gloss;
	bool	 lights_changed;
	Light	 lights[MAX_LIGHTS];
	unsigned light_count;
	bool	 use_base_over, ambient_material_base, ambient_material_offset;
	float	 ambient_base[4], ambient_offset[4];

	/* Within a model */
	bool	 culling_reversed, open_volume, shadowed_volume;
	uint32_t model_tsp;
	uint64_t commands;	/* those of this kick */
};

/* What carries a communication board's frames to the next board of its ring:
 * TCP between two emulators ([Network]), or the boards of one machine (Driving
 * Simulator's three). Each board has its own; see naomi-comm.cpp */
class CommRing {
public:
	virtual ~CommRing () {}

	/* The game let the board's 68000 go: mode as it gave it (0 master) and
	 * the bytes each node sends; false when the ring cannot open */
	virtual bool start (uint16_t mode, uint16_t slot_size) = 0;
	/* The game holds the board, or the link is gone */
	virtual void stop () = 0;
	/* Each frame until true: this node's number and how many there are */
	virtual bool numbered (unsigned *node, unsigned *nodes) = 0;
	/* Each frame since: what this node sends, where what the previous one
	 * sends goes (out and in may overlap); true when in was filled */
	virtual bool exchange (const uint8_t *out, uint8_t *in, uint32_t size,
	                       uint16_t number, uint16_t *number_in) = 0;
	virtual bool broken () const = 0;
};

/* The memory of the boards of one machine that are on one ring: each board's
 * letter box, used between two slices with all the boards stopped */
struct CommBoxes {
	struct Box {
		std::vector<uint8_t> data;
		uint16_t number = 0;
		bool full = false;
	};
	explicit CommBoxes (unsigned nodes) : boxes (nodes) {}
	std::vector<Box> boxes;
	unsigned started = 0;
};

/* The rings the board can have: over TCP as [Network] says, or the machine's own */
CommRing *comm_network_ring (hl::Machine *mach);
CommRing *comm_local_ring (CommBoxes *boxes, unsigned index);

/* The communication board on the G1 bus (005F7018-005F702B): its two memories,
 * its ring if it has one. A plain object of its board; see naomi-comm.cpp */
class CommBoard {
public:
	CommBoard ();

	/* The ring that carries its frames: none, a cabinet alone */
	void	 attach (CommRing *ring);
	void	 reset ();
	uint32_t get (uint32_t addr);
	void	 put (uint32_t addr, uint32_t val);
	/* The G1's DMA, when the board has the bus: false else */
	bool	 dma_to_ram (uint8_t *out, uint32_t size);
	bool	 dma_from_ram (const uint8_t *in, uint32_t size);
	/* Once a frame, all the boards stopped: the ring's work */
	void	 vblank ();
	int	 save_state (hl::State *state);
	int	 load_state (hl::State *state);

	/* For the logs */
	const char *name = "comm";

private:
	void	 start_ring ();
	void	 connected ();
	void	 lost ();
	uint16_t slot_size () const;

	enum RingState { RING_OFF, RING_NUMBERING, RING_UP };

	std::vector<uint8_t> m68k_ram, comm_ram;
	uint16_t control, offset, status1, status2;

	std::unique_ptr<CommRing> ring;
	RingState ring_state;
	unsigned node, nodes;
	uint16_t packet;
};

/* The link of a cabinet of several boards (F355 Challenge's): a memory the
 * master writes through the G2 bus and each slave reads through the G1 bus,
 * and a status register. One for the machine, shared by the boards' threads;
 * see naomi-multiboard.cpp */
class Multiboard {
public:
	explicit Multiboard (unsigned boards) : boards (boards), status (0), data (0x100000 / 2) {}

	unsigned boards;
	std::atomic<uint16_t> status;
	std::vector<uint16_t> data;
};

/* A board's side of that link: its G1 registers (005F7050-005F70C4) and its G2
 * bus (01000000-) */
class MultiboardPort : public hl::Device {
public:
	MultiboardPort (Board *board, Multiboard *link, unsigned id);

	/* The G2 bus */
	int	 get (unsigned size, uint32_t addr, void *val) override;
	int	 put (unsigned size, uint32_t addr, uint64_t val) override;
	void	 reset (hl::ResetType type) override;
	int	 save_state (hl::State *state) override;
	int	 load_state (hl::State *state) override;

	/* False when the address is not the link's */
	bool	 g1_get (uint32_t addr, uint32_t *val);
	bool	 g1_put (uint32_t addr, uint32_t val);
	/* A slave without a cartridge: its G1 DMA from the link's memory */
	bool	 dma (uint32_t size, uint8_t *out);
	/* SB_SFRES written: a slave is no longer ready */
	void	 software_reset ();
	/* All the boards stopped, between two slices: a slave's interrupt when
	 * the master switched its bank */
	void	 sync ();

private:
	bool	 master () const { return id == 0; }

	Board	*board;
	Multiboard *link;
	unsigned id;
	uint32_t offset;
	uint16_t dma_offset, seen_status;
};

/* What Driving Simulator's master says on the SCIF: the rev counter and the
 * speedometer of its dashboard; see naomi-comm.cpp */
class DriveSimMeters : public SerialPipe {
public:
	void	 write (uint8_t value) override;
	int	 available () override { return 0; }
	uint8_t	 read () override { return 0; }

private:
	std::vector<uint8_t> frame;
	int	 tacho = -1, speed = -1;
};

/* Club Kart's card reader, on the SCIF; see naomi-card.cpp */
class ClubKartCard : public SerialPipe {
public:
	explicit ClubKartCard (const char *game);

	void	 write (uint8_t value) override;
	int	 available () override;
	uint8_t	 read () override;
	/* Each frame: the card's control */
	void	 frame ();

private:
	static const unsigned TRACK = 0x45;

	bool	 load ();
	void	 save ();
	void	 command ();
	void	 reply (int cmd);
	static uint8_t crc (const uint8_t *begin, const uint8_t *end);

	std::string read_path, write_path;
	std::deque<uint8_t> out;
	std::vector<uint8_t> in;
	uint8_t	 data[TRACK];
	bool	 inserted = false, read_pending = false, was_held = false;
};

/* The SWP hopper board of the medal games, on the SCIF; see naomi-hopper.cpp */
class SwpHopper : public SerialPipe {
public:
	SwpHopper ();

	void	 write (uint8_t value) override;
	int	 available () override;
	uint8_t	 read () override;
	/* Each frame: the coin chute */
	void	 frame ();
	/* The game's board restarted: the line starts anew */
	void	 reset ();
	/* What is battery backed on the board, for the nvram file */
	const void *settings () const { return &s; }
	size_t	 settings_size () const { return sizeof (s); }
	bool	 load (const uint8_t *data, size_t size);
	/* Since the last time it was asked */
	bool	 changed ();
	int	 save_state (hl::State *state);
	int	 load_state (hl::State *state);

private:
	/* Words alone: compared with memcmp, and written to the nvram file */
	struct Settings {
		uint32_t credit0 = 0, credit1 = 0, total_credit = 100, premium = 0;
		uint32_t game_number = 0, won = 0, paid = 0;
		uint32_t free_play = 0, auto_pay_out = 0, two_way = 1, auto_exchange = 0;
		uint32_t coin_discrimination = 1, bet_button = 1;
		uint32_t currency = 0xFF, medal_exchange = 5, cur_base = 100;
		uint32_t max_pay = 1999900, max_credit = 1999900, hopper_size = 39900;
		uint32_t max_bet = 10000, min_bet = 100, add_bet = 100;
	};

	uint32_t arg (size_t offset) const;
	void	 send (uint8_t cmd, const std::vector<uint32_t> &payload);
	void	 command (uint8_t cmd);
	void	 power_on ();
	void	 config ();
	void	 bet ();
	void	 pay_out (uint32_t value);
	void	 coin_in ();
	void	 send_pay_win ();
	void	 set_defaults ();
	uint32_t play_options () const;
	uint32_t exchange_options () const;
	void	 put_clock (uint32_t *words) const;

	Settings s, saved;
	std::vector<uint8_t> in;
	std::deque<uint8_t> out;
	uint32_t expected = 0;
	bool	 started = false, coin_held = false;
};

/* A Sanwa CRP-1231 card reader on the SCIF (Initial D's) */
class SanwaCard : public SerialPipe {
public:
	SanwaCard (hl::Machine *machine, const std::string &path) : reader (new hl::CardReader (machine, path)) {}

	void	 write (uint8_t value) override { reader->receive (value); }
	int	 available () override { return reader->has_byte () ? 1 : 0; }
	uint8_t	 read () override { return reader->has_byte () ? reader->send () : 0; }
	/* A device: the machine's to delete */
	hl::CardReader *reader;
};

/* The servo board 838-14174 of the wheels on the MIDI port; see naomi-servo.cpp */
class ServoBoard {
public:
	explicit ServoBoard (Board *board);

	void	 attach ();
	void	 reset ();
	/* Each millisecond */
	void	 tick ();
	void	 midi_received (uint8_t data);
	int	 save_state (hl::State *state);
	int	 load_state (hl::State *state);

private:
	hikaru::Aica *aica () const;
	void	 effect (int which, float force);
	int	 wheel_position () const;
	bool	 holding_back () const;
	void	 apply_torque ();
	void	 apply_spring ();
	void	 apply_damper ();
	void	 apply_sine ();
	void	 apply_effects ();
	void	 stop_effects ();
	void	 reset_board ();
	void	 send (uint8_t b1, uint8_t b2, uint8_t b3);
	void	 reply ();
	void	 process (uint8_t cmd, uint8_t b1, uint8_t b2);

	Board	*board;
	struct {
		uint8_t	 frame[4], params[16][2], select[2];
		uint32_t received, state, reply_address, now_ms, search_end, last_frame_ms;
		bool	 calibrating, active, short_mode, dropped, holds_back, relayed;
		float	 power, position, center, speed, sine;
		int	 torque;
		uint16_t table[256];
	} s = {};
};

/* One NAOMI board, whole: an SH-4 at 200 MHz, a Holly (two and an Elan on the
 * NAOMI 2), an AICA with its ARM7, the MIE, a cartridge. An object with no state
 * outside it, its parts talking to it alone: a machine holds several of them
 * (the cabinets of several boards, multiboard; or whole systems on one ring,
 * Driving Simulator's three), each run by a thread of its own. What crosses
 * between boards goes through Multiboard or CommBoxes. See naomi.cpp */
class Board {
public:
	Board (Naomi *machine, unsigned index, hl::Game *game);
	~Board ();

	Board (const Board &) = delete;
	Board &operator= (const Board &) = delete;

	/* False when the game's ROMs do not fit */
	bool	 init ();
	/* The MIE's EEPROM made right for the game, as its header says */
	void	 configure_eeprom ();
	/* On the master of the games that have one; NULL else */
	std::unique_ptr<ServoBoard> servo;
	std::unique_ptr<ClubKartCard> card;
	std::unique_ptr<SanwaCard> sanwa;
	std::unique_ptr<SwpHopper> hopper;
	std::unique_ptr<DriveSimMeters> meters;
	/* F355 Challenge's, on the I/O board's lines */
	hl::DriveBoard *drive_board = NULL;
	void	 setup_servo ();
	void	 setup_hopper ();
	unsigned servo_lines = 0;
	void	 default_backup ();
	void	 default_eeprom ();
	/* [Network] LINK as the game's own settings hold it, from its title;
	 * see naomi-comm.cpp */
	void	 link_settings ();
	/* A byte of a record of the EEPROM, or of the backup memory's: false
	 * when the game has not written that record yet */
	bool	 eeprom_write (unsigned offs, uint8_t val);
	uint8_t	 eeprom_read (unsigned offs) const;
	bool	 flash_write (unsigned offs, uint8_t val);
	void	 reset (hl::ResetType type);
	/* The software reset (SB_SFRES): the SH-4 and the Holly */
	void	 software_reset ();
	/* The frame's start: how many lines it has */
	unsigned begin_frame ();
	/* Those lines run, from line */
	void	 run_lines (unsigned line, unsigned count);
	/* The frame's end: its sound moved to the machine's when asked */
	void	 end_frame (std::vector<int16_t> *audio);
	int	 load_state (hl::State *state);
	int	 save_state (hl::State *state);
	const char *debug_string ();

	Naomi	*machine;
	hl::Game *game;
	/* 0 the master, whose screen the machine shows first */
	unsigned index;
	bool	 is_naomi2;
	/* A NAOMI's sister: no MIE, its controllers on the Maple bus; naomi-aw.cpp */
	bool	 is_atomiswave;
	/* A game on a GD-ROM: its cartridge the DIMM board */
	GdCart	*gd = NULL;
	/* The cartridge's bytes: its header */
	hl::Buffer *rom_image = NULL;
	bool	 aw_wheel = false;
	AwControllers controllers;
	AwModem	*modem = NULL;
	AwFlash	*flash = NULL;

	hl::Cpu	*sh;
	hl::Mmap *mmap;
	Holly	*holly;
	Elan	*elan;
	Mie	*mie;
	std::unique_ptr<Cart> cart;
	/* False on a slave: what it runs comes through the link */
	bool	 has_cart;
	MultiboardPort *link;
	CommBoard comm;
	/* Its G1 bus has the communication board */
	bool	 has_comm = false;
	/* Vertical blank went by since the last time the boards met */
	bool	 vblank_due = false;
	/* [Network] LINK could not be set yet: the record is not there */
	bool	 link_settings_due = false;
	hl::Device *aica;
	hl::JvsBus *jvs;
	hl::Device *io_board;

	hl::Buffer *bios, *sram, *ram, *vram, *aica_ram, *elan_ram;

	/* Port A: the main board's X76F100, its serial number, which the BIOS
	 * checks at each start (with the cartridge's when the game asks) */
	hl::X76F100 main_serial;
	uint16_t porta;

	unsigned cycles_per_line;
	int	 mie_t_states;
	unsigned frame_lines;

	/* Those of nvram.get ()'s blocks of this board: EEPR, MIEE, BRAM, X76F,
	 * for the master; a suffix for the others */
	void	 nvram_read (const hl::Nvram &nvram);
	void	 nvram_write (hl::Nvram &nvram);
	/* The memories changed since the last call */
	bool	 nvram_changed ();

private:
	hl::Mmap *setup_mmap ();
	void	 add_buffer (hl::Buffer *&buf, unsigned size, bool battery = false);
	char	 debug_text[256];
	uint8_t	 nvram_old[128 + 128 + 132];
};

/* The NAOMI machine: its boards, and the threads that run them. One board runs
 * in the caller's thread, with no thread started */
class Naomi : public hl::Machine {
public:
	static Naomi *create (hl::Game *game);
	~Naomi ();

	int	 run_frame () override;
	void	 on_reset (hl::ResetType type) override;
	int	 on_load_state (hl::State *state) override;
	int	 on_save_state (hl::State *state) override;
	const char *debug_string () override;
	void	 next_screen () override;

	std::vector<std::unique_ptr<Board>> boards;
	std::unique_ptr<Multiboard> link;
	/* The boards are complete systems on a ring of communication boards,
	 * each with its cartridge (games.json "network_boards") */
	std::unique_ptr<CommBoxes> comm_boxes;
	bool	 networked = false;
	/* [Display] MULTI_SCREEN for this game: MULTI_ONE with one board */
	int	 multi_screen = 0;
	/* The boards that have a monitor, left to right */
	std::vector<unsigned> screen_order;
	/* The boards shown, one per screen, left to right */
	std::vector<unsigned> shown_boards () const;
	/* MULTI_ONE: the board shown */
	unsigned screen = 0;
	/* The other modes, NextScreen: every board having a monitor, how far
	 * they moved along; else the board without one the middle screen shows
	 * (F355 Deluxe's master), from 1, or 0 for the screen's own */
	unsigned screen_shift = 0;
	std::vector<unsigned> boards_unseen () const;
	/* NextScreen was pressed: the board shown no longer follows the drawings */
	bool	 screen_chosen = false;
	Board	*master () { return boards[0].get (); }

	void	 load_nvram ();
	void	 save_nvram ();
	void	 keep_nvram ();
	hl::Nvram nvram;
	std::string nvram_path;
	unsigned nvram_due;

	explicit Naomi (hl::Game *game);

private:
	/* The lines run between two meetings of the boards */
	static const unsigned SLICE_LINES = 8;

	/* A thread for each board but the master; they run slices when told */
	void	 start_threads ();
	void	 stop_threads ();
	void	 worker (unsigned index);
	void	 run_slice (Board *board);
	/* The board shown, its frames without a drawing of its own */
	void	 setup_screens ();
	void	 choose_screen ();
	unsigned screen_idle = 0;

	std::vector<std::thread> threads;
	/* The boards meet 66 times a frame: waking a thread each time costs more
	 * than the wait, so they spin; asleep (go) only for the long waits, the
	 * frame's end or a pause */
	std::mutex lock;
	std::condition_variable go;
	std::atomic<uint64_t> slice_id { 0 };
	std::atomic<unsigned> running { 0 };
	std::atomic<unsigned> sleeping { 0 };
	std::atomic<bool> quitting { false };
	unsigned slice_line = 0, slice_count = 0;
};

} /* namespace naomi */

#endif /* __HL_NAOMI_H__ */
