/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "vdp.h"
#include "blastem.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "render.h"
#include "util.h"
#include "event_log.h"
#include "terminal.h"
#include "refdata.h"
#ifndef DISABLE_NUKLEAR
#include "nuklear_ui/debug_ui.h"
#endif

#define NTSC_INACTIVE_START 224
#define PAL_INACTIVE_START 240
#define MODE4_INACTIVE_START 192
#define BUF_BIT_PRIORITY 0x40
#define MAP_BIT_PRIORITY 0x8000
#define MAP_BIT_H_FLIP 0x800
#define MAP_BIT_V_FLIP 0x1000

#define SCROLL_BUFFER_MASK (SCROLL_BUFFER_SIZE-1)
#define SCROLL_BUFFER_DRAW (SCROLL_BUFFER_SIZE/2)

#define MCLKS_SLOT_H40  16
#define MCLKS_SLOT_H32  20
#define VINT_SLOT_H40  0 //21 slots before HSYNC, 16 during, 10 after
#define VINT_SLOT_H32  0  //old value was 23, but recent tests suggest the actual value is close to the H40 one
#define VINT_SLOT_MODE4 4
#define HSYNC_SLOT_H40  230
#define HSYNC_END_H40  (HSYNC_SLOT_H40+17)
#define HBLANK_START_H40 178 //should be 179 according to Nemesis, but 178 seems to fit slightly better with my test ROM results
#define HBLANK_END_H40  0 //should be 5.5 according to Nemesis, but 0 seems to fit better with my test ROM results
#define HBLANK_START_H32 233 //should be 147 according to Nemesis which is very different from my test ROM result
#define HBLANK_END_H32 0 //should be 5 according to Nemesis, but 0 seems to fit better with my test ROM results
#define LINE_CHANGE_H40 165
#define LINE_CHANGE_H32 133
#define LINE_CHANGE_MODE4 248
#define VBLANK_START_H40 (LINE_CHANGE_H40+2)
#define VBLANK_START_H32 (LINE_CHANGE_H32+2)
#define FIFO_LATENCY    3
#define READ_LATENCY    3

#define BORDER_TOP_V24     27
#define BORDER_TOP_V28     11
#define BORDER_TOP_V24_PAL 54
#define BORDER_TOP_V28_PAL 38
#define BORDER_TOP_V30_PAL 30

#define BORDER_BOT_V24     24
#define BORDER_BOT_V28     8
#define BORDER_BOT_V24_PAL 48
#define BORDER_BOT_V28_PAL 32
#define BORDER_BOT_V30_PAL 24

enum {
	INACTIVE = 0,
	PREPARING, //used for line 0x1FF
	ACTIVE
};

uint16_t mode4_address_map[0x4000];
static uint32_t planar_to_chunky[256];
static uint8_t levels[] = {0, 27, 49, 71, 87, 103, 119, 130, 146, 157, 174, 190, 206, 228, 255};

static uint8_t debug_base[][3] = {
	{127, 127, 127}, //BG
	{0, 0, 127},     //A
	{127, 0, 0},     //Window
	{0, 127, 0},     //B
	{127, 0, 127}    //Sprites
};

static FILE *s_sprite_trace_file;
static uint32_t s_sprite_trace_lines;
static uint32_t k_sprite_trace_frame_start = 0;
static uint32_t k_sprite_trace_frame_end = 260;
static const uint16_t k_sprite_trace_line_start = 0;
static const uint16_t k_sprite_trace_line_end = 239;
static const uint32_t k_sprite_trace_max_lines = 200000;
static FILE *s_comp_trace_file;
static uint32_t s_comp_trace_lines;
static uint32_t k_comp_trace_frame_start = 0;
static uint32_t k_comp_trace_frame_end = 260;
static const uint16_t k_comp_trace_line_start = 0;
static const uint16_t k_comp_trace_line_end = 239;
static const uint16_t k_comp_trace_x_start = 0;
static const uint16_t k_comp_trace_x_end = 319;
static const uint32_t k_comp_trace_max_lines = 200000;

static void sprite_trace_open_once(void)
{
	if (!s_sprite_trace_file) {
		s_sprite_trace_file = fopen("blastem_reference\\sprite_trace_blastem.log", "w");
		if (s_sprite_trace_file) {
			fprintf(s_sprite_trace_file, "# Sprite tile diagnostic trace (BlastEm)\n");
			fprintf(s_sprite_trace_file, "# frameRange=%u-%u lineRange=%u-%u maxLines=%u\n",
				k_sprite_trace_frame_start, k_sprite_trace_frame_end,
				k_sprite_trace_line_start, k_sprite_trace_line_end, k_sprite_trace_max_lines);
			fflush(s_sprite_trace_file);
		}
	}
}

static void comp_trace_open_once(void)
{
	if (!s_comp_trace_file) {
		s_comp_trace_file = fopen("blastem_reference\\compose_trace_blastem.log", "w");
		if (s_comp_trace_file) {
			fprintf(s_comp_trace_file, "# Compositor diagnostic trace (BlastEm)\n");
			fprintf(s_comp_trace_file, "# frameRange=%u-%u lineRange=%u-%u xRange=%u-%u maxLines=%u\n",
				k_comp_trace_frame_start, k_comp_trace_frame_end,
				k_comp_trace_line_start, k_comp_trace_line_end,
				k_comp_trace_x_start, k_comp_trace_x_end, k_comp_trace_max_lines);
			fflush(s_comp_trace_file);
		}
	}
}

static uint8_t sprite_trace_enabled(vdp_context *context, uint16_t line)
{
	sprite_trace_open_once();
	return s_sprite_trace_file
		&& context->frame >= k_sprite_trace_frame_start
		&& context->frame <= k_sprite_trace_frame_end
		&& line >= k_sprite_trace_line_start
		&& line <= k_sprite_trace_line_end
		&& s_sprite_trace_lines < k_sprite_trace_max_lines;
}

static uint8_t comp_trace_enabled(vdp_context *context, uint16_t line, uint16_t x)
{
	comp_trace_open_once();
	return s_comp_trace_file
		&& context->frame >= k_comp_trace_frame_start
		&& context->frame <= k_comp_trace_frame_end
		&& line >= k_comp_trace_line_start
		&& line <= k_comp_trace_line_end
		&& x >= k_comp_trace_x_start
		&& x <= k_comp_trace_x_end
		&& s_comp_trace_lines < k_comp_trace_max_lines;
}

static void sprite_trace_log(vdp_context *context, uint16_t line, const char *fmt, ...)
{
	if (!sprite_trace_enabled(context, line)) {
		return;
	}
	va_list args;
	va_start(args, fmt);
	fprintf(s_sprite_trace_file, "F%04u L%03u ", context->frame, line);
	vfprintf(s_sprite_trace_file, fmt, args);
	fputc('\n', s_sprite_trace_file);
	va_end(args);

	s_sprite_trace_lines++;
	if ((s_sprite_trace_lines & 0x3FF) == 0) {
		fflush(s_sprite_trace_file);
	}
}

static void comp_trace_log(vdp_context *context, uint16_t line, uint16_t x, const char *fmt, ...)
{
	if (!comp_trace_enabled(context, line, x)) {
		return;
	}
	va_list args;
	va_start(args, fmt);
	vfprintf(s_comp_trace_file, fmt, args);
	fputc('\n', s_comp_trace_file);
	va_end(args);

	s_comp_trace_lines++;
	if ((s_comp_trace_lines & 0x3FF) == 0) {
		fflush(s_comp_trace_file);
	}
}

/* ---- horizontal-scroll diagnostic trace ---- */
static FILE *s_scroll_trace_file;
static uint32_t s_scroll_trace_lines;
static uint32_t k_scroll_trace_frame_start = 1110;
static uint32_t k_scroll_trace_frame_end   = 1864;
static uint16_t k_scroll_trace_line_start  = 0;
static uint16_t k_scroll_trace_line_end    = 239;
static uint16_t k_scroll_trace_col_start   = 0;
static uint16_t k_scroll_trace_col_end     = 40;
static uint32_t k_scroll_trace_max_lines   = 200000;

static void scroll_trace_open_once(void)
{
	if (!s_scroll_trace_file) {
		const char *env;
		if ((env = getenv("BLASTEM_SCR_FRAME_START"))) k_scroll_trace_frame_start = (uint32_t)atoi(env);
		if ((env = getenv("BLASTEM_SCR_FRAME_END")))   k_scroll_trace_frame_end   = (uint32_t)atoi(env);
		if ((env = getenv("BLASTEM_SCR_LINE_START")))   k_scroll_trace_line_start  = (uint16_t)atoi(env);
		if ((env = getenv("BLASTEM_SCR_LINE_END")))     k_scroll_trace_line_end    = (uint16_t)atoi(env);
		if ((env = getenv("BLASTEM_SCR_COL_START")))    k_scroll_trace_col_start   = (uint16_t)atoi(env);
		if ((env = getenv("BLASTEM_SCR_COL_END")))      k_scroll_trace_col_end     = (uint16_t)atoi(env);
		if ((env = getenv("BLASTEM_SCR_MAX_LINES")))    k_scroll_trace_max_lines   = (uint32_t)atoi(env);
		s_scroll_trace_file = fopen("blastem_reference\\scroll_trace_blastem.log", "w");
		if (s_scroll_trace_file) {
			fprintf(s_scroll_trace_file, "# Horizontal-scroll diagnostic trace (BlastEm)\n");
			fprintf(s_scroll_trace_file, "# frameRange=%u-%u lineRange=%u-%u colRange=%u-%u maxLines=%u\n",
				k_scroll_trace_frame_start, k_scroll_trace_frame_end,
				k_scroll_trace_line_start, k_scroll_trace_line_end,
				k_scroll_trace_col_start, k_scroll_trace_col_end, k_scroll_trace_max_lines);
			fflush(s_scroll_trace_file);
		}
	}
}

static uint8_t scroll_trace_enabled(vdp_context *context, uint16_t line)
{
	scroll_trace_open_once();
	return s_scroll_trace_file
		&& context->frame >= k_scroll_trace_frame_start
		&& context->frame <= k_scroll_trace_frame_end
		&& line >= k_scroll_trace_line_start
		&& line <= k_scroll_trace_line_end
		&& s_scroll_trace_lines < k_scroll_trace_max_lines;
}

static uint8_t scroll_trace_col_enabled(vdp_context *context, uint16_t line, uint16_t col)
{
	return scroll_trace_enabled(context, line)
		&& col >= k_scroll_trace_col_start
		&& col <= k_scroll_trace_col_end;
}

/* Per-line events (HS_BEGIN, HS_LOAD) — no column prefix */
static void scroll_trace_log_line(vdp_context *context, uint16_t line, const char *fmt, ...)
{
	if (!scroll_trace_enabled(context, line)) return;
	va_list args;
	va_start(args, fmt);
	fprintf(s_scroll_trace_file, "F%04u L%03u ", context->frame, line);
	vfprintf(s_scroll_trace_file, fmt, args);
	fputc('\n', s_scroll_trace_file);
	va_end(args);
	s_scroll_trace_lines++;
	if ((s_scroll_trace_lines & 0x3FF) == 0) fflush(s_scroll_trace_file);
}

/* Per-column events (HS_A_MAP, HS_B_MAP, HS_A_WIN, HS_OUT) */
static void scroll_trace_log_col(vdp_context *context, uint16_t line, uint16_t col, const char *fmt, ...)
{
	if (!scroll_trace_col_enabled(context, line, col)) return;
	va_list args;
	va_start(args, fmt);
	fprintf(s_scroll_trace_file, "F%04u L%03u C%02u ", context->frame, line, col);
	vfprintf(s_scroll_trace_file, fmt, args);
	fputc('\n', s_scroll_trace_file);
	va_end(args);
	s_scroll_trace_lines++;
	if ((s_scroll_trace_lines & 0x3FF) == 0) fflush(s_scroll_trace_file);
}

static char src_char_from_dbg(uint8_t dbg)
{
	switch (dbg & DBG_SRC_MASK)
	{
	case DBG_SRC_A:
		return 'A';
	case DBG_SRC_B:
		return 'B';
	case DBG_SRC_S:
		return 'S';
	case DBG_SRC_W:
		return 'W';
	default:
		return 'G';
	}
}

void vdp_set_trace_frame_range(uint32_t start, uint32_t end)
{
	if (end < start) {
		uint32_t tmp = start;
		start = end;
		end = tmp;
	}
	k_sprite_trace_frame_start = start;
	k_sprite_trace_frame_end = end;
	k_comp_trace_frame_start = start;
	k_comp_trace_frame_end = end;
	k_scroll_trace_frame_start = start;
	k_scroll_trace_frame_end = end;
}

static uint32_t calc_crop(uint32_t crop, uint32_t border)
{
	return crop >= border ? 0 : border - crop;
}

static void update_video_params(vdp_context *context)
{
	uint32_t top_crop = render_overscan_top();
	uint32_t bot_crop = render_overscan_bot();
	uint32_t border_top;
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		if (context->regs[REG_MODE_2] & BIT_PAL) {
			if (context->flags2 & FLAG2_REGION_PAL) {
				context->inactive_start = PAL_INACTIVE_START;
				border_top = BORDER_TOP_V30_PAL;
				context->border_bot = calc_crop(bot_crop, BORDER_BOT_V30_PAL);
			} else {
				//the behavior here is rather weird and needs more investigation
				context->inactive_start = 0xF0;
				border_top = 1;
				context->border_bot = calc_crop(bot_crop, 3);
			}
		} else {
			context->inactive_start = NTSC_INACTIVE_START;
			if (context->flags2 & FLAG2_REGION_PAL) {
				border_top = BORDER_TOP_V28_PAL;
				context->border_bot = calc_crop(bot_crop, BORDER_BOT_V28_PAL);
			} else {
				border_top = BORDER_TOP_V28;
				context->border_bot = calc_crop(bot_crop, BORDER_BOT_V28);
			}
		}
		if (context->regs[REG_MODE_4] & BIT_H40) {
			context->max_sprites_frame = MAX_SPRITES_FRAME;
			context->max_sprites_line = MAX_SPRITES_LINE;
		} else {
			context->max_sprites_frame = MAX_SPRITES_FRAME_H32;
			context->max_sprites_line = MAX_SPRITES_LINE_H32;
		}
		if (context->state == INACTIVE) {
			//Undo forced INACTIVE state due to neither Mode 4 nor Mode 5 being active
			if (context->vcounter < context->inactive_start) {
				context->state = ACTIVE;
			} else if (context->vcounter == 0x1FF) {
				context->state = PREPARING;
				if (!context->done_composite) {
					memset(context->compositebuf, 0, sizeof(context->compositebuf));
				}
			}
		}
	} else {
		context->inactive_start = MODE4_INACTIVE_START;
		if (context->flags2 & FLAG2_REGION_PAL) {
			border_top = BORDER_TOP_V24_PAL;
			context->border_bot = calc_crop(bot_crop, BORDER_BOT_V24_PAL);
		} else {
			border_top = BORDER_TOP_V24;
			context->border_bot = calc_crop(bot_crop, BORDER_BOT_V24);
		}
		if (!(context->regs[REG_MODE_1] & BIT_MODE_4) && context->type == VDP_GENESIS){
			context->state = INACTIVE;
		} else if (context->state == INACTIVE) {
			//Undo forced INACTIVE state due to neither Mode 4 nor Mode 5 being active
			if (context->vcounter < context->inactive_start) {
				context->state = ACTIVE;
			}
			else if (context->vcounter == 0x1FF) {
				context->state = PREPARING;
				if (!context->done_composite) {
					memset(context->compositebuf, 0, sizeof(context->compositebuf));
				}
			}
		}
	}
	context->border_top = calc_crop(top_crop, border_top);
	context->top_offset = border_top - context->border_top;
	context->double_res = (context->regs[REG_MODE_4] & (BIT_INTERLACE | BIT_DOUBLE_RES)) == (BIT_INTERLACE | BIT_DOUBLE_RES);
	if (!(context->regs[REG_MODE_4] & BIT_INTERLACE)) {
		context->flags2 &= ~FLAG2_EVEN_FIELD;
	}
}

static void update_color_map(vdp_context *context, uint16_t index, uint16_t value)
{
	context->colors[index] = context->color_map[value & CRAM_BITS];
	context->colors[index + SHADOW_OFFSET] = context->color_map[(value & CRAM_BITS) | FBUF_SHADOW];
	context->colors[index + HIGHLIGHT_OFFSET] = context->color_map[(value & CRAM_BITS) | FBUF_HILIGHT];
	if (context->type == VDP_GAMEGEAR) {
		context->colors[index + MODE4_OFFSET] = context->color_map[value & 0xFFF];
	} else {
		context->colors[index + MODE4_OFFSET] = context->color_map[(value & CRAM_BITS) | FBUF_MODE4];
	}
}

static uint8_t static_table_init_done;

vdp_context *init_vdp_context_int(uint8_t region_pal, uint8_t has_max_vsram, uint8_t type)
{
	vdp_context *context = calloc(1, sizeof(vdp_context) + VRAM_SIZE);
	context->sprite_draws = MAX_SPRITES_LINE;
	context->fifo_write = 0;
	context->fifo_read = -1;
	context->regs[REG_HINT] = context->hint_counter = 0xFF;
	context->vsram_size = has_max_vsram ? MAX_VSRAM_SIZE : MIN_VSRAM_SIZE;
	context->type = type;
	uint8_t b,g,r,index;
	for (uint16_t color = 0; color < (1 << 12); color++) {
		if (type == VDP_GAMEGEAR) {
			b = (color >> 8 & 0xF) * 0x11;
			g = (color >> 4 & 0xF) * 0x11;
			r = (color & 0xF) * 0x11;
		} else {
			switch (color & FBUF_MASK)
			{
			case FBUF_SHADOW:
				b = levels[(color >> 9) & 0x7];
				g = levels[(color >> 5) & 0x7];
				r = levels[(color >> 1) & 0x7];
				break;
			case FBUF_HILIGHT:
				b = levels[((color >> 9) & 0x7) + 7];
				g = levels[((color >> 5) & 0x7) + 7];
				r = levels[((color >> 1) & 0x7) + 7];
				break;
			case FBUF_MODE4:
				//TODO: Mode 4 has a separate DAC tap so this isn't quite correct
				//TODO: blue channel has one of its taps offest on SMS1 and MD
				b = levels[(color >> 4 & 0xC) | (color >> 6 & 0x2)];
				g = levels[(color >> 2 & 0x8) | (color >> 1 & 0x4) | (color >> 4 & 0x2)];
				r = levels[(color << 1 & 0xC) | (color >> 1 & 0x2)];
				break;
			case FBUF_TMS:
				index = color >> 1 & 0x7;
				index |= color >> 2 & 0x8;
				if (type == VDP_TMS9918A) {
					switch (index)
					{
					case 0:
					case 1:
						r = g = b = 0;
						break;
					case 2:
						r = 0x21; g = 0xC8; b = 0x42;
						break;
					case 3:
						r = 0x5E; g = 0xDC; b = 0x78;
						break;
					case 4:
						r = 0x54; g = 0x55; b = 0xED;
						break;
					case 5:
						r = 0x7D; g = 0x76; b = 0xFC;
						break;
					case 6:
						r = 0xD4; g = 0x52; b = 0x4D;
						break;
					case 7:
						r = 0x42; g = 0xEB; b = 0xF5;
						break;
					case 8:
						r = 0xFC; g = 0x55; b = 0x54;
						break;
					case 9:
						r = 0xFF; g = 0x79; b = 0x78;
						break;
					case 10:
						r = 0xD4; g = 0xC1; b = 0x54;
						break;
					case 11:
						r = 0xE6; g = 0xCE; b = 0x80;
						break;
					case 12:
						r = 0x21; g = 0xB0; b = 0x3B;
						break;
					case 13:
						r = 0xC9; g = 0x5B; b = 0xBA;
						break;
					case 14:
						r = g = b = 0xCC;
						break;
					case 15:
						r = g = b = 0xFF;
						break;
					}
				} else {
					static const uint8_t tms_to_sms[] = {
						0x00, 0x00, 0x08, 0x0C, 0x10, 0x30, 0x01, 0x3C, 0x02, 0x03, 0x05, 0x0F, 0x04, 0x33, 0x15, 0x3F
					};
					index = tms_to_sms[index] << 1;
					index = (index & 0xE) | (index << 1 & 0xE0);
					//TODO: Mode 4 has a separate DAC tap so this isn't quite correct
					//TODO: blue channel has one of its taps offest on SMS1 and MD
					b = levels[(index >> 4 & 0xC) | (index >> 6 & 0x2)];
					g = levels[(index >> 2 & 0x8) | (index >> 1 & 0x4) | (index >> 4 & 0x2)];
					r = levels[(index << 1 & 0xC) | (index >> 1 & 0x2)];
				}
				break;
			default:
				b = levels[(color >> 8) & 0xE];
				g = levels[(color >> 4) & 0xE];
				r = levels[color & 0xE];
			}
		}
		context->color_map[color] = render_map_color(r, g, b);
	}

	if (!static_table_init_done) {

		for (uint16_t mode4_addr = 0; mode4_addr < 0x4000; mode4_addr++)
		{
			uint16_t mode5_addr = mode4_addr & 0x3DFD;
			mode5_addr |= mode4_addr << 8 & 0x200;
			mode5_addr |= mode4_addr >> 8 & 2;
			mode4_address_map[mode4_addr] = mode5_addr;
		}
		for (uint32_t planar = 0; planar < 256; planar++)
		{
			uint32_t chunky = 0;
			for (int bit = 7; bit >= 0; bit--)
			{
				chunky = chunky << 4;
				chunky |= planar >> bit & 1;
			}
			planar_to_chunky[planar] = chunky;
		}
		static_table_init_done = 1;
	}
	for (uint8_t color = 0; color < (1 << (3 + 1 + 1 + 1)); color++)
	{
		uint8_t src = color & DBG_SRC_MASK;
		if (src > DBG_SRC_S) {
			context->debugcolors[color] = 0;
		} else {
			uint8_t r,g,b;
			b = debug_base[src][0];
			g = debug_base[src][1];
			r = debug_base[src][2];
			if (color & DBG_PRIORITY)
			{
				if (b) {
					b += 48;
				}
				if (g) {
					g += 48;
				}
				if (r) {
					r += 48;
				}
			}
			if (color & DBG_SHADOW) {
				b /= 2;
				g /= 2;
				r /=2 ;
			}
			if (color & DBG_HILIGHT) {
				if (b) {
					b += 72;
				}
				if (g) {
					g += 72;
				}
				if (r) {
					r += 72;
				}
			}
			context->debugcolors[color] = render_map_color(r, g, b);
		}
	}
	if (region_pal) {
		context->flags2 |= FLAG2_REGION_PAL;
	}
	for (int i = 0; i < 64; i++)
	{
		update_color_map(context, i, context->cram[i]);
	}
	update_video_params(context);
	
	return context;
}

static uint32_t mode5_sat_address(vdp_context *context)
{
	uint32_t addr = context->regs[REG_SAT] << 9;
	if (!(context->regs[REG_MODE_2] & BIT_128K_VRAM)) {
		addr &= 0xFFFF;
	}
	if (context->regs[REG_MODE_4] & BIT_H40) {
		addr &= 0x1FC00;
	}
	return addr;
}

static void vdp_check_update_sat(vdp_context *context, uint32_t address, uint16_t value)
{
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		if (!(address & 4)) {
			uint32_t sat_address = mode5_sat_address(context);
			if(address >= sat_address && address < (sat_address + SAT_CACHE_SIZE*2)) {
				uint16_t cache_address = address - sat_address;
				cache_address = (cache_address & 3) | (cache_address >> 1 & 0x1FC);
				context->sat_cache[cache_address] = value >> 8;
				context->sat_cache[cache_address^1] = value;
			}
		}
	}
}

void vdp_check_update_sat_byte(vdp_context *context, uint32_t address, uint8_t value)
{
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		if (!(address & 4)) {
			uint32_t sat_address = mode5_sat_address(context);
			if(address >= sat_address && address < (sat_address + SAT_CACHE_SIZE*2)) {
				uint16_t cache_address = address - sat_address;
				cache_address = (cache_address & 3) | (cache_address >> 1 & 0x1FC);
				context->sat_cache[cache_address] = value;
			}
		}
	}
}

static void write_vram_word(vdp_context *context, uint32_t address, uint16_t value)
{
	address = (address & 0x3FC) | (address >> 1 & 0xFC01) | (address >> 9 & 0x2);
	address ^= 1;
	//TODO: Support an option to actually have 128KB of VRAM
	context->vdpmem[address] = value;
}

static void write_vram_byte(vdp_context *context, uint32_t address, uint8_t value)
{
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		address &= 0xFFFF;
	} else {
		address = mode4_address_map[address & 0x3FFF];
	}
	context->vdpmem[address] = value;
}

#define VSRAM_DIRTY_BITS 0xF800

//rough estimate of slot number at which border display starts
#define BG_START_SLOT 6

void write_cram_internal(vdp_context * context, uint16_t addr, uint16_t value)
{
	context->cram[addr] = value;
	update_color_map(context, addr, value);
}

static void write_cram(vdp_context * context, uint16_t address, uint16_t value)
{
	uint16_t addr;
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		addr = (address/2) & (CRAM_SIZE-1);
	} else if (context->type == VDP_GAMEGEAR) {
		addr = (address/2) & 31;
	} else {
		addr = address & 0x1F;
		value = (value << 1 & 0xE) | (value << 2 & 0xE0) | (value & 0xE00);
	}
	write_cram_internal(context, addr, value);

	if (context->output && context->hslot >= BG_START_SLOT && (
		context->vcounter < context->inactive_start + context->border_bot
		|| context->vcounter > 0x200 - context->border_top
	)) {
		uint8_t bg_end_slot = BG_START_SLOT + (context->regs[REG_MODE_4] & BIT_H40) ? LINEBUF_SIZE/2 : (256+HORIZ_BORDER)/2;
		if (context->hslot < bg_end_slot) {
			pixel_t color = (context->regs[REG_MODE_2] & BIT_MODE_5) ? context->colors[addr] : context->colors[addr + MODE4_OFFSET];
			context->output[(context->hslot - BG_START_SLOT)*2 + 1] = color;
		}
	}
}

#ifndef _WIN32
static int vdp_render_thread_main(void *vcontext)
{
	vdp_context *context = vcontext;
	event_out event;
	for (;;)
	{
		event.autoinc = context->regs[REG_AUTOINC];
		uint8_t etype = mem_reader_next_event(&event);
		if (etype == EVENT_EOF) {
			break;
		}
		vdp_run_context(context, event.cycle);
		switch (etype)
		{
		case EVENT_ADJUST:
			vdp_adjust_cycles(context, event.address);
			break;
		case EVENT_VDP_REG:
			context->regs[event.address] = event.value;
			if (event.address == REG_MODE_4) {
				context->double_res = (event.value & (BIT_INTERLACE | BIT_DOUBLE_RES)) == (BIT_INTERLACE | BIT_DOUBLE_RES);
				if (!(event.value & BIT_INTERLACE)) {
					context->flags2 &= ~FLAG2_EVEN_FIELD;
				}
			}
			if (event.address == REG_MODE_1 || event.address == REG_MODE_2 || event.address == REG_MODE_4) {
				update_video_params(context);
			}
			break;
		case EVENT_VRAM_BYTE:
		case EVENT_VRAM_BYTE_DELTA:
		case EVENT_VRAM_BYTE_ONE:
		case EVENT_VRAM_BYTE_AUTO:
			vdp_check_update_sat_byte(context, event.address ^ 1, event.value);
			write_vram_byte(context, event.address ^ 1, event.value);
			break;
		case EVENT_VRAM_WORD:
		case EVENT_VRAM_WORD_DELTA:
			vdp_check_update_sat(context, event.address, event.value);
			write_vram_word(context, event.address, event.value);
			break;
		case EVENT_VDP_INTRAM:
			if (event.address < 128) {
				write_cram(context, event.address, event.value);
			} else {
				context->vsram[event.address&63] = event.value;
			}
			break;
		}
	}
	return 0;
}
#endif

static render_thread vdp_thread;
vdp_context *init_vdp_context(uint8_t region_pal, uint8_t has_max_vsram, uint8_t type)
{
	vdp_context *ret = init_vdp_context_int(region_pal, has_max_vsram, type);
	vdp_context *context;
#ifndef _WIN32
	if (render_is_threaded_video()) {
		context = ret->renderer = init_vdp_context_int(region_pal, has_max_vsram, type);
	} else
#endif
	{
		context = ret;
	}
	if (headless) {
		context->fb = malloc(512 * LINEBUF_SIZE * sizeof(pixel_t));
		context->output_pitch = LINEBUF_SIZE * sizeof(pixel_t);
	} else {
		context->cur_buffer = FRAMEBUFFER_ODD;
		context->fb = render_get_framebuffer(FRAMEBUFFER_ODD, &context->output_pitch);
	}
	context->output = (pixel_t *)(((char *)context->fb) + context->output_pitch * context->border_top);
#ifndef _WIN32
	if (ret->renderer) {
		event_log_mem();
		render_create_thread(&vdp_thread, "vdp_render", vdp_render_thread_main, ret->renderer);
	}
#endif
	return ret;
}

void vdp_free(vdp_context *context)
{
	if (headless) {
		free(context->fb);
	}
	if (s_sprite_trace_file && !context->renderer) {
		fflush(s_sprite_trace_file);
		fclose(s_sprite_trace_file);
		s_sprite_trace_file = NULL;
	}
	if (s_comp_trace_file && !context->renderer) {
		fflush(s_comp_trace_file);
		fclose(s_comp_trace_file);
		s_comp_trace_file = NULL;
	}
	for (int i = 0; i < NUM_DEBUG_TYPES; i++)
	{
		if (context->enabled_debuggers & (1 << i)) {
			vdp_toggle_debug_view(context, i);
		}
	}
#ifndef _WIN32
	if (context->renderer) {
		event_log_mem_stop();
		vdp_free(context->renderer);
	}
#endif
	free(context);
}

static int is_refresh(vdp_context * context, uint32_t slot)
{
	if (context->regs[REG_MODE_4] & BIT_H40) {
		return slot == 250 || slot == 26 || slot == 59 || slot == 90 || slot == 122 || slot == 154;
	} else {
		//TODO: Figure out which slots are refresh when display is off in 32-cell mode
		//These numbers are guesses based on H40 numbers
		return slot == 243 || slot == 19 || slot == 51 || slot == 83 || slot == 115;
		//The numbers below are the refresh slots during active display
		//return (slot == 29 || slot == 61 || slot == 93 || slot == 125);
	}
}

static void increment_address(vdp_context *context)
{
	context->address += context->regs[REG_AUTOINC];
	if (!(context->regs[REG_MODE_2] & BIT_MODE_5)) {
		context->address++;
	}
}

static void render_sprite_cells(vdp_context * context)
{
	if (context->cur_slot < 0) {
		//should this be 16 in H32?
		context->cur_slot += 32;
	}
	if (context->cur_slot >= MAX_SPRITES_LINE) {
		context->cur_slot--;
		return;
	}
	sprite_draw * d = context->sprite_draw_list + context->cur_slot;
	uint16_t address = d->address;
	address += context->sprite_x_offset * d->height * 4;
	context->serial_address = address;
	if (sprite_trace_enabled(context, context->vcounter) && d->width && d->height) {
		uint8_t b0 = context->vdpmem[(address + 0) & 0xFFFF];
		uint8_t b1 = context->vdpmem[(address + 1) & 0xFFFF];
		uint8_t b2 = context->vdpmem[(address + 2) & 0xFFFF];
		uint8_t b3 = context->vdpmem[(address + 3) & 0xFFFF];
		static const char hexnib[] = "0123456789ABCDEF";
		char pix[9] = {0};
		for (uint8_t px = 0; px < 8; px++) {
			uint8_t src = d->h_flip ? (uint8_t)(7 - px) : px;
			uint8_t bb = context->vdpmem[(address + (src >> 1)) & 0xFFFF];
			uint8_t nib = (src & 1) ? (bb & 0x0F) : (bb >> 4);
			pix[px] = hexnib[nib & 0xF];
		}

		uint8_t cell_h = context->double_res ? 16 : 8;
		uint8_t vert_cells = (uint8_t)(d->height / cell_h);
		uint8_t col = context->sprite_x_offset;
		uint8_t act_col = col;
		if (d->h_flip) {
			act_col = (uint8_t)(d->width - 1 - col);
		}
		uint16_t exp_tile = d->base_tile + col * vert_cells + d->row_cell;
		uint16_t exp_pat = (context->double_res ? (exp_tile << 6) : (exp_tile << 5)) + d->row_pix * 4;
		uint8_t mismatch = (address != exp_pat);
		int16_t screen_x = (int16_t)d->x_pos - 128 + col * 8;

		sprite_trace_log(context, context->vcounter,
			"SPR_CELL sat=%u draw=%u cell=%u/%u x=%d col=%u actCol=%u rowCell=%u rowPix=%u pat=%04X expPat=%04X mismatch=%u bytes=%02X%02X%02X%02X pix=%s mask=%u prevDotOvf=%u",
			(unsigned)d->sat_index, (unsigned)context->cur_slot, (unsigned)col, (unsigned)d->width,
			(int)screen_x, (unsigned)col, (unsigned)act_col, (unsigned)d->row_cell, (unsigned)d->row_pix,
			(unsigned)address, (unsigned)exp_pat, (unsigned)mismatch,
			(unsigned)b0, (unsigned)b1, (unsigned)b2, (unsigned)b3, pix,
			(unsigned)((context->flags & FLAG_MASKED) ? 1 : 0), 0u);
	}
	if (d->x_pos) {
		uint16_t dir;
		int16_t x;
		if (d->h_flip) {
			x = d->x_pos + 7 + 8 * (d->width - context->sprite_x_offset - 1);
			dir = -1;
		} else {
			x = d->x_pos + context->sprite_x_offset * 8;
			dir = 1;
		}
		context->flags |= FLAG_CAN_MASK;
		if (!(context->flags & FLAG_MASKED)) {
			x -= 128;
			//printf("Draw Slot %d of %d, Rendering sprite cell from %X to x: %d\n", context->cur_slot, context->sprite_draws, d->address, x);
			uint8_t collide = 0;
			if (x >= 8 && x < 312) {
				//sprite is fully visible
				for (; address != ((context->serial_address+4) & 0xFFFF); address++) {
					uint8_t pixel = context->vdpmem[address] >> 4;
					if (!(context->linebuf[x] & 0xF)) {
						context->linebuf[x] = pixel | d->pal_priority;
					} else {
						collide |= pixel;
					}
					x += dir;
					pixel = context->vdpmem[address] & 0xF;
					if (!(context->linebuf[x] & 0xF)) {
						context->linebuf[x] = pixel  | d->pal_priority;
					} else {
						collide |= pixel;
					}
					x += dir;
				}
			} else if (x > -8 && x < 327) {
				//sprite is partially visible
				for (; address != ((context->serial_address+4) & 0xFFFF); address++) {
					if (x >= 0 && x < 320) {
						uint8_t pixel = context->vdpmem[address] >> 4;
						if (!(context->linebuf[x] & 0xF)) {
							context->linebuf[x] = pixel | d->pal_priority;
						} else {
							collide |= pixel;
						}
					}
					x += dir;
					if (x >= 0 && x < 320) {
						uint8_t pixel = context->vdpmem[address] & 0xF;
						if (!(context->linebuf[x] & 0xF)) {
							context->linebuf[x] = pixel  | d->pal_priority;
						} else {
							collide |= pixel;
						}
					}
					x += dir;
				}
			}
			if (collide) {
				context->flags2 |= FLAG2_SPRITE_COLLIDE;
			}
		}
	} else if (context->flags & FLAG_CAN_MASK) {
		context->flags |= FLAG_MASKED;
		context->flags &= ~FLAG_CAN_MASK;
	}

	context->sprite_x_offset++;
	if (context->sprite_x_offset == d->width) {
		d->x_pos = 0;
		context->sprite_x_offset = 0;
		context->cur_slot--;
	}
}

static void fetch_sprite_cells_mode4(vdp_context * context)
{
	if (context->sprite_index >= context->sprite_draws) {
		sprite_draw * d = context->sprite_draw_list + context->sprite_index;
		uint32_t address = mode4_address_map[d->address & 0x3FFF];
		context->fetch_tmp[0] = context->vdpmem[address];
		context->fetch_tmp[1] = context->vdpmem[address + 1];
	}
}

static void render_sprite_cells_mode4(vdp_context * context)
{
	if (context->sprite_index >= context->sprite_draws) {
		uint8_t zoom = context->type != VDP_GENESIS && (context->regs[REG_MODE_2] & BIT_SPRITE_ZM);
		if (context->type == VDP_SMS && context->sprite_index < 4) {
			zoom = 0;
		}
		sprite_draw * d = context->sprite_draw_list + context->sprite_index;
		uint32_t pixels = planar_to_chunky[context->fetch_tmp[0]] << 1;
		pixels |= planar_to_chunky[context->fetch_tmp[1]];
		uint32_t address = mode4_address_map[(d->address + 2) & 0x3FFF];
		pixels |= planar_to_chunky[context->vdpmem[address]] << 3;
		pixels |= planar_to_chunky[context->vdpmem[address + 1]] << 2;
		int x = d->x_pos & 0xFF;
		for (int i = 28; i >= 0; i -= 4, x++)
		{
			uint8_t pixel = pixels >> i & 0xF;
			if (pixel) {
				if (!context->linebuf[x]) {
					context->linebuf[x] = pixel;
				} else if(
					((context->regs[REG_MODE_1] & BIT_SPRITE_8PX) && x > 8)
					|| ((!(context->regs[REG_MODE_1] & BIT_SPRITE_8PX)) && x < 256)
				) {
					context->flags2 |= FLAG2_SPRITE_COLLIDE;
				}
			}
			if (zoom) {
				x++;
				if (pixel) {
					if (!context->linebuf[x]) {
						context->linebuf[x] = pixel;
					} else if(
						((context->regs[REG_MODE_1] & BIT_SPRITE_8PX) && x > 8)
						|| ((!(context->regs[REG_MODE_1] & BIT_SPRITE_8PX)) && x < 256)
					) {
						context->flags2 |= FLAG2_SPRITE_COLLIDE;
					}
				}
			}
		}
		context->sprite_index--;
	}
}

void vdp_print_sprite_table(vdp_context * context)
{
	if (context->type == VDP_GENESIS && context->regs[REG_MODE_2] & BIT_MODE_5) {
		uint16_t sat_address = mode5_sat_address(context);
		uint16_t current_index = 0;
		uint8_t count = 0;
		do {
			uint16_t address = current_index * 8 + sat_address;
			uint16_t cache_address = current_index * 4;
			uint8_t height = ((context->sat_cache[cache_address+2] & 0x3) + 1) * 8;
			uint8_t width = (((context->sat_cache[cache_address+2]  >> 2) & 0x3) + 1) * 8;
			int16_t y = ((context->sat_cache[cache_address] & 0x3) << 8 | context->sat_cache[cache_address+1]) & 0x1FF;
			int16_t x = ((context->vdpmem[address+ 6] & 0x3) << 8 | context->vdpmem[address + 7]) & 0x1FF;
			uint16_t link = context->sat_cache[cache_address+3] & 0x7F;
			uint8_t pal = context->vdpmem[address + 4] >> 5 & 0x3;
			uint8_t pri = context->vdpmem[address + 4] >> 7;
			uint16_t pattern = ((context->vdpmem[address + 4] << 8 | context->vdpmem[address + 5]) & 0x7FF) << 5;
			printf("Sprite %d: X=%d(%d), Y=%d(%d), Width=%u, Height=%u, Link=%u, Pal=%u, Pri=%u, Pat=%X\n", current_index, x, x-128, y, y-128, width, height, link, pal, pri, pattern);
			current_index = link;
			count++;
		} while (current_index != 0 && count < 80);
	} else if (context->type != VDP_TMS9918A && context->regs[REG_MODE_1] & BIT_MODE_4) {
		uint16_t sat_address = (context->regs[REG_SAT] & 0x7E) << 7;
		for (int i = 0; i < 64; i++)
		{
			uint8_t y = context->vdpmem[mode4_address_map[sat_address + (i ^ 1)]];
			if (y == 0xD0) {
				break;
			}
			uint8_t x = context->vdpmem[mode4_address_map[sat_address + 0x80 + i*2 + 1]];
			uint16_t tile_address = context->vdpmem[mode4_address_map[sat_address + 0x80 + i*2]] * 32
				+ (context->regs[REG_STILE_BASE] << 11 & 0x2000);
			if (context->regs[REG_MODE_2] & BIT_SPRITE_SZ) {
				tile_address &= ~32;
			}
			printf("Sprite %d: X=%d, Y=%d, Pat=%X\n", i, x, y, tile_address);
		}
	} else if (context->type != VDP_GENESIS) {
		uint16_t sat_address = context->regs[REG_SAT] << 7 & 0x3F80;
		for (int i = 0; i < 32; i++)
		{
			uint16_t address = i << 2 | sat_address;
			int16_t y = context->vdpmem[mode4_address_map[address++] ^ 1];
			if (y == 208) {
				break;
			}
			if (y > 192) {
				y -= 256;
			}
			int16_t x = context->vdpmem[mode4_address_map[address++] ^ 1];
			uint8_t name = context->vdpmem[mode4_address_map[address++] ^ 1];
			uint8_t tag = context->vdpmem[mode4_address_map[address] ^ 1];
			if (tag & 0x80) {
				x -= 32;
			}
			tag &= 0xF;
			printf("Sprite %d: X=%d, Y=%d, Pat=%X, Color=%X\n", i, x, y, name, tag);
		}
	}
}

#define VRAM_READ 0 //0000
#define VRAM_WRITE 1 //0001
//2 would trigger register write 0010
#define CRAM_WRITE 3 //0011
#define VSRAM_READ 4 //0100
#define VSRAM_WRITE 5//0101
//6 would trigger regsiter write 0110
//7 is a mystery //0111
#define CRAM_READ 8  //1000
//writes go nowhere, acts 8-bit wide like VRAM //1001
//A would trigger register write 1010
//B is a mystery 1011
#define VRAM_READ8 0xC //1100
//writes go nowhere, acts 16-bit wide like VSRAM/CRAM 1101
//E would trigger register write 1110
//F is a mystery 1111

//Possible theory on how bits work
//CD0 = Read/Write flag
//CD2,(CD1|CD3) = RAM type
//  00 = VRAM
//  01 = CRAM
//  10 = VSRAM
//  11 = VRAM8
//Would result in
//  7 = VRAM8 write
//  9 = CRAM write alias
//  B = CRAM write alias
//  D = VRAM8 write alias
//  F = VRAM8 write alais

#define DMA_START 0x20

static const char * cd_name(uint8_t cd)
{
	switch (cd & 0xF)
	{
	case VRAM_READ:
		return "VRAM read";
	case VRAM_WRITE:
		return "VRAM write";
	case CRAM_WRITE:
		return "CRAM write";
	case VSRAM_READ:
		return "VSRAM read";
	case VSRAM_WRITE:
		return "VSRAM write";
	case VRAM_READ8:
		return "VRAM read (undocumented 8-bit mode)";
	default:
		return "invalid";
	}
}

void vdp_print_reg_explain(vdp_context * context)
{
	char * hscroll[] = {"full", "7-line", "cell", "line"};
	printf("**Mode Group**\n"
	       "00: %.2X | H-ints %s, Pal Select %d, HVC latch %s, Display gen %s\n"
	       "01: %.2X | Display %s, V-ints %s, Height: %d, Mode %d, %dK VRAM\n"
	       "0B: %.2X | E-ints %s, V-Scroll: %s, H-Scroll: %s\n"
	       "0C: %.2X | Width: %d, Shadow/Highlight: %s\n",
	       context->regs[REG_MODE_1], context->regs[REG_MODE_1] & BIT_HINT_EN ? "enabled" : "disabled", (context->regs[REG_MODE_1] & BIT_PAL_SEL) != 0,
	           context->regs[REG_MODE_1] & BIT_HVC_LATCH ? "enabled" : "disabled", context->regs[REG_MODE_1] & BIT_DISP_DIS ? "disabled" : "enabled",
	       context->regs[REG_MODE_2], context->regs[REG_MODE_2] & BIT_DISP_EN ? "enabled" : "disabled", context->regs[REG_MODE_2] & BIT_VINT_EN ? "enabled" : "disabled",
	           context->regs[REG_MODE_2] & BIT_PAL ? 30 : 28, context->regs[REG_MODE_2] & BIT_MODE_5 ? 5 : 4, context->regs[REG_MODE_1] & BIT_128K_VRAM ? 128 : 64,
	       context->regs[REG_MODE_3], context->regs[REG_MODE_3] & BIT_EINT_EN ? "enabled" : "disabled", context->regs[REG_MODE_3] & BIT_VSCROLL ? "2 cell" : "full",
	           hscroll[context->regs[REG_MODE_3] & 0x3],
	       context->regs[REG_MODE_4], context->regs[REG_MODE_4] & BIT_H40 ? 40 : 32, context->regs[REG_MODE_4] & BIT_HILIGHT ? "enabled" : "disabled");
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		printf("\n**Table Group**\n"
			   "02: %.2X | Scroll A Name Table:    $%.4X\n"
			   "03: %.2X | Window Name Table:      $%.4X\n"
			   "04: %.2X | Scroll B Name Table:    $%.4X\n"
			   "05: %.2X | Sprite Attribute Table: $%.4X\n"
			   "0D: %.2X | HScroll Data Table:     $%.4X\n",
			   context->regs[REG_SCROLL_A], (context->regs[REG_SCROLL_A] & 0x38) << 10,
			   context->regs[REG_WINDOW], (context->regs[REG_WINDOW] & (context->regs[REG_MODE_4] & BIT_H40 ? 0x3C : 0x3E)) << 10,
			   context->regs[REG_SCROLL_B], (context->regs[REG_SCROLL_B] & 0x7) << 13,
			   context->regs[REG_SAT], mode5_sat_address(context),
			   context->regs[REG_HSCROLL], (context->regs[REG_HSCROLL] & 0x3F) << 10);
	} else {
		printf("\n**Table Group**\n"
			   "02: %.2X | Background Name Table:  $%.4X\n"
			   "05: %.2X | Sprite Attribute Table: $%.4X\n"
			   "06: %.2X | Sprite Tile Base:       $%.4X\n"
			   "08: %.2X | Background X Scroll:    %d\n"
			   "09: %.2X | Background Y Scroll:    %d\n",
			   context->regs[REG_SCROLL_A], (context->regs[REG_SCROLL_A] & 0xE) << 10,
			   context->regs[REG_SAT], (context->regs[REG_SAT] & 0x7E) << 7,
			   context->regs[REG_STILE_BASE], (context->regs[REG_STILE_BASE] & 2) << 11,
			   context->regs[REG_X_SCROLL], context->regs[REG_X_SCROLL],
			   context->regs[REG_Y_SCROLL], context->regs[REG_Y_SCROLL]);

	}
	char * sizes[] = {"32", "64", "invalid", "128"};
	printf("\n**Misc Group**\n"
	       "07: %.2X | Backdrop Color: $%X\n"
	       "0A: %.2X | H-Int Counter: %u\n"
	       "0F: %.2X | Auto-increment: $%X\n"
	       "10: %.2X | Scroll A/B Size: %sx%s\n",
	       context->regs[REG_BG_COLOR], context->regs[REG_BG_COLOR],
	       context->regs[REG_HINT], context->regs[REG_HINT],
	       context->regs[REG_AUTOINC], context->regs[REG_AUTOINC],
	       context->regs[REG_SCROLL], sizes[context->regs[REG_SCROLL] & 0x3], sizes[context->regs[REG_SCROLL] >> 4 & 0x3]);
	char * src_types[] = {"68K", "68K", "Copy", "Fill"};
	printf("\n**DMA Group**\n"
	       "13: %.2X |\n"
		   "14: %.2X | DMA Length: $%.4X words\n"
		   "15: %.2X |\n"
		   "16: %.2X |\n"
		   "17: %.2X | DMA Source Address: $%.6X, Type: %s\n",
		   context->regs[REG_DMALEN_L],
		   context->regs[REG_DMALEN_H], context->regs[REG_DMALEN_H] << 8 | context->regs[REG_DMALEN_L],
		   context->regs[REG_DMASRC_L],
		   context->regs[REG_DMASRC_M],
		   context->regs[REG_DMASRC_H],
		       context->regs[REG_DMASRC_H] << 17 | context->regs[REG_DMASRC_M] << 9 | context->regs[REG_DMASRC_L] << 1,
			   src_types[context->regs[REG_DMASRC_H] >> 6 & 3]);
	uint8_t old_flags = context->flags;
	uint8_t old_flags2 = context->flags2;
	printf("\n**Internal Group**\n"
	       "Address: %X\n"
	       "CD:      %X - %s\n"
	       "Pending: %s\n"
		   "VCounter: %d\n"
		   "HCounter: %d\n"
		   "VINT Pending: %s\n"
		   "HINT Pending: %s\n"
		   "Status: %X\n",
	       context->address, context->cd, cd_name(context->cd),
		   (context->flags & FLAG_PENDING) ? "word" : (context->flags2 & FLAG2_BYTE_PENDING) ? "byte" : "none",
		   context->vcounter, context->hslot*2, (context->flags2 & FLAG2_VINT_PENDING) ? "true" : "false",
		   (context->flags2 & FLAG2_HINT_PENDING) ? "true" : "false", vdp_status(context));
	printf("\nDebug Register: %X | Output disabled: %s, Force Layer: %d\n", context->test_regs[0],
		(context->test_regs[0] & TEST_BIT_DISABLE)  ? "true" : "false", context->test_regs[0] >> 7 & 3
	);
}

static uint8_t is_active(vdp_context *context)
{
	return context->state != INACTIVE && (context->regs[REG_MODE_2] & BIT_DISP_EN) != 0;
}

static void scan_sprite_table(uint32_t line, vdp_context * context)
{
	if (context->sprite_index && 
		(((uint8_t)context->slot_counter) < context->max_sprites_line || !(context->flags & FLAG_SPRITE_OFLOW))
	) {
		line += 1;
		uint16_t ymask, ymin;
		uint8_t height_mult;
		if (context->double_res) {
			line *= 2;
			if (context->flags2 & FLAG2_EVEN_FIELD) {
				line++;
			}
			ymask = 0x3FF;
			ymin = 256;
			height_mult = 16;
		} else {
			ymask = 0x1FF;
			ymin = 128;
			height_mult = 8;
		}
		context->sprite_index &= 0x7F;
		//TODO: Implement squirelly behavior documented by Kabuto
		if (context->sprite_index >= MAX_SPRITES_FRAME) {
			context->sprite_index = 0;
			return;
		}
		uint16_t address = context->sprite_index * 4;
		line += ymin;
		line &= ymask;
		uint16_t y = ((context->sat_cache[address] & 0x3) << 8 | context->sat_cache[address+1]) & ymask;
		uint8_t height = ((context->sat_cache[address+2] & 0x3) + 1) * height_mult;
		//printf("Sprite %d | y: %d, height: %d\n", context->sprite_index, y, height);
		if (y <= line && line < (y + height)) {
			if (((uint8_t)context->slot_counter) == context->max_sprites_line) {
				context->flags |= FLAG_SPRITE_OFLOW;
				return;
			}
			//printf("Sprite %d at y: %d with height %d is on line %d\n", context->sprite_index, y, height, line);
			context->sprite_info_list[context->slot_counter].size = context->sat_cache[address+2];
			context->sprite_info_list[context->slot_counter++].index = context->sprite_index;
		}
		context->sprite_index = context->sat_cache[address+3] & 0x7F;
		if (context->sprite_index && ((uint8_t)context->slot_counter) < context->max_sprites_line)
		{
			//TODO: Implement squirelly behavior documented by Kabuto
			if (context->sprite_index >= MAX_SPRITES_FRAME) {
				context->sprite_index = 0;
				return;
			}
			address = context->sprite_index * 4;
			y = ((context->sat_cache[address] & 0x3) << 8 | context->sat_cache[address+1]) & ymask;
			height = ((context->sat_cache[address+2] & 0x3) + 1) * height_mult;
			//printf("Sprite %d | y: %d, height: %d\n", context->sprite_index, y, height);
			if (y <= line && line < (y + height)) {
				if (((uint8_t)context->slot_counter) == context->max_sprites_line) {
					context->flags |= FLAG_SPRITE_OFLOW;
					return;
				}
				//printf("Sprite %d at y: %d with height %d is on line %d\n", context->sprite_index, y, height, line);
				context->sprite_info_list[context->slot_counter].size = context->sat_cache[address+2];
				context->sprite_info_list[context->slot_counter++].index = context->sprite_index;
			}
			context->sprite_index = context->sat_cache[address+3] & 0x7F;
		}
	}
}

static void scan_sprite_table_mode4(vdp_context * context)
{
	if (context->sprite_index < MAX_SPRITES_FRAME_H32) {
		int16_t line = context->vcounter;
		line &= 0xFF;
		if (line > context->inactive_start) {
			line -= 0x100;
		}

		uint32_t sat_address = mode4_address_map[(context->regs[REG_SAT] << 7 & 0x3F00) + context->sprite_index];
		int16_t y = context->vdpmem[sat_address+1];
		uint32_t size = (context->regs[REG_MODE_2] & BIT_SPRITE_SZ) ? 16 : 8;
		int16_t ysize = size;
		uint8_t zoom = context->type != VDP_GENESIS && (context->regs[REG_MODE_2] & BIT_SPRITE_ZM);
		if (zoom) {
			ysize *= 2;
		}

		if (y == 0xd0) {
			context->sprite_index = MAX_SPRITES_FRAME_H32;
			return;
		} else {
			if (y > context->inactive_start) {
				y -= 0x100;
			}
			if (y <= line && line < (y + ysize)) {
				if (!context->slot_counter) {
					context->sprite_index = MAX_SPRITES_FRAME_H32;
					context->flags |= FLAG_SPRITE_OFLOW;
					return;
				}
				context->sprite_info_list[--(context->slot_counter)].size = size;
				context->sprite_info_list[context->slot_counter].index = context->sprite_index;
				context->sprite_info_list[context->slot_counter].y = y;
			}
			context->sprite_index++;
		}

		if (context->sprite_index < MAX_SPRITES_FRAME_H32) {
			y = context->vdpmem[sat_address];
			if (y == 0xd0) {
				context->sprite_index = MAX_SPRITES_FRAME_H32;
				return;
			} else {
				if (y > context->inactive_start) {
					y -= 0x100;
				}
				if (y <= line && line < (y + ysize)) {
					if (!context->slot_counter) {
						context->sprite_index = MAX_SPRITES_FRAME_H32;
						context->flags |= FLAG_SPRITE_OFLOW;
						return;
					}
					context->sprite_info_list[--(context->slot_counter)].size = size;
					context->sprite_info_list[context->slot_counter].index = context->sprite_index;
					context->sprite_info_list[context->slot_counter].y = y;
				}
				context->sprite_index++;
			}
		}

	}
}

static void read_sprite_x(uint32_t line, vdp_context * context)
{
	uint16_t log_line = (uint16_t)line;
	if (context->cur_slot == context->max_sprites_line) {
		context->cur_slot = 0;
	}
	if (context->cur_slot < context->slot_counter) {
		if (context->sprite_draws) {
			line += 1;
			//in tiles
			uint8_t width = ((context->sprite_info_list[context->cur_slot].size >> 2) & 0x3) + 1;
			//in pixels
			uint8_t height = ((context->sprite_info_list[context->cur_slot].size & 0x3) + 1) * 8;
			if (context->double_res) {
				line *= 2;
				if (context->flags2 & FLAG2_EVEN_FIELD) {
					line++;
				}
				height *= 2;
			}
			uint16_t ymask, ymin;
			if (context->double_res) {
				ymask = 0x3FF;
				ymin = 256;
			} else {
				ymask = 0x1FF;
				ymin = 128;
			}
			uint8_t index = context->sprite_info_list[context->cur_slot].index;
			if (!(context->regs[REG_MODE_4] & BIT_H40)) {
				index &= MAX_SPRITES_FRAME_H32 - 1;
			}
			uint16_t att_addr = mode5_sat_address(context) + index * 8 + 4;
			uint16_t tileinfo = (context->vdpmem[att_addr] << 8) | context->vdpmem[att_addr+1];
			uint8_t pal_priority = (tileinfo >> 9) & 0x70;
			uint8_t row;
			uint16_t cache_addr = context->sprite_info_list[context->cur_slot].index * 4;
			line = (line + ymin) & ymask;
			int16_t y = ((context->sat_cache[cache_addr] << 8 | context->sat_cache[cache_addr+1]) & ymask)/* - ymin*/;
			if (tileinfo & MAP_BIT_V_FLIP) {
				row = (y + height - 1) - line;
			} else {
				row = line-y;
			}
			row &= ymask >> 4;
			uint16_t address;
			if (context->double_res) {
				address = ((tileinfo & 0x3FF) << 6) + row * 4;
			} else {
				address = ((tileinfo & 0x7FF) << 5) + row * 4;
			}
			uint8_t cell_h = context->double_res ? 16 : 8;
			uint8_t vert_cells = (uint8_t)(height / cell_h);
			uint16_t base_tile = context->double_res ? (tileinfo & 0x3FF) : (tileinfo & 0x7FF);
			uint16_t x_raw = ((context->vdpmem[att_addr + 2] & 0x3) << 8 | context->vdpmem[att_addr + 3]) & 0x1FF;
			int16_t x_signed = (int16_t)x_raw - 128;
			uint8_t row_cell = row / cell_h;
			uint8_t row_pix = row % cell_h;
			uint8_t h_flip = (tileinfo & MAP_BIT_H_FLIP) ? 1 : 0;
			uint8_t v_flip = (tileinfo & MAP_BIT_V_FLIP) ? 1 : 0;

			context->sprite_draws--;
			sprite_draw *d = context->sprite_draw_list + context->sprite_draws;
			d->x_pos = x_raw;
			d->address = address;
			d->pal_priority = pal_priority;
			d->h_flip = h_flip;
			d->width = width;
			d->height = height;
			d->base_tile = base_tile;
			d->row_cell = row_cell;
			d->row_pix = row_pix;
			d->sat_index = context->sprite_info_list[context->cur_slot].index;

			sprite_trace_log(context, log_line,
				"SPR_SETUP sat=%u xRaw=%03X x=%d size=%ux%u tile=%03X rowCell=%u rowPix=%u hflip=%u vflip=%u palPri=%02X firstCol=%u firstTile=%03X pat=%04X",
				(unsigned)d->sat_index, (unsigned)x_raw, (int)x_signed, (unsigned)width, (unsigned)vert_cells,
				(unsigned)base_tile, (unsigned)row_cell, (unsigned)row_pix, (unsigned)h_flip, (unsigned)v_flip,
				(unsigned)pal_priority, 0u, (unsigned)(base_tile + row_cell), (unsigned)address);
		}
	}
	context->cur_slot++;
}

static void read_sprite_x_mode4(vdp_context * context)
{
	if (context->cur_slot >= context->slot_counter) {
		uint32_t address = (context->regs[REG_SAT] << 7 & 0x3F00) + 0x80 + context->sprite_info_list[context->cur_slot].index * 2;
		address = mode4_address_map[address];
		--context->sprite_draws;
		uint8_t zoom = context->type != VDP_GENESIS && (context->regs[REG_MODE_2] & BIT_SPRITE_ZM);
		uint32_t tile_address = context->vdpmem[address] * 32 + (context->regs[REG_STILE_BASE] << 11 & 0x2000);
		if (context->regs[REG_MODE_2] & BIT_SPRITE_SZ) {
			tile_address &= ~32;
		}
		int16_t line = context->vcounter & 0xFF;
		if (context->vcounter > context->inactive_start) {
			line -= 0x100;
		}
		uint16_t y_diff = line - context->sprite_info_list[context->cur_slot].y;
		if (zoom) {
			y_diff >>= 1;
		}
		tile_address += y_diff * 4;
		context->sprite_draw_list[context->sprite_draws].x_pos = context->vdpmem[address + 1];
		context->sprite_draw_list[context->sprite_draws].address = tile_address;
		context->cur_slot--;
	}
}

static void vdp_advance_dma(vdp_context * context)
{
	context->regs[REG_DMASRC_L] += 1;
	if (!context->regs[REG_DMASRC_L]) {
		context->regs[REG_DMASRC_M] += 1;
	}
	context->address += context->regs[REG_AUTOINC];
	uint16_t dma_len = ((context->regs[REG_DMALEN_H] << 8) | context->regs[REG_DMALEN_L]) - 1;
	context->regs[REG_DMALEN_H] = dma_len >> 8;
	context->regs[REG_DMALEN_L] = dma_len;
	if (!dma_len) {
		if (refdata_enabled) {
			/* Classify DMA type before we clear FLAG_DMA_RUN */
			uint8_t dmasrc_h = context->regs[REG_DMASRC_H];
			uint8_t dtype;
			if (!(dmasrc_h & 0x80))       dtype = 1; /* bus 68K */
			else if ((dmasrc_h & 0xC0) == 0x80) dtype = 2; /* fill */
			else                                dtype = 3; /* copy */
			refdata_dma_done(context, dtype);
		}
		context->flags &= ~FLAG_DMA_RUN;
		context->cd &= 0xF;
	}
}

#define DMA_FILL 0x80
#define DMA_COPY 0xC0
#define DMA_TYPE_MASK 0xC0
static void external_slot(vdp_context * context)
{
	if ((context->flags & FLAG_DMA_RUN) && (context->regs[REG_DMASRC_H] & DMA_TYPE_MASK) == DMA_FILL && context->fifo_read < 0) {
		context->fifo_read = (context->fifo_write-1) & (FIFO_SIZE-1);
		fifo_entry * cur = context->fifo + context->fifo_read;
		cur->cycle = context->cycles;
		cur->address = context->address;
		cur->partial = 1;
		vdp_advance_dma(context);
	}
	fifo_entry * start = context->fifo + context->fifo_read;
	if (context->fifo_read >= 0 && start->cycle <= context->cycles) {
		switch (start->cd & 0xF)
		{
		case VRAM_WRITE:
			if ((context->regs[REG_MODE_2] & (BIT_128K_VRAM|BIT_MODE_5)) == (BIT_128K_VRAM|BIT_MODE_5)) {
				event_vram_word(context->cycles, start->address, start->value);
				vdp_check_update_sat(context, start->address, start->value);
				write_vram_word(context, start->address, start->value);
			} else {
				uint8_t byte = start->partial == 1 ? start->value >> 8 : start->value;
				uint32_t address = start->address ^ 1;
				event_vram_byte(context->cycles, start->address, byte, context->regs[REG_AUTOINC]);
				vdp_check_update_sat_byte(context, address, byte);
				write_vram_byte(context, address, byte);
				if (!start->partial) {
					start->address = address;
					start->partial = 1;
					//skip auto-increment and removal of entry from fifo
					return;
				}
			}
			break;
		case CRAM_WRITE: {
			//printf("CRAM Write | %X to %X\n", start->value, (start->address/2) & (CRAM_SIZE-1));
			uint16_t val;
			if (start->partial == 3) {
				if (context->type == VDP_GAMEGEAR) {
					if (start->address & 1) {
						val = start->value << 8 | context->cram_latch;
					} else {
						context->cram_latch = start->value;
						break;
					}
				} else if ((start->address & 1) && (context->regs[REG_MODE_2] & BIT_MODE_5)) {
					val = (context->cram[start->address >> 1 & (CRAM_SIZE-1)] & 0xFF) | start->value << 8;
				} else {
					uint16_t address = (context->regs[REG_MODE_2] & BIT_MODE_5) ? start->address >> 1 & (CRAM_SIZE-1) : start->address & 0x1F;
					val = (context->cram[address] & 0xFF00) | start->value;
				}
			} else {
				val = start->partial ? context->fifo[context->fifo_write].value : start->value;
			}
			uint8_t buffer[3] = {start->address & 127, val >> 8, val};
			event_log(EVENT_VDP_INTRAM, context->cycles, sizeof(buffer), buffer);
			write_cram(context, start->address, val);
			break;
		}
		case VSRAM_WRITE:
			if (((start->address/2) & 63) < context->vsram_size) {
				//printf("VSRAM Write: %X to %X @ frame: %d, vcounter: %d, hslot: %d, cycle: %d\n", start->value, start->address, context->frame, context->vcounter, context->hslot, context->cycles);
				if (start->partial == 3) {
					if (start->address & 1) {
						context->vsram[(start->address/2) & 63] &= 0xFF;
						context->vsram[(start->address/2) & 63] |= start->value << 8;
					} else {
						context->vsram[(start->address/2) & 63] &= 0xFF00;
						context->vsram[(start->address/2) & 63] |= start->value;
					}
				} else {
					context->vsram[(start->address/2) & 63] = start->partial ? context->fifo[context->fifo_write].value : start->value;
				}
				uint8_t buffer[3] = {((start->address/2) & 63) + 128, context->vsram[(start->address/2) & 63] >> 8, context->vsram[(start->address/2) & 63]};
				event_log(EVENT_VDP_INTRAM, context->cycles, sizeof(buffer), buffer);
			}

			break;
		default:
			if (!(context->cd & 6) && !start->partial && (context->regs[REG_MODE_2] & (BIT_128K_VRAM|BIT_MODE_5)) != (BIT_128K_VRAM|BIT_MODE_5)) {
				start->partial = 1;
				return;
			}
		}
		context->fifo_read = (context->fifo_read+1) & (FIFO_SIZE-1);
		if (context->fifo_read == context->fifo_write) {
			if ((context->cd & 0x20) && (context->regs[REG_DMASRC_H] & DMA_TYPE_MASK) == DMA_FILL) {
				context->flags |= FLAG_DMA_RUN;
				if (refdata_enabled) {
					refdata_dma_start(context);
				}
				if (context->dma_hook) {
					context->dma_hook(context);
				}
			}
			context->fifo_read = -1;
		}
	} else if ((context->flags & FLAG_DMA_RUN) && (context->regs[REG_DMASRC_H] & DMA_TYPE_MASK) == DMA_COPY) {
		if (context->flags & FLAG_READ_FETCHED) {
			write_vram_byte(context, context->address ^ 1, context->prefetch);

			//Update DMA state
			vdp_advance_dma(context);

			context->flags &= ~FLAG_READ_FETCHED;
		} else {
			context->prefetch = context->vdpmem[(context->regs[REG_DMASRC_M] << 8) | context->regs[REG_DMASRC_L] ^ 1];

			context->flags |= FLAG_READ_FETCHED;
		}
	} else if (!(context->cd & 1) && !(context->flags & (FLAG_READ_FETCHED|FLAG_PENDING)) && context->read_latency <= context->cycles) {
		switch(context->cd & 0xF)
		{
		case VRAM_READ:
			if (context->flags2 & FLAG2_READ_PENDING) {
				//TODO: 128K VRAM support
				context->prefetch |= context->vdpmem[(context->address & 0xFFFE) | 1];
				context->flags |= FLAG_READ_FETCHED;
				context->flags2 &= ~FLAG2_READ_PENDING;
				//Should this happen after the prefetch or after the read?
				increment_address(context);
			} else {
				//TODO: 128K VRAM support
				context->prefetch = context->vdpmem[context->address & 0xFFFE] << 8;
				context->flags2 |= FLAG2_READ_PENDING;
			}
			break;
		case VRAM_READ8: {
			uint32_t address = context->address ^ 1;
			if (!(context->regs[REG_MODE_2] & BIT_MODE_5)) {
				address = mode4_address_map[address & 0x3FFF];
			}
			//TODO: 128K VRAM support
			context->prefetch = context->vdpmem[address & 0xFFFF];
			context->prefetch |= context->fifo[context->fifo_write].value & 0xFF00;
			context->flags |= FLAG_READ_FETCHED;
			//Should this happen after the prefetch or after the read?
			increment_address(context);
			break;
		}
		case CRAM_READ:
			context->prefetch = context->cram[(context->address/2) & (CRAM_SIZE-1)] & CRAM_BITS;
			context->prefetch |= context->fifo[context->fifo_write].value & ~CRAM_BITS;
			context->flags |= FLAG_READ_FETCHED;
			//Should this happen after the prefetch or after the read?
			increment_address(context);
			break;
		case VSRAM_READ: {
			uint16_t address = (context->address /2) & 63;
			if (address >= context->vsram_size) {
				address = 0;
			}
			context->prefetch = context->vsram[address] & VSRAM_BITS;
			context->prefetch |= context->fifo[context->fifo_write].value & VSRAM_DIRTY_BITS;
			context->flags |= FLAG_READ_FETCHED;
			//Should this happen after the prefetch or after the read?
			increment_address(context);
			break;
		}
		}
	}
}

static void run_dma_src(vdp_context * context, int32_t slot)
{
	//TODO: Figure out what happens if CD bit 4 is not set in DMA copy mode
	//TODO: Figure out what happens when CD:0-3 is not set to a write mode in DMA operations
	if (context->fifo_write == context->fifo_read) {
		return;
	}
	fifo_entry * cur = NULL;
	if (!(context->regs[REG_DMASRC_H] & 0x80))
	{
		//68K -> VDP
		if (slot == -1 || !is_refresh(context, slot-1)) {
			cur = context->fifo + context->fifo_write;
			cur->cycle = context->cycles + ((context->regs[REG_MODE_4] & BIT_H40) ? 16 : 20)*FIFO_LATENCY;
			cur->address = context->address;
			cur->value = read_dma_value((context->regs[REG_DMASRC_H] << 16) | (context->regs[REG_DMASRC_M] << 8) | context->regs[REG_DMASRC_L]);
			cur->cd = context->cd;
			cur->partial = 0;
			if (context->fifo_read < 0) {
				context->fifo_read = context->fifo_write;
			}
			context->fifo_write = (context->fifo_write + 1) & (FIFO_SIZE-1);
			vdp_advance_dma(context);
		}
	}
}

#define WINDOW_RIGHT 0x80
#define WINDOW_DOWN  0x80

static void read_map_scroll(uint16_t column, uint16_t vsram_off, uint32_t line, uint16_t address, uint16_t hscroll_val, vdp_context * context)
{
	uint16_t window_line_shift, v_offset_mask, vscroll_shift;
	if (context->double_res) {
		line *= 2;
		if (context->flags2 & FLAG2_EVEN_FIELD) {
			line++;
		}
		window_line_shift = 4;
		v_offset_mask = 0xF;
		vscroll_shift = 4;
	} else {
		window_line_shift = 3;
		v_offset_mask = 0x7;
		vscroll_shift = 3;
	}
	//TODO: Further research on vscroll latch behavior
	if (context->regs[REG_MODE_3] & BIT_VSCROLL) {
		if (!column) {
			if (context->regs[REG_MODE_4] & BIT_H40) {
				//Pre MD2VA4, behavior seems to vary from console to console
				//On some consoles it's a stable AND, on some it's always zero and others it's an "unstable" AND
				if (context->vsram_size == MIN_VSRAM_SIZE) {
					// For now just implement the AND behavior
					if (!vsram_off) {
						context->vscroll_latch[0] &= context->vscroll_latch[1];
						context->vscroll_latch[1] = context->vscroll_latch[0];
					}
				} else {
					//MD2VA4 and later use the column 0 value
					context->vscroll_latch[vsram_off] = context->vsram[vsram_off];
				}
			} else {
				//supposedly it's always forced to 0 in the H32 case
				//TODO: repeat H40 tests in H32 mode to confirm
				context->vscroll_latch[0] = context->vscroll_latch[1] = 0;
			}
		} else if (context->regs[REG_MODE_3] & BIT_VSCROLL) {
			context->vscroll_latch[vsram_off] = context->vsram[column - 2 + vsram_off];
		}
	}
	if (!vsram_off) {
		uint16_t left_col, right_col;
		if (context->window_h_latch & WINDOW_RIGHT) {
			left_col = (context->window_h_latch & 0x1F) * 2 + 2;
			right_col = 42;
		} else {
			left_col = 0;
			right_col = (context->window_h_latch & 0x1F) * 2;
			if (right_col) {
				right_col += 2;
			}
		}
		uint16_t top_line, bottom_line;
		if (context->window_v_latch & WINDOW_DOWN) {
			top_line = (context->window_v_latch & 0x1F) << window_line_shift;
			bottom_line = context->double_res ? 481 : 241;
		} else {
			top_line = 0;
			bottom_line = (context->window_v_latch & 0x1F) << window_line_shift;
		}
		if ((column >= left_col && column < right_col) || (line >= top_line && line < bottom_line)) {
			uint16_t address = context->regs[REG_WINDOW] << 10;
			uint16_t line_offset, offset, mask;
			if (context->regs[REG_MODE_4] & BIT_H40) {
				address &= 0xF000;
				line_offset = (((line) >> vscroll_shift) * 64 * 2) & 0xFFF;
				mask = 0x7F;

			} else {
				address &= 0xF800;
				line_offset = (((line) >> vscroll_shift) * 32 * 2) & 0xFFF;
				mask = 0x3F;
			}
			if (context->double_res) {
				mask <<= 1;
				mask |= 1;
			}
			offset = address + line_offset + (((column - 2) * 2) & mask);
			context->col_1 = (context->vdpmem[offset] << 8) | context->vdpmem[offset+1];
			//printf("Window | top: %d, bot: %d, left: %d, right: %d, base: %X, line: %X offset: %X, tile: %X, reg: %X\n", top_line, bottom_line, left_col, right_col, address, line_offset, offset, ((context->col_1 & 0x3FF) << 5), context->regs[REG_WINDOW]);
			{
				uint16_t addr1_win = offset;
				offset = address + line_offset + (((column - 1) * 2) & mask);
				context->col_2 = (context->vdpmem[offset] << 8) | context->vdpmem[offset+1];
				context->v_offset = (line) & v_offset_mask;
				/* scroll trace: HS_A_WIN */
				if (scroll_trace_col_enabled(context, context->vcounter, column)) {
					scroll_trace_log_col(context, context->vcounter, column,
						"HS_A_WIN mode=%u effLine=%u winLine=%u rowPix=%u "
						"tc1=%u tc2=%u addr1=0x%04X addr2=0x%04X col1=0x%04X col2=0x%04X "
						"hA=%u fineA=%u",
						context->regs[REG_SCROLL] & 0x3,
						(unsigned)line, (unsigned)line, context->v_offset,
						(unsigned)((((column - 2) * 2) & mask) >> 1),
						(unsigned)((((column - 1) * 2) & mask) >> 1),
						addr1_win, offset,
						context->col_1, context->col_2,
						hscroll_val, (uint16_t)(hscroll_val & 0xF));
				}
			}
			context->flags |= FLAG_WINDOW;
			return;
		} else if (column == right_col) {
			context->flags |= FLAG_WINDOW_EDGE;
			context->flags &= ~FLAG_WINDOW;
		} else {
			context->flags &= ~(FLAG_WINDOW_EDGE|FLAG_WINDOW);
		}
	}
	//TODO: Verify behavior for 0x20 case
	uint16_t vscroll = 0xFF | (context->regs[REG_SCROLL] & 0x30) << 4;
	if (context->double_res) {
		vscroll <<= 1;
		vscroll |= 1;
	}
	vscroll &= context->vscroll_latch[vsram_off] + line;
	context->v_offset = vscroll & v_offset_mask;
	//printf("%s | line %d, vsram: %d, vscroll: %d, v_offset: %d\n",(vsram_off ? "B" : "A"), line, context->vsram[context->regs[REG_MODE_3] & 0x4 ? column : 0], vscroll, context->v_offset);
	uint16_t eff_line_val = vscroll;
	vscroll >>= vscroll_shift;
	//TODO: Verify the behavior for a setting of 2
	static const uint16_t hscroll_masks[] = {0x1F, 0x3F, 0x1F, 0x7F};
	static const uint16_t v_shifts[] = {6, 7, 16, 8};
	uint16_t hscroll_mask = hscroll_masks[context->regs[REG_SCROLL] & 0x3];
	uint16_t v_shift = v_shifts[context->regs[REG_SCROLL] & 0x3];
	uint16_t hscroll, offset;
	uint16_t tc_arr[2], addr_arr[2];
	for (int i = 0; i < 2; i++) {
		hscroll = (column - 2 + i - ((hscroll_val/8) & 0xFFFE)) & hscroll_mask;
		offset = address + (((vscroll << v_shift) + hscroll*2) & 0x1FFF);
		//printf("%s | line: %d, col: %d, x: %d, hs_mask %X, scr reg: %X, tbl addr: %X\n", (vsram_off ? "B" : "A"), line, (column-2+i), hscroll, hscroll_mask, context->regs[REG_SCROLL], offset);
		tc_arr[i] = hscroll;
		addr_arr[i] = offset;
		uint16_t col_val = (context->vdpmem[offset] << 8) | context->vdpmem[offset+1];
		if (i) {
			context->col_2 = col_val;
		} else {
			context->col_1 = col_val;
		}
	}
	/* scroll trace: HS_A_MAP or HS_B_MAP */
	if (scroll_trace_col_enabled(context, context->vcounter, column)) {
		char plane = vsram_off ? 'B' : 'A';
		scroll_trace_log_col(context, context->vcounter, column,
			"HS_%c_MAP mode=%u effLine=%u v%c=%u tileRow=%u rowPix=%u "
			"h%c=%u fine%c=%u "
			"spx1=%d spx2=%d tc1=%u tc2=%u "
			"addr1=0x%04X addr2=0x%04X col1=0x%04X col2=0x%04X",
			plane,
			context->regs[REG_SCROLL] & 0x3,
			eff_line_val, plane, context->vscroll_latch[vsram_off],
			vscroll, context->v_offset,
			plane, hscroll_val, plane, (uint16_t)(hscroll_val & 0xF),
			(int)((column - 2) * 8), (int)((column - 1) * 8),
			tc_arr[0], tc_arr[1],
			addr_arr[0], addr_arr[1],
			context->col_1, context->col_2);
	}
}

static void read_map_scroll_a(uint16_t column, uint32_t line, vdp_context * context)
{
	/* scroll trace: HS_BEGIN on first column of the line */
	if (column == 0 && scroll_trace_enabled(context, (uint16_t)line)) {
		scroll_trace_log_line(context, (uint16_t)line,
			"HS_BEGIN mode=%u base=0x%04X hA=%u hB=%u fineA=%u fineB=%u",
			context->regs[REG_MODE_3] & 0x3,
			(uint16_t)((context->regs[REG_HSCROLL] & 0x3F) << 10),
			context->hscroll_a, context->hscroll_b,
			context->hscroll_a_fine, context->hscroll_b_fine);
	}
	read_map_scroll(column, 0, line, (context->regs[REG_SCROLL_A] & 0x38) << 10, context->hscroll_a, context);
}

static void read_map_scroll_b(uint16_t column, uint32_t line, vdp_context * context)
{
	read_map_scroll(column, 1, line, (context->regs[REG_SCROLL_B] & 0x7) << 13, context->hscroll_b, context);
}

static void read_map_mode4(uint16_t column, uint32_t line, vdp_context * context)
{
	uint32_t address = (context->regs[REG_SCROLL_A] & 0xE) << 10;
	//add row
	uint32_t vscroll = line;
	if (column < 24 || !(context->regs[REG_MODE_1] & BIT_VSCRL_LOCK)) {
		vscroll += context->regs[REG_Y_SCROLL];
		vscroll &= 511;
	}
	if (vscroll > 223) {
		//TODO: support V28 and V30 for SMS2/GG VDPs
		vscroll -= 224;
	}
	address += (vscroll >> 3) * 2 * 32;
	//add column
	address += ((column - (context->hscroll_a >> 3)) & 31) * 2;
	//adjust for weird VRAM mapping in Mode 4
	address = mode4_address_map[address];
	context->col_1 = (context->vdpmem[address] << 8) | context->vdpmem[address+1];
}

static void render_map(uint16_t col, uint8_t * tmp_buf, uint8_t offset, vdp_context * context)
{
	uint16_t address;
	uint16_t vflip_base;
	if (context->double_res) {
		address = ((col & 0x3FF) << 6);
		vflip_base = 60;
	} else {
		address = ((col & 0x7FF) << 5);
		vflip_base = 28;
	}
	if (col & MAP_BIT_V_FLIP) {
		address +=  vflip_base - 4 * context->v_offset;
	} else {
		address += 4 * context->v_offset;
	}
	uint8_t pal_priority = (col >> 9) & 0x70;
	uint32_t bits = *((uint32_t *)(&context->vdpmem[address]));
	tmp_buf += offset;
	if (col & MAP_BIT_H_FLIP) {
		uint32_t shift = 28;
		for (int i = 0; i < 4; i++)
		{
			uint8_t right = pal_priority | ((bits >> shift) & 0xF);
			shift -= 4;
			*(tmp_buf++) = pal_priority | ((bits >> shift) & 0xF);
			shift -= 4;
			*(tmp_buf++) = right;
		}
	} else {
		for (int i = 0; i < 4; i++)
		{
			uint8_t right = pal_priority | (bits & 0xF);
			bits >>= 4;
			*(tmp_buf++) = pal_priority | (bits & 0xF);
			bits >>= 4;
			*(tmp_buf++) = right;
		}
	}
}

static void render_map_1(vdp_context * context)
{
	render_map(context->col_1, context->tmp_buf_a, context->buf_a_off, context);
}

static void render_map_2(vdp_context * context)
{
	render_map(context->col_2, context->tmp_buf_a, context->buf_a_off+8, context);
}

static void render_map_3(vdp_context * context)
{
	render_map(context->col_1, context->tmp_buf_b, context->buf_b_off, context);
}

static void fetch_map_mode4(uint16_t col, uint32_t line, vdp_context *context)
{
	//calculate pixel row to fetch
	uint32_t vscroll = line;
	if (col < 24 || !(context->regs[REG_MODE_1] & BIT_VSCRL_LOCK)) {
		vscroll += context->regs[REG_Y_SCROLL];
	}
	if (vscroll > 223) {
		vscroll -= 224;
	}
	vscroll &= 7;
	if (context->col_1 & 0x400) {
		vscroll = 7 - vscroll;
	}

	uint32_t address = mode4_address_map[((context->col_1 & 0x1FF) * 32) + vscroll * 4];
	context->fetch_tmp[0] = context->vdpmem[address];
	context->fetch_tmp[1] = context->vdpmem[address+1];
}

static uint8_t composite_normal(vdp_context *context, uint8_t *debug_dst, uint8_t sprite, uint8_t plane_a, uint8_t plane_b, uint8_t bg_index)
{
	uint8_t pixel = bg_index;
	uint8_t src = DBG_SRC_BG;
	if (plane_b & 0xF) {
		pixel = plane_b;
		src = DBG_SRC_B;
	}
	if (plane_a & 0xF && (plane_a & BUF_BIT_PRIORITY) >= (pixel & BUF_BIT_PRIORITY)) {
		pixel = plane_a;
		src = DBG_SRC_A;
	}
	if (sprite & 0xF && (sprite & BUF_BIT_PRIORITY) >= (pixel & BUF_BIT_PRIORITY)) {
		pixel = sprite;
		src = DBG_SRC_S;
	}
	*debug_dst = src;
	return pixel;
}
typedef struct {
	uint8_t index, intensity;
} sh_pixel;

static sh_pixel composite_highlight(vdp_context *context, uint8_t *debug_dst, uint8_t sprite, uint8_t plane_a, uint8_t plane_b, uint8_t bg_index)
{
	uint8_t pixel = bg_index;
	uint8_t src = DBG_SRC_BG;
	uint8_t intensity = 0;
	if (plane_b & 0xF) {
		pixel = plane_b;
		src = DBG_SRC_B;
	}
	intensity = plane_b & BUF_BIT_PRIORITY;
	if (plane_a & 0xF && (plane_a & BUF_BIT_PRIORITY) >= (pixel & BUF_BIT_PRIORITY)) {
		pixel = plane_a;
		src = DBG_SRC_A;
	}
	intensity |= plane_a & BUF_BIT_PRIORITY;
	if (sprite & 0xF && (sprite & BUF_BIT_PRIORITY) >= (pixel & BUF_BIT_PRIORITY)) {
		if ((sprite & 0x3F) == 0x3E) {
			intensity += BUF_BIT_PRIORITY;
		} else if ((sprite & 0x3F) == 0x3F) {
			intensity = 0;
		} else {
			pixel = sprite;
			src = DBG_SRC_S;
			if ((pixel & 0xF) == 0xE) {
				intensity = BUF_BIT_PRIORITY;
			} else {
				intensity |= pixel & BUF_BIT_PRIORITY;
			}
		}
	}
	*debug_dst = src;
	return (sh_pixel){.index = pixel, .intensity = intensity};
}

static void render_normal(vdp_context *context, int32_t col, uint8_t *dst, uint8_t *debug_dst, uint8_t *buf_a, int plane_a_off, int plane_a_mask, int plane_b_off)
{
	uint8_t *sprite_buf = context->linebuf + col * 8;
	if (!col && (context->regs[REG_MODE_1] & BIT_COL0_MASK)) {
		memset(dst, 0, 8);
		memset(debug_dst, DBG_SRC_BG, 8);
		dst += 8;
		debug_dst += 8;
		sprite_buf += 8;
		plane_a_off += 8;
		plane_b_off += 8;
		for (int i = 0; i < 8; ++plane_a_off, ++plane_b_off, ++sprite_buf, ++i)
		{
			uint8_t sprite, plane_a, plane_b;
			plane_a = buf_a[plane_a_off & plane_a_mask];
			plane_b = context->tmp_buf_b[plane_b_off & SCROLL_BUFFER_MASK];
			sprite = *sprite_buf;
			uint8_t out = composite_normal(context, debug_dst, sprite, plane_a, plane_b, context->regs[REG_BG_COLOR]) & 0x3F;
			uint16_t x = (uint16_t)(col * 8 + i + 8);
			uint8_t dbg = *debug_dst;
			char src = src_char_from_dbg(dbg);
			uint8_t win_col = (context->flags & FLAG_WINDOW) ? 1 : 0;
			uint8_t win_pix = win_col;
			uint8_t shade = 1;
			uint8_t idx = out & 0x3F;
			uint8_t enc = (shade << 6) | idx;
			comp_trace_log(context, context->vcounter, x,
				"CMP f=%04u l=%03u x=%03u pA=%02X pB=%02X pS=%02X winCol=%u winPix=%u src=%c shade=%u idx=%02X enc=%02X",
				context->frame, context->vcounter, x, plane_a, plane_b, sprite,
				win_col, win_pix, src, shade, idx, enc);
			*(dst++) = out;
			debug_dst++;
		}
	} else {
		for (int i = 0; i < 16; ++plane_a_off, ++plane_b_off, ++sprite_buf, ++i)
		{
			uint8_t sprite, plane_a, plane_b;
			plane_a = buf_a[plane_a_off & plane_a_mask];
			plane_b = context->tmp_buf_b[plane_b_off & SCROLL_BUFFER_MASK];
			sprite = *sprite_buf;
			uint8_t out = composite_normal(context, debug_dst, sprite, plane_a, plane_b, context->regs[REG_BG_COLOR]) & 0x3F;
			uint16_t x = (uint16_t)(col * 8 + i);
			uint8_t dbg = *debug_dst;
			char src = src_char_from_dbg(dbg);
			uint8_t win_col = (context->flags & FLAG_WINDOW) ? 1 : 0;
			uint8_t win_pix = win_col;
			uint8_t shade = 1;
			uint8_t idx = out & 0x3F;
			uint8_t enc = (shade << 6) | idx;
			comp_trace_log(context, context->vcounter, x,
				"CMP f=%04u l=%03u x=%03u pA=%02X pB=%02X pS=%02X winCol=%u winPix=%u src=%c shade=%u idx=%02X enc=%02X",
				context->frame, context->vcounter, x, plane_a, plane_b, sprite,
				win_col, win_pix, src, shade, idx, enc);
			*(dst++) = out;
			debug_dst++;
		}
	}
}

static void render_highlight(vdp_context *context, int32_t col, uint8_t *dst, uint8_t *debug_dst, uint8_t *buf_a, int plane_a_off, int plane_a_mask, int plane_b_off)
{
	int start = 0;
	if (!col && (context->regs[REG_MODE_1] & BIT_COL0_MASK)) {
		memset(dst, SHADOW_OFFSET + (context->regs[REG_BG_COLOR] & 0x3F), 8);
		memset(debug_dst, DBG_SRC_BG | DBG_SHADOW, 8);
		dst += 8;
		debug_dst += 8;
		start = 8;
	}
	uint8_t *sprite_buf = context->linebuf + col * 8 + start;
	for (int i = start; i < 16; ++plane_a_off, ++plane_b_off, ++sprite_buf, ++i)
	{
		uint8_t sprite, plane_a, plane_b;
		plane_a = buf_a[plane_a_off & plane_a_mask];
		plane_b = context->tmp_buf_b[plane_b_off & SCROLL_BUFFER_MASK];
		sprite = *sprite_buf;
		sh_pixel pixel = composite_highlight(context, debug_dst, sprite, plane_a, plane_b, context->regs[REG_BG_COLOR]);
		uint8_t final_pixel;
		if (pixel.intensity == BUF_BIT_PRIORITY << 1) {
			final_pixel = (pixel.index & 0x3F) + HIGHLIGHT_OFFSET;
		} else if (pixel.intensity) {
			final_pixel = pixel.index & 0x3F;
		} else {
			final_pixel = (pixel.index & 0x3F) + SHADOW_OFFSET;
		}
		uint8_t shade, idx;
		if (final_pixel >= HIGHLIGHT_OFFSET) {
			shade = 2;
			idx = final_pixel - HIGHLIGHT_OFFSET;
		} else if (final_pixel >= SHADOW_OFFSET) {
			shade = 0;
			idx = final_pixel - SHADOW_OFFSET;
		} else {
			shade = 1;
			idx = final_pixel;
		}
		idx &= 0x3F;
		uint8_t enc = (shade << 6) | idx;
		uint16_t x = (uint16_t)(col * 8 + i);
		uint8_t dbg = *debug_dst;
		char src = src_char_from_dbg(dbg);
		uint8_t win_col = (context->flags & FLAG_WINDOW) ? 1 : 0;
		uint8_t win_pix = win_col;
		comp_trace_log(context, context->vcounter, x,
			"CMP f=%04u l=%03u x=%03u pA=%02X pB=%02X pS=%02X winCol=%u winPix=%u src=%c shade=%u idx=%02X enc=%02X",
			context->frame, context->vcounter, x, plane_a, plane_b, sprite,
			win_col, win_pix, src, shade, idx, enc);
		debug_dst++;
		*(dst++) = final_pixel;
	}
}

static void render_testreg(vdp_context *context, int32_t col, uint8_t *dst, uint8_t *debug_dst, uint8_t *buf_a, int plane_a_off, int plane_a_mask, int plane_b_off, uint8_t output_disabled, uint8_t test_layer)
{
	uint8_t pixel;
	if (output_disabled) {
		switch (test_layer)
		{
		case 0:
			pixel = context->regs[REG_BG_COLOR] & 0x3F;
			for (int i = 0; i < 16; i++)
			{
				*(dst++) = pixel; //Behavior confirmed on hardware by vladikcomper
				*(debug_dst++) = DBG_SRC_BG;
			}
			break;
		case 1: {
			uint8_t *sprite_buf = context->linebuf + col * 8;
			for (int i = 0; i < 16; i++)
			{
				*(dst++) = *(sprite_buf++) & 0x3F;
				*(debug_dst++) = DBG_SRC_S;
			}
			break;
		}
		case 2:
			for (int i = 0; i < 16; i++)
			{
				*(dst++) = buf_a[(plane_a_off++) & plane_a_mask] & 0x3F;
				*(debug_dst++) = DBG_SRC_A;
			}
			break;
		case 3:
			for (int i = 0; i < 16; i++)
			{
				*(dst++) = context->tmp_buf_b[(plane_b_off++) & SCROLL_BUFFER_MASK] & 0x3F;
				*(debug_dst++) = DBG_SRC_B;
			}
			break;
		}
	} else {
		int start = 0;
		uint8_t *sprite_buf = context->linebuf + col * 8;
		if (!col && (context->regs[REG_MODE_1] & BIT_COL0_MASK)) {
			//TODO: Confirm how test register interacts with column 0 blanking
			pixel = 0x3F;
			uint8_t src = DBG_SRC_BG;
			for (int i = 0; i < 8; ++i)
			{
				switch (test_layer)
				{
				case 1:
					pixel &= sprite_buf[i];
					if (pixel) {
						src = DBG_SRC_S;
					}
					break;
				case 2:
					pixel &= buf_a[(plane_a_off + i) & plane_a_mask];
					if (pixel) {
						src = DBG_SRC_A;
					}
					break;
				case 3:
					pixel &= context->tmp_buf_b[(plane_b_off + i) & SCROLL_BUFFER_MASK];
					if (pixel) {
						src = DBG_SRC_B;
					}
					break;
				}

				*(dst++) = pixel;
				*(debug_dst++) = src;
			}
			plane_a_off += 8;
			plane_b_off += 8;
			sprite_buf += 8;
			start = 8;
		}
		for (int i = start; i < 16; ++plane_a_off, ++plane_b_off, ++sprite_buf, ++i)
		{
			uint8_t sprite, plane_a, plane_b;
			plane_a = buf_a[plane_a_off & plane_a_mask];
			plane_b = context->tmp_buf_b[plane_b_off & SCROLL_BUFFER_MASK];
			sprite = *sprite_buf;
			pixel = composite_normal(context, debug_dst, sprite, plane_a, plane_b, 0x3F) & 0x3F;
			switch (test_layer)
			{
			case 1:
				pixel &= sprite;
				if (pixel) {
					*debug_dst = DBG_SRC_S;
				}
				break;
			case 2:
				pixel &= plane_a;
				if (pixel) {
					*debug_dst = DBG_SRC_A;
				}
				break;
			case 3:
				pixel &= plane_b;
				if (pixel) {
					*debug_dst = DBG_SRC_B;
				}
				break;
			}
			debug_dst++;
			*(dst++) = pixel;
		}
	}
}

static void render_testreg_highlight(vdp_context *context, int32_t col, uint8_t *dst, uint8_t *debug_dst, uint8_t *buf_a, int plane_a_off, int plane_a_mask, int plane_b_off, uint8_t output_disabled, uint8_t test_layer)
{
	int start = 0;
	uint8_t *sprite_buf = context->linebuf + col * 8;
	if (!col && (context->regs[REG_MODE_1] & BIT_COL0_MASK)) {
		//TODO: Confirm how test register interacts with column 0 blanking
		uint8_t pixel = 0x3F;
		uint8_t src = DBG_SRC_BG | DBG_SHADOW;
		for (int i = 0; i < 8; ++i)
		{
			switch (test_layer)
			{
			case 1:
				pixel &= sprite_buf[i];
				if (pixel) {
					src = DBG_SRC_S | DBG_SHADOW;
				}
				break;
			case 2:
				pixel &= buf_a[(plane_a_off + i) & plane_a_mask];
				if (pixel) {
					src = DBG_SRC_A | DBG_SHADOW;
				}
				break;
			case 3:
				pixel &= context->tmp_buf_b[(plane_b_off + i) & SCROLL_BUFFER_MASK];
				if (pixel) {
					src = DBG_SRC_B | DBG_SHADOW;
				}
				break;
			}

			*(dst++) = SHADOW_OFFSET + pixel;
			*(debug_dst++) = src;
		}
		plane_a_off += 8;
		plane_b_off += 8;
		sprite_buf += 8;
		start = 8;
	}
	for (int i = start; i < 16; ++plane_a_off, ++plane_b_off, ++sprite_buf, ++i)
	{
		uint8_t sprite, plane_a, plane_b;
		plane_a = buf_a[plane_a_off & plane_a_mask];
		plane_b = context->tmp_buf_b[plane_b_off & SCROLL_BUFFER_MASK];
		sprite = *sprite_buf;
		sh_pixel pixel = composite_highlight(context, debug_dst, sprite, plane_a, plane_b, 0x3F);
		if (output_disabled) {
			pixel.index = 0x3F;
		} else {
			pixel.index &= 0x3F;
		}
		switch (test_layer)
		{
		case 0:
			if (output_disabled) {
				pixel.index &= context->regs[REG_BG_COLOR];
				*debug_dst = DBG_SRC_BG;
			}
			break;
		case 1:
			pixel.index &= sprite;
			if (pixel.index) {
				*debug_dst = DBG_SRC_S;
			}
			break;
		case 2:
			pixel.index &= plane_a;
			if (pixel.index) {
				*debug_dst = DBG_SRC_A;
			}
			break;
		case 3:
			pixel.index &= plane_b;
			if (pixel.index) {
				*debug_dst = DBG_SRC_B;
			}
			break;
		}
		if (pixel.intensity == BUF_BIT_PRIORITY << 1) {
			pixel.index += HIGHLIGHT_OFFSET;
		} else if (!pixel.intensity) {
			pixel.index += SHADOW_OFFSET;
		}
		debug_dst++;
		*(dst++) = pixel.index;
	}
}

static void render_map_output(uint32_t line, int32_t col, vdp_context * context)
{
	uint8_t *dst;
	uint8_t *debug_dst;
	uint8_t output_disabled = (context->test_regs[0] & TEST_BIT_DISABLE) != 0;
	uint8_t test_layer = context->test_regs[0] >> 7 & 3;
	if (context->state == PREPARING && !test_layer) {
		if (col) {
			col -= 2;
			dst = context->compositebuf + BORDER_LEFT + col * 8;
		} else {
			dst = context->compositebuf;
			pixel_t bg_color = context->colors[context->regs[REG_BG_COLOR] & 0x3F];
			memset(dst, 0, BORDER_LEFT);
			context->done_composite = dst + BORDER_LEFT;
			return;
		}
		memset(dst, 0, 16);
		context->done_composite = dst + 16;
		return;
	}
	line &= 0xFF;
	render_map(context->col_2, context->tmp_buf_b, context->buf_b_off+8, context);
	uint8_t *sprite_buf;
	uint8_t sprite, plane_a, plane_b;
	int plane_a_off, plane_b_off;
	if (col)
	{
		col-=2;
		dst = context->compositebuf + BORDER_LEFT + col * 8;
		debug_dst = context->layer_debug_buf + BORDER_LEFT + col * 8;


		uint8_t a_src, src;
		uint8_t *buf_a;
		int plane_a_mask;
		if (context->flags & FLAG_WINDOW) {
			plane_a_off = context->buf_a_off;
			buf_a = context->tmp_buf_a;
			a_src = DBG_SRC_W;
			plane_a_mask = SCROLL_BUFFER_MASK;
		} else {
			if (context->flags & FLAG_WINDOW_EDGE) {
				buf_a = context->tmp_buf_a + context->buf_a_off;
				plane_a_mask = 15;
				plane_a_off = -context->hscroll_a_fine;
			} else {
				plane_a_off = context->buf_a_off - context->hscroll_a_fine;
				plane_a_mask = SCROLL_BUFFER_MASK;
				buf_a = context->tmp_buf_a;
			}
			a_src = DBG_SRC_A;
		}
		plane_a_off &= plane_a_mask;
		plane_b_off = context->buf_b_off - context->hscroll_b_fine;
		//printf("A | tmp_buf offset: %d\n", 8 - (context->hscroll_a & 0x7));
		/* scroll trace: HS_OUT */
		if (scroll_trace_col_enabled(context, context->vcounter, (uint16_t)(col + 2))) {
			scroll_trace_log_col(context, context->vcounter, (uint16_t)(col + 2),
				"HS_OUT outX=%d hA=%u hB=%u fineA=%u fineB=%u "
				"bufAOff=%d bufBOff=%d winCol=%u",
				col * 8,
				context->hscroll_a, context->hscroll_b,
				context->hscroll_a_fine, context->hscroll_b_fine,
				plane_a_off, plane_b_off,
				(context->flags & FLAG_WINDOW) ? 1u : 0u);
		}

		if (context->regs[REG_MODE_4] & BIT_HILIGHT) {
			if (output_disabled || test_layer) {
				render_testreg_highlight(context, col, dst, debug_dst, buf_a, plane_a_off, plane_a_mask, plane_b_off, output_disabled, test_layer);
			} else {
				render_highlight(context, col, dst, debug_dst, buf_a, plane_a_off, plane_a_mask, plane_b_off);
			}
		} else {
			if (output_disabled || test_layer) {
				render_testreg(context, col, dst, debug_dst, buf_a, plane_a_off, plane_a_mask, plane_b_off, output_disabled, test_layer);
			} else {
				render_normal(context, col, dst, debug_dst, buf_a, plane_a_off, plane_a_mask, plane_b_off);
			}
		}
		dst += 16;
	} else {
		dst = context->compositebuf;
		debug_dst = context->layer_debug_buf;
		uint8_t pixel = 0;
		if (output_disabled) {
			pixel = 0x3F;
		}
		if (test_layer) {
			switch(test_layer)
			{
			case 1:
				memset(dst, 0, BORDER_LEFT);
				memset(debug_dst, DBG_SRC_BG, BORDER_LEFT);
				dst += BORDER_LEFT;
				break;
			case 2: {
				//plane A
				//TODO: Deal with Window layer
				int i;
				i = 0;
				uint8_t buf_off = context->buf_a_off - context->hscroll_a_fine + (16 - BORDER_LEFT);
				//uint8_t *src = context->tmp_buf_a + ((context->buf_a_off + (i ? 0 : (16 - BORDER_LEFT) - (context->hscroll_a & 0xF))) & SCROLL_BUFFER_MASK);
				for (; i < BORDER_LEFT; buf_off++, i++, dst++, debug_dst++)
				{
					*dst = context->tmp_buf_a[buf_off & SCROLL_BUFFER_MASK];
					*debug_dst = DBG_SRC_A;
				}
				break;
			}
			case 3: {
				//plane B
				int i;
				i = 0;
				uint8_t buf_off = context->buf_b_off - context->hscroll_b_fine + (16 - BORDER_LEFT);
				//uint8_t *src = context->tmp_buf_b + ((context->buf_b_off + (i ? 0 : (16 - BORDER_LEFT) - (context->hscroll_b & 0xF))) & SCROLL_BUFFER_MASK);
				for (; i < BORDER_LEFT; buf_off++, i++, dst++, debug_dst++)
				{
					*dst = context->tmp_buf_b[buf_off & SCROLL_BUFFER_MASK];
					*debug_dst = DBG_SRC_B;
				}
				break;
			}
			}
		} else {
			memset(dst, pixel, BORDER_LEFT);
			memset(debug_dst, DBG_SRC_BG, BORDER_LEFT);
			dst += BORDER_LEFT;
		}
	}
	context->done_composite = dst;
	context->buf_a_off = (context->buf_a_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_MASK;
	context->buf_b_off = (context->buf_b_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_MASK;
}

static void render_map_mode4(uint32_t line, int32_t col, vdp_context * context)
{
	uint32_t vscroll = line;
	if (col < 24 || !(context->regs[REG_MODE_1] & BIT_VSCRL_LOCK)) {
		vscroll += context->regs[REG_Y_SCROLL];
	}
	if (vscroll > 223) {
		vscroll -= 224;
	}
	vscroll &= 7;
	if (context->col_1 & 0x400) {
		//vflip
		vscroll = 7 - vscroll;
	}

	uint32_t pixels = planar_to_chunky[context->fetch_tmp[0]] << 1;
	pixels |=  planar_to_chunky[context->fetch_tmp[1]];

	uint32_t address = mode4_address_map[((context->col_1 & 0x1FF) * 32) + vscroll * 4 + 2];
	pixels |= planar_to_chunky[context->vdpmem[address]] << 3;
	pixels |= planar_to_chunky[context->vdpmem[address+1]] << 2;

	int i, i_inc, i_limit;
	if (context->col_1 & 0x200) {
		//hflip
		i = 0;
		i_inc = 4;
		i_limit = 32;
	} else {
		i = 28;
		i_inc = -4;
		i_limit = -4;
	}
	uint8_t pal_priority = (context->col_1 >> 7 & 0x10) | (context->col_1 >> 6 & 0x40);
	for (uint8_t *dst = context->tmp_buf_a + context->buf_a_off; i != i_limit; i += i_inc, dst++)
	{
		*dst = (pixels >> i & 0xF) | pal_priority;
	}
	context->buf_a_off = (context->buf_a_off + 8) & 15;

	uint8_t *dst = context->compositebuf + col * 8 + BORDER_LEFT;
	uint8_t *debug_dst = context->layer_debug_buf + col * 8 + BORDER_LEFT;
	if (context->state == PREPARING) {
		memset(dst, 0x10 + (context->regs[REG_BG_COLOR] & 0xF) + MODE4_OFFSET, 8);
		memset(debug_dst, DBG_SRC_BG, 8);
		context->done_composite = dst + 8;
		return;
	}

	if (col || !(context->regs[REG_MODE_1] & BIT_COL0_MASK)) {
		uint8_t *sprite_src = context->linebuf + col * 8;
		if (context->regs[REG_MODE_1] & BIT_SPRITE_8PX) {
			sprite_src += 8;
		}
		for (int i = 0; i < 8; i++, sprite_src++)
		{
			uint8_t *bg_src = context->tmp_buf_a + ((8 + i + col * 8 - (context->hscroll_a & 0x7)) & 15);
			if ((*bg_src & 0x4F) > 0x40 || !*sprite_src) {
				//background plane has priority and is opaque or sprite layer is transparent
				uint8_t pixel = *bg_src & 0x1F;
				*(dst++) = pixel + MODE4_OFFSET;
				*(debug_dst++) = pixel ? DBG_SRC_A : DBG_SRC_BG;
			} else {
				//sprite layer is opaque and not covered by high priority BG pixels
				*(dst++) = (*sprite_src | 0x10) + MODE4_OFFSET;
				*(debug_dst++) = DBG_SRC_S;
			}
		}
		context->done_composite = dst;
	} else {
		memset(dst, 0x10 + (context->regs[REG_BG_COLOR] & 0xF) + MODE4_OFFSET, 8);
		memset(debug_dst, DBG_SRC_BG, 8);
		context->done_composite = dst + 8;
	}
}

static uint32_t const h40_hsync_cycles[] = {19, 20, 20, 20, 18, 20, 20, 20, 18, 20, 20, 20, 18, 20, 20, 20, 19};

static void vdp_advance_line(vdp_context *context)
{
#ifdef TIMING_DEBUG
	static uint32_t last_line = 0xFFFFFFFF;
	if (last_line != 0xFFFFFFFF) {
		uint32_t diff = context->cycles - last_line;
		if (diff != MCLKS_LINE) {
			printf("Line %d took %d cycles\n", context->vcounter, diff);
		}
	}
	last_line = context->cycles;
#endif
	uint16_t jump_start, jump_end;
	uint8_t is_mode_5 = context->regs[REG_MODE_2] & BIT_MODE_5;
	if (is_mode_5) {
		if (context->flags2 & FLAG2_REGION_PAL) {
			if (context->regs[REG_MODE_2] & BIT_PAL) {
				jump_start = 0x10B;
				jump_end = 0x1D2;
			} else {
				jump_start = 0x103;
				jump_end = 0x1CA;
			}
		} else if (context->regs[REG_MODE_2] & BIT_PAL) {
			jump_start = 0x100;
			jump_end = 0x1FA;
		} else {
			jump_start = 0xEB;
			jump_end = 0x1E5;
		}
	} else {
		//TODO: extended heights on VDPs with that feature
		if (context->flags2 & FLAG2_REGION_PAL) {
			jump_start = 0xF3;
			jump_end = 0x1BA;
		} else {
			jump_start = 0xDB;
			jump_end = 0x1D5;
		}
	}

	if (context->enabled_debuggers & (1 << DEBUG_CRAM | 1 << DEBUG_COMPOSITE)) {
		uint32_t line = context->vcounter;
		if (line >= jump_end) {
			line -= jump_end - jump_start;
		}
		uint32_t total_lines = (context->flags2 & FLAG2_REGION_PAL) ? 313 : 262;

		if (total_lines - line <= context->border_top) {
			line -= total_lines - context->border_top;
		} else {
			line += context->border_top;
		}
		if (context->enabled_debuggers & (1 << DEBUG_CRAM)) {
			pixel_t *fb = context->debug_fbs[DEBUG_CRAM] + context->debug_fb_pitch[DEBUG_CRAM] * line / sizeof(pixel_t);
			if (context->regs[REG_MODE_2] & BIT_MODE_5) {
				for (int i = 0; i < 64; i++)
				{
					for (int x = 0; x < 8; x++)
					{
						*(fb++) = context->colors[i];
					}
				}
			} else if (context->type == VDP_GENESIS || (context->regs[REG_MODE_1] & BIT_MODE_4)) {
				for (int i = MODE4_OFFSET; i < MODE4_OFFSET+32; i++)
				{
					for (int x = 0; x < 16; x++)
					{
						*(fb++) = context->colors[i];
					}
				}
			} else if (context->type != VDP_GENESIS) {
				uint16_t address = context->regs[REG_COLOR_TABLE] << 6;
				for (int i = 0; i < 32; i++, address++)
				{
					uint8_t entry = context->vdpmem[mode4_address_map[address] ^ 1];
					uint8_t fg = entry >> 4, bg = entry & 0xF;
					pixel_t fg_full, bg_full;
					if (context->type == VDP_GAMEGEAR) {
						//Game Gear uses CRAM entries 16-31 for TMS9918A modes
						fg_full = context->colors[fg + 16 + MODE4_OFFSET];
						bg_full = context->colors[bg + 16 + MODE4_OFFSET];
					} else {
						fg <<= 1;
						fg = (fg & 0xE) | (fg << 1 & 0x20);
						fg_full = context->color_map[fg | FBUF_TMS];
						bg <<= 1;
						bg = (bg & 0xE) | (bg << 1 & 0x20);
						bg_full = context->color_map[bg | FBUF_TMS];
					}
					for (int x = 0; x < 8; x++)
					{
						*(fb++) = fg_full;
					}
					for (int x = 0; x < 8; x++)
					{
						*(fb++) = bg_full;
					}
				}
			}
		}
		if (
			context->enabled_debuggers & (1 << DEBUG_COMPOSITE)
			&& line < (context->inactive_start + context->border_bot + context->border_top)
		) {
			pixel_t *fb = context->debug_fbs[DEBUG_COMPOSITE] + context->debug_fb_pitch[DEBUG_COMPOSITE] * line / sizeof(pixel_t);
			if (is_mode_5) {
				uint32_t left, right;
				uint16_t top_line, bottom_line;
				if (context->window_v_latch & WINDOW_DOWN) {
					top_line = ((context->window_v_latch & 0x1F) << 3) + context->border_top;
					bottom_line = context->inactive_start + context->border_top;
				} else {
					top_line = context->border_top;
					bottom_line = ((context->window_v_latch & 0x1F) << 3) + context->border_top;
				}
				if (line >= top_line && line < bottom_line) {
					left = 0;
					right = 320 + BORDER_LEFT + BORDER_RIGHT;
				} else if (context->window_h_latch & WINDOW_RIGHT) {
					left = (context->window_h_latch & 0x1F) * 16 + BORDER_LEFT;
					right = 320 + BORDER_LEFT + BORDER_RIGHT;
				} else {
					left = 0;
					right = (context->window_h_latch & 0x1F) * 16 + BORDER_LEFT;
				}
				for (uint32_t i = left; i < right; i++)
				{
					uint8_t src = context->layer_debug_buf[i] & DBG_SRC_MASK;
					if (src == DBG_SRC_A) {
						context->layer_debug_buf[i] &= ~DBG_SRC_MASK;
						context->layer_debug_buf[i] |= DBG_SRC_W;
					}
				}
			}
			for (int i = 0; i < LINEBUF_SIZE; i++)
			{
				*(fb++) = context->debugcolors[context->layer_debug_buf[i]];
			}
		}
	}

	context->vcounter++;
	if (context->renderer && context->vcounter == context->inactive_start) {
		context->frame++;
	}
	if (is_mode_5) {
		context->window_h_latch = context->regs[REG_WINDOW_H];
		context->window_v_latch = context->regs[REG_WINDOW_V];
	}
	if (context->vcounter == jump_start) {
		context->vcounter = jump_end;
	} else {
		context->vcounter &= 0x1FF;
	}
	if (context->state == PREPARING) {
		context->state = ACTIVE;
	}
	if (context->vcounter == 0x1FF) {
		context->flags2 &= ~FLAG2_PAUSE;
	}

	if (context->state != ACTIVE) {
		context->hint_counter = context->regs[REG_HINT];
	} else if (context->hint_counter) {
		context->hint_counter--;
	} else {
		context->flags2 |= FLAG2_HINT_PENDING;
		context->pending_hint_start = context->cycles;
		context->hint_counter = context->regs[REG_HINT];
	}
}

static void vram_debug_mode5(pixel_t *fb, uint32_t pitch, vdp_context *context)
{
	uint8_t pal = (context->debug_modes[DEBUG_VRAM] % 4) << 4;
	int yshift, ymask, tilesize;
	if (context->double_res) {
		yshift = 5;
		ymask = 0xF;
		tilesize = 64;
	} else {
		yshift = 4;
		ymask = 0x7;
		tilesize = 32;
	}
	for (int y = 0; y < 512; y++)
	{
		pixel_t *line = fb + y * pitch / sizeof(pixel_t);
		int row = y >> yshift;
		int yoff = y >> 1 & ymask;
		for (int col = 0; col < 64; col++)
		{
			uint16_t address = (row * 64 + col) * tilesize + yoff * 4;
			for (int x = 0; x < 4; x++)
			{
				uint8_t byte = context->vdpmem[address++];
				uint8_t left = byte >> 4 | pal;
				uint8_t right = byte & 0xF | pal;
				*(line++) = context->colors[left];
				*(line++) = context->colors[left];
				*(line++) = context->colors[right];
				*(line++) = context->colors[right];
			}
		}
	}
}

static void vram_debug_mode4(pixel_t *fb, uint32_t pitch, vdp_context *context)
{
	for (int y = 0; y < 256; y++)
	{
		pixel_t *line = fb + y * pitch / sizeof(pixel_t);
		int row = y >> 4;
		int yoff = y >> 1 & 7;
		for (int col = 0; col < 64; col++)
		{
			uint8_t pal = (col >= 32) << 4;
			uint16_t address = (row * 32 + (col & 31)) * 32 + yoff * 4;
			uint32_t pixels = 0;
			for (int x = 0; x < 4; x++)
			{
				uint8_t byte = context->vdpmem[mode4_address_map[address++]];
				pixels |= planar_to_chunky[byte] << (x ^ 1);
			}
			for (int x = 0; x < 32; x+=4)
			{
				uint8_t pixel = (pixels >> (28 - x) & 0xF) | pal;
				*(line++) = context->colors[pixel + MODE4_OFFSET];
				*(line++) = context->colors[pixel + MODE4_OFFSET];
			}
		}
	}
}

static void vram_debug_tms(pixel_t *fb, uint32_t pitch, vdp_context *context)
{
	uint8_t pal = ((context->debug_modes[DEBUG_VRAM] % 14) + 2) << 1;
	pal = (pal & 0xE) | (pal << 1 & 0x20);
	for (int y = 0; y < 512; y++)
	{
		pixel_t *line = fb + y * pitch / sizeof(pixel_t);
		int row = y >> 4;
		int yoff = y >> 1 & 7;
		for (int col = 0; col < 64; col++)
		{
			uint16_t address = (row * 64 + col) * 8 + yoff;
			uint8_t byte = context->vdpmem[mode4_address_map[address^1]];
			for (int x = 0; x < 8; x++)
			{
				uint16_t pixel = (byte & 0x80) ? pal : 0;
				byte <<= 1;
				*(line++) = context->color_map[pixel | FBUF_TMS];
				*(line++) = context->color_map[pixel | FBUF_TMS];
			}
		}
	}
}

static void plane_debug_mode5(pixel_t *fb, uint32_t pitch, vdp_context *context)
{
	uint16_t hscroll_mask;
	uint16_t v_mul;
	uint16_t vscroll_mask = 0x1F | (context->regs[REG_SCROLL] & 0x30) << 1;
	switch(context->regs[REG_SCROLL] & 0x3)
	{
	case 0:
		hscroll_mask = 0x1F;
		v_mul = 64;
		break;
	case 0x1:
		hscroll_mask = 0x3F;
		v_mul = 128;
		break;
	case 0x2:
		//TODO: Verify this behavior
		hscroll_mask = 0x1F;
		v_mul = 0;
		break;
	case 0x3:
		hscroll_mask = 0x7F;
		v_mul = 256;
		break;
	}
	uint16_t table_address;
	switch(context->debug_modes[DEBUG_PLANE] & 3)
	{
	case 0:
		table_address = context->regs[REG_SCROLL_A] << 10 & 0xE000;
		break;
	case 1:
		table_address = context->regs[REG_SCROLL_B] << 13 & 0xE000;
		break;
	case 2:
		table_address = context->regs[REG_WINDOW] << 10;
		if (context->regs[REG_MODE_4] & BIT_H40) {
			table_address &= 0xF000;
			v_mul = 128;
			hscroll_mask = 0x3F;
		} else {
			table_address &= 0xF800;
			v_mul = 64;
			hscroll_mask = 0x1F;
		}
		vscroll_mask = 0x1F;
		break;
	}
	pixel_t bg_color = context->colors[context->regs[REG_BG_COLOR] & 0x3F];
	uint16_t num_rows;
	int num_lines;
	if (context->double_res) {
		num_rows = 64;
		num_lines = 16;
	} else {
		num_rows = 128;
		num_lines = 8;
	}
	for (uint16_t row = 0; row < num_rows; row++)
	{
		uint16_t row_address = table_address + (row & vscroll_mask) * v_mul;
		for (uint16_t col = 0; col < 128; col++)
		{
			uint16_t address = row_address + (col & hscroll_mask) * 2;
			//pccv hnnn nnnn nnnn
			//
			uint16_t entry = context->vdpmem[address] << 8 | context->vdpmem[address + 1];
			uint8_t pal = entry >> 9 & 0x30;

			pixel_t *dst = fb + (row * pitch * num_lines / sizeof(pixel_t)) + col * 8;
			if (context->double_res) {
				address = (entry & 0x3FF) * 64;
			} else {
				address = (entry & 0x7FF) * 32;
			}
			int y_diff = 4;
			if (entry & 0x1000) {
				y_diff = -4;
				address += (num_lines - 1) * 4;
			}
			int x_diff = 1;
			if (entry & 0x800) {
				x_diff = -1;
				address += 3;
			}
			for (int y = 0; y < num_lines; y++)
			{
				uint16_t trow_address = address;
				pixel_t *row_dst = dst;
				for (int x = 0; x < 4; x++)
				{
					uint8_t byte = context->vdpmem[trow_address];
					trow_address += x_diff;
					uint8_t left, right;
					if (x_diff > 0) {
						left = byte >> 4;
						right = byte & 0xF;
					} else {
						left = byte & 0xF;
						right = byte >> 4;
					}
					*(row_dst++) = left ? context->colors[left|pal] : bg_color;
					*(row_dst++) = right ? context->colors[right|pal] : bg_color;
				}
				address += y_diff;
				dst += pitch / sizeof(pixel_t);
			}
		}
	}
	if (context->debug_flags & DEBUG_FLAG_PLANE_BORDER) {
		pixel_t border_line = render_map_color(255, 0, 255);
		if ((context->debug_modes[DEBUG_PLANE] & 3) < 2) {
			uint16_t hscroll_base = (context->regs[REG_HSCROLL] & 0x3F) << 10;
			hscroll_base += (context->debug_modes[DEBUG_PLANE] & 1) * 2;
			uint16_t hscroll_mask = 0;
			if (context->regs[REG_MODE_3] & 0x2) {
				hscroll_mask |= 0xF8;
			}
			if (context->regs[REG_MODE_3] & 0x1) {
				hscroll_mask |= 0x7;
			}
			uint16_t hscroll = 1023 - (context->vdpmem[hscroll_base] << 8 | context->vdpmem[hscroll_base+1]) & 1023;
			uint16_t width = (context->regs[REG_MODE_4] & BIT_H40) ? 320 : 256;
			if (context->regs[REG_MODE_3] & BIT_VSCROLL) {
				//2-column vscroll
				uint16_t num_cols = width >> 4;
				for (int y_off = -2; y_off; y_off++)
				{
					for (uint16_t col = 0; col < num_cols; col++)
					{
						uint16_t vscroll = context->vsram[col * 2 + (context->debug_modes[DEBUG_PLANE] & 1)] & 1023;
						int y = (vscroll + y_off) & 1023;
						pixel_t *line = fb + y * pitch / sizeof(pixel_t);
						for (int x = col ? hscroll + col * 16 : hscroll - 2, x_end = col == num_cols-1 ? hscroll + width + 2 : x + (col ? 16 : 18); x < x_end; x++)
						{
							line[x & 1023] = border_line;
						}
					}
				}
				for (int y = context->vsram[context->debug_modes[DEBUG_PLANE] & 1], y_end = y + context->inactive_start; y < y_end; y++)
				{
					pixel_t *line = fb + (y & 1023) * pitch / sizeof(pixel_t);
					line[(hscroll - 2) & 1023] = border_line;
					line[(hscroll - 1) & 1023] = border_line;
					uint16_t hscroll_addr = hscroll_base + ((y - (y_end - context->inactive_start)) & hscroll_mask) * 4;
					hscroll = 1024 - (context->vdpmem[hscroll_addr] << 8 | context->vdpmem[hscroll_addr+1]) & 1023;
				}
				for (int y = context->vsram[19 * 2 + (context->debug_modes[DEBUG_PLANE] & 1)], y_end = y + context->inactive_start; y < y_end; y++)
				{
					pixel_t *line = fb + (y & 1023) * pitch / sizeof(pixel_t);
					line[(hscroll + width) & 1023] = border_line;
					line[(hscroll + width + 1) & 1023] = border_line;
					uint16_t hscroll_addr = hscroll_base + ((y - (y_end - context->inactive_start)) & hscroll_mask) * 4;
					hscroll = 1024 - (context->vdpmem[hscroll_addr] << 8 | context->vdpmem[hscroll_addr+1]) & 1023;
				}
				for (int y_off = context->inactive_start; y_off < context->inactive_start + 2; y_off++)
				{
					for (uint16_t col = 0; col < num_cols; col++)
					{
						uint16_t vscroll = context->vsram[col * 2 + (context->debug_modes[DEBUG_PLANE] & 1)] & 1023;
						int y = (vscroll + y_off) & 1023;
						pixel_t *line = fb + y * pitch / sizeof(pixel_t);
						for (int x = col ? hscroll + col * 16 : hscroll - 2, x_end = col == num_cols-1 ? hscroll + width + 2 : x + (col ? 16 : 18); x < x_end; x++)
						{
							line[x & 1023] = border_line;
						}
					}
				}
			} else {
				//full screen vscroll
				uint16_t vscroll = context->vsram[context->debug_modes[DEBUG_PLANE] & 1] & 1023;
				int y = (vscroll - 2) & 1023;
				pixel_t *line = fb + y * pitch / sizeof(pixel_t);
				for (int x = hscroll - 2, x_end = hscroll + width + 2; x < x_end; x++)
				{
					line[x & 1023] = border_line;
				}
				y = (y + 1) & 1023;
				line = fb + y * pitch / sizeof(pixel_t);
				for (int x = hscroll - 1, x_end = hscroll + width + 1; x < x_end; x++)
				{
					line[x & 1023] = border_line;
				}
				int y_end = y + context->inactive_start + 1;
				for (y++; y < y_end; y++)
				{
					line = fb + (y & 1023) * pitch / sizeof(pixel_t);
					line[(hscroll - 2) & 1023] = border_line;
					line[(hscroll - 1) & 1023] = border_line;
					line[(hscroll + width) & 1023] = border_line;
					line[(hscroll + width + 1) & 1023] = border_line;
					uint16_t hscroll_last = hscroll;
					uint16_t hscroll_addr = hscroll_base + ((y - vscroll) & hscroll_mask) * 4;
					hscroll = 1024 - (context->vdpmem[hscroll_addr] << 8 | context->vdpmem[hscroll_addr+1]) & 1023;
					if (abs(hscroll - hscroll_last) > 512) {
						//handle wraparound
						if (hscroll > hscroll_last) {
							hscroll_last += 1024;
						} else {
							hscroll += 1024;
						}
					}
					if (hscroll_last < hscroll) {
						for (int x = hscroll_last + width + 2, x_end = hscroll + width; x < x_end; x++)
						{
							line[x & 1023] = border_line;
						}
						line = fb + ((y+1) & 1023) * pitch / sizeof(pixel_t);
						for (int x = hscroll_last, x_end = hscroll; x < x_end; x++)
						{
							line[x & 1023] = border_line;
						}
					} else if (hscroll_last > hscroll) {
						for (int x = hscroll, x_end = hscroll_last; x < x_end; x++)
						{
							line[x & 1023] = border_line;
						}
						line = fb + ((y+1) & 1023) * pitch / sizeof(pixel_t);
						for (int x = hscroll + width + 2, x_end = hscroll_last + width; x < x_end; x++)
						{
							line[x & 1023] = border_line;
						}
					}
				}
				line = fb + (y_end & 1023) * pitch / sizeof(pixel_t);
				for (int x = hscroll - 2, x_end = hscroll + width + 2; x < x_end; x++)
				{
					line[x & 1023] = border_line;
				}
				line = fb + ((y_end + 1) & 1023) * pitch / sizeof(pixel_t);
				for (int x = hscroll - 2, x_end = hscroll + width + 2; x < x_end; x++)
				{
					line[x & 1023] = border_line;
				}
			}
		}
	}
}

static void sprite_debug_mode5(pixel_t *fb, uint32_t pitch, vdp_context *context)
{
	pixel_t bg_color = context->colors[context->regs[REG_BG_COLOR] & 0x3F];
	//clear a single alpha channel bit so we can distinguish between actual bg color and sprite
	//pixels that just happen to be the same color
	bg_color &= 0xFEFFFFFF;
	pixel_t *line = fb;
	pixel_t border_line = render_map_color(0, 0, 255);
	pixel_t sprite_outline = render_map_color(255, 0, 255);
	int right_border = 256 + ((context->h40_lines > context->output_lines / 2) ? 640 : 512);
	for (int y = 0; y < 1024; y++)
	{
		pixel_t *cur = line;
		if (y != 256 && y != 256+context->inactive_start*2) {
			for (int x = 0; x < 255; x++)
			{
				*(cur++) = bg_color;
			}
			*(cur++) = border_line;
			for (int x = 256; x < right_border; x++)
			{
				*(cur++) = bg_color;
			}
			*(cur++) = border_line;
			for (int x = right_border + 1; x < 1024; x++)
			{
				*(cur++) = bg_color;
			}
		} else {
			for (int x = 0; x < 1024; x++)
			{
				*(cur++) = border_line;
			}
		}
		line += pitch / sizeof(pixel_t);
	}
	for (int i = 0, index = 0; i < context->max_sprites_frame; i++)
	{
		uint32_t y = (context->sat_cache[index] & 3) << 8 | context->sat_cache[index + 1];
		if (!context->double_res) {
			y &= 0x1FF;
			y <<= 1;
		}
		uint8_t tile_width = ((context->sat_cache[index+2] >> 2) & 0x3);
		uint32_t pixel_width = (tile_width + 1) * 16;
		uint8_t height = ((context->sat_cache[index+2] & 3) + 1) * 16;
		uint16_t col_offset = height * (context->double_res ? 4 : 2);
		uint16_t att_addr = mode5_sat_address(context) + index * 2 + 4;
		uint16_t tileinfo = (context->vdpmem[att_addr] << 8) | context->vdpmem[att_addr+1];
		uint16_t tile_addr;
		if (context->double_res) {
			tile_addr = (tileinfo & 0x3FF) << 6;
		} else {
			tile_addr = (tileinfo & 0x7FF) << 5;
		}
		uint8_t pal = (tileinfo >> 9) & 0x30;
		uint16_t hflip = tileinfo & MAP_BIT_H_FLIP;
		uint16_t vflip = tileinfo & MAP_BIT_V_FLIP;
		uint32_t x = (((context->vdpmem[att_addr+ 2] & 0x3) << 8 | context->vdpmem[att_addr + 3]) & 0x1FF) * 2;
		pixel_t *line = fb + y * pitch / sizeof(pixel_t) + x;
		pixel_t *cur = line;
		for (uint32_t cx = x, x2 = x + pixel_width; cx < x2; cx++)
		{
			*(cur++) = sprite_outline;
		}
		uint8_t advance_source = 1;
		uint32_t y2 = y + height - 1;
		if (y2 > 1024) {
			y2 = 1024;
		}
		uint16_t line_offset = 4;
		if (vflip) {
			tile_addr += col_offset - 4;
			line_offset = -line_offset;
		}
		if (hflip) {
			tile_addr += col_offset * tile_width + 3;
			col_offset = -col_offset;
		}
		for (; y < y2; y++)
		{
			line += pitch / sizeof(pixel_t);
			cur = line;
			*(cur++) = sprite_outline;
			uint16_t line_addr = tile_addr;
			for (uint8_t tx = 0; tx <= tile_width; tx++)
			{
				uint16_t cur_addr = line_addr;
				for (uint8_t cx = 0; cx < 4; cx++)
				{
					uint8_t pair = context->vdpmem[cur_addr];
					pixel_t left, right;
					if (hflip) {
						right = pair >> 4;
						left = pair & 0xF;
						cur_addr--;
					} else {
						left = pair >> 4;
						right = pair & 0xF;
						cur_addr++;
					}
					left = left ? context->colors[pal | left] : bg_color;
					right = right ? context->colors[pal | right] : bg_color;
					if (*cur == bg_color) {
						*(cur) = left;
					}
					cur++;
					if (cx | tx) {
						if (*cur == bg_color) {
							*(cur) = left;
						}
						cur++;
					}
					if (*cur == bg_color) {
						*(cur) = right;
					}
					cur++;
					if (cx != 3 || tx != tile_width) {
						if (*cur == bg_color) {
							*(cur) = right;
						}
						cur++;
					}
				}
				line_addr += col_offset;
			}
			
			*(cur++) = sprite_outline;
			if (advance_source || context->double_res) {
				tile_addr += line_offset;
			}
			advance_source = !advance_source;
		}
		if (y2 != 1024) {
			line += pitch / sizeof(pixel_t);
			cur = line;
			for (uint32_t cx = x, x2 = x + pixel_width; cx < x2; cx++)
			{
				*(cur++) = sprite_outline;
			}
		}
		index = context->sat_cache[index+3] * 4;
		if (!index) {
			break;
		}
	}
}

static void plane_debug_mode4(pixel_t *fb, uint32_t pitch, vdp_context *context)
{
	pixel_t bg_color = context->colors[(context->regs[REG_BG_COLOR] & 0xF) + MODE4_OFFSET];
	uint32_t address = (context->regs[REG_SCROLL_A] & 0xE) << 10;
	for (uint32_t row_address = address, end = address + 32*32*2; row_address < end; row_address += 2 * 32)
	{
		pixel_t *col = fb;
		for(uint32_t cur = row_address, row_end = row_address + 2 * 32; cur < row_end; cur += 2)
		{
			uint32_t mapped = mode4_address_map[cur];
			uint16_t entry = context->vdpmem[mapped] << 8 | context->vdpmem[mapped + 1];
			uint32_t tile_address = (entry & 0x1FF) << 5;
			uint8_t pal = entry >> 7 & 0x10;
			uint32_t i_init, i_inc, i_limit, tile_inc;
			if (entry  & 0x200) {
				//hflip
				i_init = 0;
				i_inc = 4;
				i_limit = 32;
			} else {
				i_init = 28;
				i_inc = -4;
				i_limit = -4;
			}
			if (entry & 0x400) {
				//vflip
				tile_address += 7*4;
				tile_inc = -4;
			} else {
				tile_inc = 4;
			}
			pixel_t *line = col;
			for (int y = 0; y < 16; y++)
			{
				uint32_t first = mode4_address_map[tile_address];
				uint32_t last = mode4_address_map[tile_address + 2];
				uint32_t pixels = planar_to_chunky[context->vdpmem[first]] << 1;
				pixels |= planar_to_chunky[context->vdpmem[first+1]];
				pixels |= planar_to_chunky[context->vdpmem[last]] << 3;
				pixels |= planar_to_chunky[context->vdpmem[last+1]] << 2;
				pixel_t *out = line;
				for (uint32_t i = i_init; i != i_limit; i += i_inc)
				{
					pixel_t pixel = context->colors[((pixels >> i & 0xF) | pal) + MODE4_OFFSET];
					*(out++) = pixel;
					*(out++) = pixel;
				}
				
				
				if (y & 1) {
					tile_address += tile_inc;
				}
				line += pitch / sizeof(pixel_t);
			}
			
			
			
			col += 16;
		}
		fb += 16 * pitch / sizeof(pixel_t);
	}
}

static void sprite_debug_mode4(pixel_t *fb, uint32_t pitch, vdp_context *context)
{
}

pixel_t tms_map_color(vdp_context *context, uint8_t color)
{
	if (context->type == VDP_GAMEGEAR) {
		//Game Gear uses CRAM entries 16-31 for TMS9918A modes
		return context->colors[color + 16 + MODE4_OFFSET];
	} else {
		color <<= 1;
		color = (color & 0xE) | (color << 1 & 0x20);
		return context->color_map[color | FBUF_TMS];
	}
}

static void plane_debug_tms(pixel_t *fb, uint32_t pitch, vdp_context *context)
{
	uint16_t table_address = context->regs[REG_SCROLL_A] << 10 & 0x3C00;
	uint16_t color_address = context->regs[REG_COLOR_TABLE] << 6;
	uint16_t pattern_address = context->regs[REG_PATTERN_GEN] << 11 & 0x3800;
	uint16_t upper_vcounter_mask;
	uint16_t upper_vcounter_pmask;
	uint16_t pattern_name_mask;
	if (context->type > VDP_SMS2) {
		//SMS1 and TMS9918A
		upper_vcounter_mask = color_address & 0x1800;
		upper_vcounter_pmask = pattern_address & 0x1800;
		pattern_name_mask = (color_address & 0x07C0) | 0x0038;
	} else {
		//SMS2 and Game Gear
		upper_vcounter_mask = 0x1800;
		upper_vcounter_pmask = 0x1800;
		pattern_name_mask = 0x07F8;
	}
	uint32_t cols, pixels;
	if (context->regs[REG_MODE_2] & BIT_M1) {
		//Text mode
		cols = 40;
		pixels = 12;
	} else {
		//Graphics/Multicolor
		cols = 32;
		pixels = 16;
	}
	uint32_t fg, bg;
	if (context->regs[REG_MODE_2] & BIT_M1) {
		//Text mode uses TC and BD colors
		fg = tms_map_color(context, context->regs[REG_BG_COLOR] >> 4);
		bg = tms_map_color(context, context->regs[REG_BG_COLOR] & 0xF);
	}
	for (uint32_t row = 0; row < 24; row++)
	{
		pixel_t *colfb = fb;
		for (uint32_t col = 0; col < cols; col++)
		{
			pixel_t *linefb = colfb;
			uint8_t pattern = context->vdpmem[mode4_address_map[table_address] ^ 1];
			uint16_t caddress = color_address;
			uint16_t paddress = pattern_address;
			if (context->regs[REG_MODE_2] & BIT_M2) {
			} else {
				if (context->regs[REG_MODE_1] & BIT_M3) {
					//Graphics II
					caddress &= 0x2000;
					paddress &= 0x2000;
					caddress |= (row * 8) << 5 & upper_vcounter_mask;
					caddress |= pattern << 3 & pattern_name_mask;
					paddress |= (row * 8) << 5 & upper_vcounter_pmask;
				} else {
					caddress |= pattern >> 3;
				}
				paddress |= pattern << 3 & 0x7F8;
				for (uint32_t y = 0; y < 16; y++)
				{
					uint8_t bits = context->vdpmem[mode4_address_map[paddress] ^ 1];
					if (!(context->regs[REG_MODE_2] & BIT_M1)) {
						uint8_t colors = context->vdpmem[mode4_address_map[caddress] ^ 1];
						fg = tms_map_color(context, colors >> 4);
						bg = tms_map_color(context, colors & 0xF);
					}
					
					pixel_t *curfb = linefb;
					for (uint32_t x = 0; x < pixels; x++)
					{
						*(curfb++) = (bits & 0x80) ? fg : bg;
						if (x & 1) {
							bits <<= 1;
						}
					}
					linefb += pitch / sizeof(pixel_t);
					if (y & 1) {
						if (context->regs[REG_MODE_1] & BIT_M3) {
							caddress++;
						}
						paddress++;
					}
				}
			}
			table_address++;
			colfb += pixels;
		}
		fb += 16 * pitch / sizeof(pixel_t);
	}
}

static void sprite_debug_tms(pixel_t *fb, uint32_t pitch, vdp_context *context)
{
}

void vdp_update_per_frame_debug(vdp_context *context)
{
	if (context->enabled_debuggers & (1 << DEBUG_PLANE)) {
		
		uint32_t pitch;
		pixel_t *fb = render_get_framebuffer(context->debug_fb_indices[DEBUG_PLANE], &pitch);
		if (context->type == VDP_GENESIS && (context->regs[REG_MODE_2] & BIT_MODE_5)) {
			if ((context->debug_modes[DEBUG_PLANE] & 3) == 3) {
				sprite_debug_mode5(fb, pitch, context);
			} else {
				plane_debug_mode5(fb, pitch, context);
			}
		} else if (context->type != VDP_TMS9918A && (context->regs[REG_MODE_1] & BIT_MODE_4)) {
			if (context->debug_modes[DEBUG_PLANE] & 1) {
				sprite_debug_mode4(fb, pitch, context);
			} else {
				plane_debug_mode4(fb, pitch, context);
			}
		} else if (context->type != VDP_GENESIS) {
			if (context->debug_modes[DEBUG_PLANE] & 1) {
				sprite_debug_tms(fb, pitch, context);
			} else {
				plane_debug_tms(fb, pitch, context);
			}
		}
		render_framebuffer_updated(context->debug_fb_indices[DEBUG_PLANE], 1024);
	}

	if (context->enabled_debuggers & (1 << DEBUG_VRAM)) {
		uint32_t pitch;
		pixel_t *fb = render_get_framebuffer(context->debug_fb_indices[DEBUG_VRAM], &pitch);
		if (context->type == VDP_GENESIS && (context->regs[REG_MODE_2] & BIT_MODE_5)) {
			vram_debug_mode5(fb, pitch, context);
		} else if (context->type != VDP_TMS9918A && (context->regs[REG_MODE_1] & BIT_MODE_4)) {
			vram_debug_mode4(fb, pitch, context);
		} else if (context->type != VDP_GENESIS) {
			vram_debug_tms(fb, pitch, context);
		}
		render_framebuffer_updated(context->debug_fb_indices[DEBUG_VRAM], 1024);
	}

	if (context->enabled_debuggers & (1 << DEBUG_CRAM)) {
		uint32_t starting_line = 512 - 32*4;
		pixel_t *line = context->debug_fbs[DEBUG_CRAM]
			+ context->debug_fb_pitch[DEBUG_CRAM]  * starting_line / sizeof(pixel_t);
		pixel_t black = render_map_color(0, 0, 0);
		if (context->regs[REG_MODE_2] & BIT_MODE_5) {
			for (int pal = 0; pal < 4; pal ++)
			{
				pixel_t *cur;
				for (int y = 0; y < 31; y++)
				{
					cur = line;
					for (int offset = 0; offset < 16; offset++)
					{
						for (int x = 0; x < 31; x++)
						{
							*(cur++) = context->colors[pal * 16 + offset];
						}
						*(cur++) = black;
					}
					line += context->debug_fb_pitch[DEBUG_CRAM] / sizeof(pixel_t);
				}
				cur = line;
				for (int x = 0; x < 512; x++)
				{
					*(cur++) = black;
				}
				line += context->debug_fb_pitch[DEBUG_CRAM] / sizeof(pixel_t);
			}
		} else {
			for (int pal = 0; pal < 2; pal ++)
			{
				pixel_t *cur;
				for (int y = 0; y < 31; y++)
				{
					cur = line;
					for (int offset = MODE4_OFFSET; offset < MODE4_OFFSET + 16; offset++)
					{
						for (int x = 0; x < 31; x++)
						{
							*(cur++) = context->colors[pal * 16 + offset];
						}
						*(cur++) = black;
					}
					line += context->debug_fb_pitch[DEBUG_CRAM] / sizeof(pixel_t);
				}
				cur = line;
				for (int x = 0; x < 512; x++)
				{
					*(cur++) = black;
				}
				line += context->debug_fb_pitch[DEBUG_CRAM] / sizeof(pixel_t);
			}
		}
		render_framebuffer_updated(context->debug_fb_indices[DEBUG_CRAM], 512);
		context->debug_fbs[DEBUG_CRAM] = render_get_framebuffer(context->debug_fb_indices[DEBUG_CRAM], &context->debug_fb_pitch[DEBUG_CRAM]);
	}
	if (context->enabled_debuggers & (1 << DEBUG_COMPOSITE)) {
		render_framebuffer_updated(context->debug_fb_indices[DEBUG_COMPOSITE], LINEBUF_SIZE);
		context->debug_fbs[DEBUG_COMPOSITE] = render_get_framebuffer(context->debug_fb_indices[DEBUG_COMPOSITE], &context->debug_fb_pitch[DEBUG_COMPOSITE]);
	}
}

void vdp_force_update_framebuffer(vdp_context *context)
{
	if (!context->fb) {
		return;
	}
	uint16_t lines_max = context->inactive_start + context->border_bot + context->border_top;

	uint16_t to_fill = lines_max - context->output_lines;
	memset(
		((char *)context->fb) + context->output_pitch * context->output_lines,
		0,
		to_fill * context->output_pitch
	);
	render_framebuffer_updated(context->cur_buffer, context->h40_lines > context->output_lines / 2 ? LINEBUF_SIZE : (256+HORIZ_BORDER));
	context->fb = render_get_framebuffer(context->cur_buffer, &context->output_pitch);
	vdp_update_per_frame_debug(context);
}

static void advance_output_line(vdp_context *context)
{
	//This function is kind of gross because of the need to deal with vertical border busting via mode changes
	uint16_t lines_max = context->inactive_start + context->border_bot + context->border_top;
	uint32_t output_line = context->vcounter;
	if (!(context->regs[REG_MODE_2] & BIT_MODE_5)) {
		//vcounter increment occurs much later in Mode 4
		output_line++;
	}

	if (context->output_lines >= lines_max || (!context->pushed_frame && output_line == context->inactive_start + context->border_top)) {
		//we've either filled up a full frame or we're at the bottom of screen in the current defined mode + border crop
		if (!headless) {
			render_framebuffer_updated(context->cur_buffer, context->h40_lines > (context->inactive_start + context->border_top) / 2 ? LINEBUF_SIZE : (256+HORIZ_BORDER));
			uint8_t is_even = context->flags2 & FLAG2_EVEN_FIELD;
			if (context->vcounter <= context->inactive_start && (context->regs[REG_MODE_4] & BIT_INTERLACE)) {
				is_even = !is_even;
			}
			context->cur_buffer = is_even ? FRAMEBUFFER_EVEN : FRAMEBUFFER_ODD;
			context->pushed_frame = 1;
			context->fb = NULL;
		}
		vdp_update_per_frame_debug(context);
		/* P0: capture framebuffer before it gets reused */
		if (refdata_enabled) {
			refdata_capture_frame(context);
		}
		context->h40_lines = 0;
		context->frame++;
		/* DMA trace: frame-start marker for the new frame */
		if (refdata_enabled) {
			refdata_dma_frame_marker(context);
		}
		context->output_lines = 0;
	}

	if (output_line < context->inactive_start + context->border_bot) {
		if (context->output_lines) {
			output_line = context->output_lines++;//context->border_top + context->vcounter;
		} else if (!output_line && !context->border_top) {
			//top border is completely cropped so we won't hit the case below
			output_line = 0;
			context->output_lines = 1;
			context->pushed_frame = 0;
		} else if (!context->pushed_frame) {
			context->output_lines = output_line + 1;
		}
	} else if (output_line >= 0x200 - context->border_top) {
		if (output_line == 0x200 - context->border_top) {
			//We're at the top of the display, force context->output_lines to be zero to avoid
			//potential screen rolling if the mode is changed at an inopportune time
			context->output_lines = 0;
			context->pushed_frame = 0;
		}
		output_line = context->output_lines++;//context->vcounter - (0x200 - context->border_top);
	} else {
		context->output = NULL;
		return;
	}
	if (!context->fb) {
		context->fb = render_get_framebuffer(context->cur_buffer, &context->output_pitch);
	}
	output_line += context->top_offset;
	context->output = (pixel_t *)(((char *)context->fb) + context->output_pitch * output_line);
#ifdef DEBUG_FB_FILL
	for (int i = 0; i < LINEBUF_SIZE; i++)
	{
		context->output[i] = 0xFFFF00FF;
	}
#endif
	if (context->output && (context->regs[REG_MODE_4] & BIT_H40)) {
		context->h40_lines++;
	}
}

void vdp_release_framebuffer(vdp_context *context)
{
	if (context->fb) {
		render_framebuffer_updated(context->cur_buffer, context->h40_lines > (context->inactive_start + context->border_top) / 2 ? LINEBUF_SIZE : (256+HORIZ_BORDER));
		context->output = context->fb = NULL;
	}
}

void vdp_reacquire_framebuffer(vdp_context *context)
{
	uint16_t lines_max = context->inactive_start + context->border_bot + context->border_top;
	if (context->output_lines <= lines_max && context->output_lines > 0) {
		context->fb = render_get_framebuffer(context->cur_buffer, &context->output_pitch);
		context->output = (pixel_t *)(((char *)context->fb) + context->output_pitch * (context->output_lines - 1 + context->top_offset));
	} else {
		context->output = NULL;
	}
}

static void render_border_garbage(vdp_context *context, uint32_t address, uint8_t *buf, uint8_t buf_off, uint16_t col)
{
	uint8_t base = col >> 9 & 0x30;
	for (int i = 0; i < 4; i++, address++)
	{
		uint8_t byte = context->vdpmem[address & 0xFFFF];
		buf[(buf_off++) & SCROLL_BUFFER_MASK] = base | byte >> 4;
		buf[(buf_off++) & SCROLL_BUFFER_MASK] = base | byte & 0xF;
	}
}

static void draw_right_border(vdp_context *context)
{
	uint8_t *dst = context->compositebuf + BORDER_LEFT + ((context->regs[REG_MODE_4] & BIT_H40) ? 320 : 256);
	uint8_t pixel = context->regs[REG_BG_COLOR] & 0x3F;
	if ((context->test_regs[0] & TEST_BIT_DISABLE) != 0) {
		pixel = 0x3F;
	}
	uint8_t test_layer = context->test_regs[0] >> 7 & 3;
	if (test_layer) {
		switch(test_layer)
			{
			case 1:
				memset(dst, 0, BORDER_RIGHT);
				dst += BORDER_RIGHT;
				break;
			case 2: {
				//plane A
				//TODO: Deal with Window layer
				int i;
				i = 0;
				uint8_t buf_off = context->buf_a_off - context->hscroll_a_fine;
				//uint8_t *src = context->tmp_buf_a + ((context->buf_a_off + (i ? 0 : (16 - BORDER_LEFT) - (context->hscroll_a & 0xF))) & SCROLL_BUFFER_MASK);
				for (; i < BORDER_RIGHT; buf_off++, i++, dst++)
				{
					*dst = context->tmp_buf_a[buf_off & SCROLL_BUFFER_MASK] & 0x3F;
				}
				break;
			}
			case 3: {
				//plane B
				int i;
				i = 0;
				uint8_t buf_off = context->buf_b_off - (context->hscroll_b & 0xF);
				//uint8_t *src = context->tmp_buf_b + ((context->buf_b_off + (i ? 0 : (16 - BORDER_LEFT) - (context->hscroll_b & 0xF))) & SCROLL_BUFFER_MASK);
				for (; i < BORDER_RIGHT; buf_off++, i++, dst++)
				{
					*dst = context->tmp_buf_b[buf_off & SCROLL_BUFFER_MASK] & 0x3F;
				}
				break;
			}
			}
	} else {
		memset(dst, 0, BORDER_RIGHT);
		dst += BORDER_RIGHT;
	}
	context->done_composite = dst;
	context->buf_a_off = (context->buf_a_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_MASK;
	context->buf_b_off = (context->buf_b_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_MASK;
}

#define CHECK_ONLY if (context->cycles >= target_cycles) { return; }
#define CHECK_LIMIT if (context->flags & FLAG_DMA_RUN) { run_dma_src(context, -1); } context->hslot++; context->cycles += slot_cycles; CHECK_ONLY
#define OUTPUT_PIXEL(slot) if ((slot) >= BG_START_SLOT) {\
		uint8_t *src = context->compositebuf + ((slot) - BG_START_SLOT) *2;\
		pixel_t *dst = context->output + ((slot) - BG_START_SLOT) *2;\
		if ((*src & 0x3F) | test_layer) {\
			*(dst++) = context->colors[*(src++)];\
		} else {\
			*(dst++) = context->colors[(*(src++) & 0xC0) | bgindex];\
		}\
		if ((*src & 0x3F) | test_layer) {\
			*(dst++) = context->colors[*(src++)];\
		} else {\
			*(dst++) = context->colors[(*(src++) & 0xC0) | bgindex];\
		}\
	}

#define OUTPUT_PIXEL_H40(slot) if (slot <= (BG_START_SLOT + LINEBUF_SIZE/2)) {\
		uint8_t *src = context->compositebuf + (slot - BG_START_SLOT) *2;\
		pixel_t *dst = context->output + (slot - BG_START_SLOT) *2;\
		if ((*src & 0x3F) | test_layer) {\
			*(dst++) = context->colors[*(src++)];\
		} else {\
			*(dst++) = context->colors[(*(src++) & 0xC0) | bgindex];\
		}\
		if (slot == (BG_START_SLOT + LINEBUF_SIZE/2)) {\
			context->done_composite = NULL;\
		} else {\
			if ((*src & 0x3F) | test_layer) {\
				*(dst++) = context->colors[*(src++)];\
			} else {\
				*(dst++) = context->colors[(*(src++) & 0xC0) | bgindex];\
			}\
		}\
	}

#define OUTPUT_PIXEL_H32(slot) if (slot <= (BG_START_SLOT + (256+HORIZ_BORDER)/2)) {\
		uint8_t *src = context->compositebuf + (slot - BG_START_SLOT) *2;\
		pixel_t *dst = context->output + (slot - BG_START_SLOT) *2;\
		if ((*src & 0x3F) | test_layer) {\
			*(dst++) = context->colors[*(src++)];\
		} else {\
			*(dst++) = context->colors[(*(src++) & 0xC0) | bgindex];\
		}\
		if (slot == (BG_START_SLOT + (256+HORIZ_BORDER)/2)) {\
			context->done_composite = NULL;\
		} else {\
			if ((*src & 0x3F) | test_layer) {\
				*(dst++) = context->colors[*(src++)];\
			} else {\
				*(dst++) = context->colors[(*(src++) & 0xC0) | bgindex];\
			}\
		}\
	}

//BG_START_SLOT => dst = 0, src = border
//BG_START_SLOT + 13/2=6, dst = 6, src = border + comp + 13
#define OUTPUT_PIXEL_MODE4(slot) if ((slot) >= BG_START_SLOT) {\
		uint8_t *src = context->compositebuf + ((slot) - BG_START_SLOT) *2;\
		pixel_t *dst = context->output + ((slot) - BG_START_SLOT) *2;\
		if ((slot) - BG_START_SLOT < BORDER_LEFT/2) {\
			*(dst++) = context->colors[bgindex];\
			*(dst++) = context->colors[bgindex];\
		} else if ((slot) - BG_START_SLOT < (BORDER_LEFT+256)/2){\
			if ((slot) - BG_START_SLOT == BORDER_LEFT/2) {\
				*(dst++) = context->colors[bgindex];\
				src++;\
			} else {\
				*(dst++) = context->colors[*(src++)];\
			}\
			*(dst++) = context->colors[*(src++)];\
		} else if ((slot) - BG_START_SLOT <= (HORIZ_BORDER+256)/2) {\
			*(dst++) = context->colors[bgindex];\
			if ((slot) - BG_START_SLOT < (HORIZ_BORDER+256)/2) {\
				*(dst++) = context->colors[bgindex];\
			}\
		}\
	}

#define COLUMN_RENDER_BLOCK(column, startcyc) \
	case startcyc:\
		OUTPUT_PIXEL(startcyc)\
		read_map_scroll_a(column, context->vcounter, context);\
		CHECK_LIMIT\
	case ((startcyc+1)&0xFF):\
		OUTPUT_PIXEL((startcyc+1)&0xFF)\
		external_slot(context);\
		CHECK_LIMIT\
	case ((startcyc+2)&0xFF):\
		OUTPUT_PIXEL((startcyc+2)&0xFF)\
		render_map_1(context);\
		CHECK_LIMIT\
	case ((startcyc+3)&0xFF):\
		OUTPUT_PIXEL((startcyc+3)&0xFF)\
		render_map_2(context);\
		CHECK_LIMIT\
	case ((startcyc+4)&0xFF):\
		OUTPUT_PIXEL((startcyc+4)&0xFF)\
		read_map_scroll_b(column, context->vcounter, context);\
		CHECK_LIMIT\
	case ((startcyc+5)&0xFF):\
		OUTPUT_PIXEL((startcyc+5)&0xFF)\
		read_sprite_x(context->vcounter, context);\
		CHECK_LIMIT\
	case ((startcyc+6)&0xFF):\
		OUTPUT_PIXEL((startcyc+6)&0xFF)\
		render_map_3(context);\
		CHECK_LIMIT\
	case ((startcyc+7)&0xFF):\
		OUTPUT_PIXEL((startcyc+7)&0xFF)\
		render_map_output(context->vcounter, column, context);\
		CHECK_LIMIT

#define COLUMN_RENDER_BLOCK_REFRESH(column, startcyc) \
	case startcyc:\
		OUTPUT_PIXEL(startcyc)\
		read_map_scroll_a(column, context->vcounter, context);\
		CHECK_LIMIT\
	case (startcyc+1):\
		/* refresh, so don't run dma src */\
		OUTPUT_PIXEL((startcyc+1)&0xFF)\
		context->hslot++;\
		context->cycles += slot_cycles;\
		CHECK_ONLY\
	case (startcyc+2):\
		OUTPUT_PIXEL((startcyc+2)&0xFF)\
		render_map_1(context);\
		CHECK_LIMIT\
	case (startcyc+3):\
		OUTPUT_PIXEL((startcyc+3)&0xFF)\
		render_map_2(context);\
		CHECK_LIMIT\
	case (startcyc+4):\
		OUTPUT_PIXEL((startcyc+4)&0xFF)\
		read_map_scroll_b(column, context->vcounter, context);\
		CHECK_LIMIT\
	case (startcyc+5):\
		OUTPUT_PIXEL((startcyc+5)&0xFF)\
		read_sprite_x(context->vcounter, context);\
		CHECK_LIMIT\
	case (startcyc+6):\
		OUTPUT_PIXEL((startcyc+6)&0xFF)\
		render_map_3(context);\
		CHECK_LIMIT\
	case (startcyc+7):\
		OUTPUT_PIXEL((startcyc+7)&0xFF)\
		render_map_output(context->vcounter, column, context);\
		CHECK_LIMIT

#define COLUMN_RENDER_BLOCK_PHONY(column, startcyc) \
	case startcyc:\
		CHECK_LIMIT\
	case ((startcyc+1)&0xFF):\
		external_slot(context);\
		CHECK_LIMIT\
	case ((startcyc+2)&0xFF):\
		CHECK_LIMIT\
	case ((startcyc+3)&0xFF):\
		CHECK_LIMIT\
	case ((startcyc+4)&0xFF):\
		CHECK_LIMIT\
	case ((startcyc+5)&0xFF):\
		read_sprite_x(context->vcounter, context);\
		CHECK_LIMIT\
	case ((startcyc+6)&0xFF):\
		CHECK_LIMIT\
	case ((startcyc+7)&0xFF):\
		CHECK_LIMIT

#define COLUMN_RENDER_BLOCK_REFRESH_PHONY(column, startcyc) \
	case startcyc:\
		CHECK_LIMIT\
	case (startcyc+1):\
		/* refresh, so don't run dma src */\
		context->hslot++;\
		context->cycles += slot_cycles;\
		CHECK_ONLY\
	case (startcyc+2):\
		CHECK_LIMIT\
	case (startcyc+3):\
		CHECK_LIMIT\
	case (startcyc+4):\
		CHECK_LIMIT\
	case (startcyc+5):\
		read_sprite_x(context->vcounter, context);\
		CHECK_LIMIT\
	case (startcyc+6):\
		CHECK_LIMIT\
	case (startcyc+7):\
		CHECK_LIMIT

#define COLUMN_RENDER_BLOCK_MODE4(column, startcyc) \
	case startcyc:\
		OUTPUT_PIXEL_MODE4(startcyc)\
		read_map_mode4(column, context->vcounter, context);\
		CHECK_LIMIT\
	case ((startcyc+1)&0xFF):\
		OUTPUT_PIXEL_MODE4((startcyc+1)&0xFF)\
		if (column & 3) {\
			scan_sprite_table_mode4(context);\
		} else {\
			external_slot(context);\
		}\
		CHECK_LIMIT\
	case ((startcyc+2)&0xFF):\
		OUTPUT_PIXEL_MODE4((startcyc+2)&0xFF)\
		fetch_map_mode4(column, context->vcounter, context);\
		CHECK_LIMIT\
	case ((startcyc+3)&0xFF):\
		OUTPUT_PIXEL_MODE4((startcyc+3)&0xFF)\
		render_map_mode4(context->vcounter, column, context);\
		CHECK_LIMIT

#define CHECK_LIMIT_HSYNC(slot) \
	if (context->flags & FLAG_DMA_RUN) { run_dma_src(context, -1); } \
	if (slot >= HSYNC_SLOT_H40 && slot < HSYNC_END_H40) {\
		context->cycles += h40_hsync_cycles[slot - HSYNC_SLOT_H40];\
	} else {\
		context->cycles += slot_cycles;\
	}\
	if (slot == 182) {\
		context->hslot = 229;\
	} else {\
		context->hslot++;\
	}\
	CHECK_ONLY

#define SPRITE_RENDER_H40(slot) \
	case slot:\
		OUTPUT_PIXEL_H40(slot)\
		if ((slot) == BG_START_SLOT + LINEBUF_SIZE/2) {\
			advance_output_line(context);\
			if (!context->output) {\
				context->output = dummy_buffer;\
			}\
		}\
		render_sprite_cells( context);\
		if (slot == 168 || slot == 247 || slot == 248) {\
			render_border_garbage(\
				context,\
				context->serial_address,\
				context->tmp_buf_b,\
				context->buf_b_off + (slot == 247 ? 0 : 8),\
				slot == 247 ? context->col_1 : context->col_2\
			);\
			if (slot == 248) {\
				context->buf_a_off = (context->buf_a_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_MASK;\
				context->buf_b_off = (context->buf_b_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_MASK;\
			}\
		} else if (slot == 243) {\
			render_border_garbage(\
				context,\
				context->serial_address,\
				context->tmp_buf_a,\
				context->buf_a_off,\
				context->col_1\
			);\
		} else if (slot == 169) {\
			draw_right_border(context);\
		}\
		scan_sprite_table(context->vcounter, context);\
		CHECK_LIMIT_HSYNC(slot)

#define SPRITE_RENDER_H40_PHONY(slot) \
	case slot:\
		render_sprite_cells( context);\
		scan_sprite_table(context->vcounter, context);\
		CHECK_LIMIT_HSYNC(slot)

//Note that the line advancement check will fail if BG_START_SLOT is > 6
//as we're bumping up against the hcounter jump
#define SPRITE_RENDER_H32(slot) \
	case slot:\
		OUTPUT_PIXEL_H32(slot)\
		if ((slot) == BG_START_SLOT + (256+HORIZ_BORDER)/2) {\
			advance_output_line(context);\
			if (!context->output) {\
				context->output = dummy_buffer;\
			}\
		}\
		render_sprite_cells( context);\
		if (slot == 136 || slot == 247 || slot == 248) {\
			render_border_garbage(\
				context,\
				context->serial_address,\
				context->tmp_buf_b,\
				context->buf_b_off + (slot == 247 ? 0 : 8),\
				slot == 247 ? context->col_1 : context->col_2\
			);\
			if (slot == 248) {\
				context->buf_a_off = (context->buf_a_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_MASK;\
				context->buf_b_off = (context->buf_b_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_MASK;\
			}\
		} else if (slot == 137) {\
			draw_right_border(context);\
		}\
		scan_sprite_table(context->vcounter, context);\
		if (context->flags & FLAG_DMA_RUN) { run_dma_src(context, -1); } \
		if (slot == 147) {\
			context->hslot = 233;\
		} else {\
			context->hslot++;\
		}\
		context->cycles += slot_cycles;\
		CHECK_ONLY

#define SPRITE_RENDER_H32_PHONY(slot) \
	case slot:\
		render_sprite_cells( context);\
		scan_sprite_table(context->vcounter, context);\
		if (context->flags & FLAG_DMA_RUN) { run_dma_src(context, -1); } \
		if (slot == 147) {\
			context->hslot = 233;\
		} else {\
			context->hslot++;\
		}\
		context->cycles += slot_cycles;\
		CHECK_ONLY

#define MODE4_CHECK_SLOT_LINE(slot) \
		if (context->flags & FLAG_DMA_RUN) { run_dma_src(context, -1); } \
		if ((slot) == BG_START_SLOT + (256+HORIZ_BORDER)/2) {\
			advance_output_line(context);\
			if (!context->output) {\
				context->output = dummy_buffer;\
			}\
		}\
		if ((slot) == 147) {\
			context->hslot = 233;\
		} else {\
			context->hslot++;\
		}\
		context->cycles += slot_cycles;\
		if ((slot+1) == LINE_CHANGE_MODE4) {\
			vdp_advance_line(context);\
			if (context->vcounter == 192) {\
				return;\
			}\
		}\
		CHECK_ONLY

#define CALC_SLOT(slot, increment) ((slot+increment) > 147 && (slot+increment) < 233 ? (slot+increment-148+233): (slot+increment))

#define SPRITE_RENDER_H32_MODE4(slot) \
	case slot:\
		OUTPUT_PIXEL_MODE4(slot)\
		read_sprite_x_mode4(context);\
		MODE4_CHECK_SLOT_LINE(slot)\
	case CALC_SLOT(slot, 1):\
		OUTPUT_PIXEL_MODE4(CALC_SLOT(slot, 1))\
		read_sprite_x_mode4(context);\
		MODE4_CHECK_SLOT_LINE(CALC_SLOT(slot,1))\
	case CALC_SLOT(slot, 2):\
		OUTPUT_PIXEL_MODE4(CALC_SLOT(slot, 2))\
		fetch_sprite_cells_mode4(context);\
		MODE4_CHECK_SLOT_LINE(CALC_SLOT(slot, 2))\
	case CALC_SLOT(slot, 3):\
		OUTPUT_PIXEL_MODE4(CALC_SLOT(slot, 3))\
		render_sprite_cells_mode4(context);\
		MODE4_CHECK_SLOT_LINE(CALC_SLOT(slot, 3))\
	case CALC_SLOT(slot, 4):\
		OUTPUT_PIXEL_MODE4(CALC_SLOT(slot, 4))\
		fetch_sprite_cells_mode4(context);\
		MODE4_CHECK_SLOT_LINE(CALC_SLOT(slot, 4))\
	case CALC_SLOT(slot, 5):\
		OUTPUT_PIXEL_MODE4(CALC_SLOT(slot, 5))\
		render_sprite_cells_mode4(context);\
		MODE4_CHECK_SLOT_LINE(CALC_SLOT(slot, 5))

static pixel_t dummy_buffer[LINEBUF_SIZE];
static void vdp_h40_line(vdp_context * context)
{
	uint16_t address;
	uint32_t mask;
	uint32_t const slot_cycles = MCLKS_SLOT_H40;
	uint8_t bgindex = context->regs[REG_BG_COLOR] & 0x3F;
	uint8_t test_layer = context->test_regs[0] >> 7 & 3;

	//165
	if (!(context->regs[REG_MODE_3] & BIT_VSCROLL)) {
		//TODO: Develop some tests on hardware to see when vscroll latch actually happens for full plane mode
		//See note in vdp_h32 for why this was originally moved out of read_map_scroll
		//Skitchin' has a similar problem, but uses H40 mode. It seems to be able to hit the extern slot at 232
		//pretty consistently
		context->vscroll_latch[0] = context->vsram[0];
		context->vscroll_latch[1] = context->vsram[1];
	}
	render_sprite_cells(context);
	//166
	render_sprite_cells(context);
	//167
	context->sprite_index = 0x80;
	context->slot_counter = 0;
	render_sprite_cells(context);
	render_border_garbage(
		context,
		context->serial_address,
		context->tmp_buf_b, context->buf_b_off,
		context->col_1
	);
	scan_sprite_table(context->vcounter, context);
	//168
	render_sprite_cells(context);
	render_border_garbage(
		context,
		context->serial_address,
		context->tmp_buf_b,
		context->buf_b_off + 8,
		context->col_2
	);
	scan_sprite_table(context->vcounter, context);

	//Do palette lookup for end of previous line
	uint8_t *src = context->compositebuf + (LINE_CHANGE_H40 - BG_START_SLOT) *2;
	pixel_t *dst = context->output + (LINE_CHANGE_H40 - BG_START_SLOT) *2;
	if (context->output) {
		if (test_layer) {
			for (int i = 0; i < LINEBUF_SIZE - (LINE_CHANGE_H40 - BG_START_SLOT) * 2; i++)
			{
				*(dst++) = context->colors[*(src++)];
			}
		} else {
			for (int i = 0; i < LINEBUF_SIZE - (LINE_CHANGE_H40 - BG_START_SLOT) * 2; i++)
			{
				if (*src & 0x3F) {
					*(dst++) = context->colors[*(src++)];
				} else {
					*(dst++) = context->colors[(*(src++) & 0xC0) | bgindex];
				}
			}
		}
	}
	advance_output_line(context);
	//169-242 (inclusive)
	for (int i = 0; i < 27; i++)
	{
		render_sprite_cells(context);
		scan_sprite_table(context->vcounter, context);
	}
	//243
	render_sprite_cells(context);
	render_border_garbage(
		context,
		context->serial_address,
		context->tmp_buf_a,
		context->buf_a_off,
		context->col_1
	);
	scan_sprite_table(context->vcounter, context);
	//244
	address = (context->regs[REG_HSCROLL] & 0x3F) << 10;
	mask = 0;
	if (context->regs[REG_MODE_3] & 0x2) {
		mask |= 0xF8;
	}
	if (context->regs[REG_MODE_3] & 0x1) {
		mask |= 0x7;
	}
	render_border_garbage(context, address, context->tmp_buf_a, context->buf_a_off+8, context->col_2);
	address += (context->vcounter & mask) * 4;
	context->hscroll_a = context->vdpmem[address] << 8 | context->vdpmem[address+1];
	context->hscroll_a_fine = context->hscroll_a & 0xF;
	context->hscroll_b = context->vdpmem[address+2] << 8 | context->vdpmem[address+3];
	context->hscroll_b_fine = context->hscroll_b & 0xF;
	//printf("%d: HScroll A: %d, HScroll B: %d\n", context->vcounter, context->hscroll_a, context->hscroll_b);
	/* scroll trace: HS_LOAD (H40 vblank) */
	if (scroll_trace_enabled(context, context->vcounter)) {
		scroll_trace_log_line(context, context->vcounter,
			"HS_LOAD mode=%u base=0x%04X hA=%u hB=%u fineA=%u fineB=%u nextLine=%u",
			context->regs[REG_MODE_3] & 0x3,
			(uint16_t)((context->regs[REG_HSCROLL] & 0x3F) << 10),
			context->hscroll_a, context->hscroll_b,
			context->hscroll_a_fine, context->hscroll_b_fine,
			context->vcounter);
	}
	//243-246 inclusive
	for (int i = 0; i < 3; i++)
	{
		render_sprite_cells(context);
		scan_sprite_table(context->vcounter, context);
	}
	//247
	render_sprite_cells(context);
	render_border_garbage(
		context,
		context->serial_address,
		context->tmp_buf_b,
		context->buf_b_off,
		context->col_1
	);
	scan_sprite_table(context->vcounter, context);
	//248
	
	render_sprite_cells(context);
	render_border_garbage(
		context,
		context->serial_address,
		context->tmp_buf_b,
		context->buf_b_off + 8,
		context->col_2
	);
	scan_sprite_table(context->vcounter, context);
	context->buf_a_off = (context->buf_a_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_MASK;
	context->buf_b_off = (context->buf_b_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_MASK;
	//250
	render_sprite_cells(context);
	scan_sprite_table(context->vcounter, context);
	//251
	scan_sprite_table(context->vcounter, context);//Just a guess
	//252
	scan_sprite_table(context->vcounter, context);//Just a guess
	//254
	render_sprite_cells(context);
	scan_sprite_table(context->vcounter, context);
	//255
	scan_sprite_table(context->vcounter, context);
	//0
	scan_sprite_table(context->vcounter, context);//Just a guess
	//seems like the sprite table scan fills a shift register
	//values are FIFO, but unused slots precede used slots
	//so we set cur_slot to slot_counter and let it wrap around to
	//the beginning of the list
	context->cur_slot = context->slot_counter;
	context->sprite_x_offset = 0;
	context->sprite_draws = MAX_SPRITES_LINE;
	//background planes and layer compositing
	for (int col = 0; col < 42; col+=2)
	{
		read_map_scroll_a(col, context->vcounter, context);
		render_map_1(context);
		render_map_2(context);
		read_map_scroll_b(col, context->vcounter, context);
		render_map_3(context);
		render_map_output(context->vcounter, col, context);
	}
	//sprite rendering phase 2
	for (int i = 0; i < MAX_SPRITES_LINE; i++)
	{
		read_sprite_x(context->vcounter, context);
	}
	//163
	context->cur_slot = MAX_SPRITES_LINE-1;
	memset(context->linebuf, 0, LINEBUF_SIZE);
	context->flags &= ~FLAG_MASKED;
	while (context->sprite_draws) {
		context->sprite_draws--;
		context->sprite_draw_list[context->sprite_draws].x_pos = 0;
	}
	render_sprite_cells(context);
	render_border_garbage(
		context,
		context->serial_address,
		context->tmp_buf_a, context->buf_a_off,
		context->col_1
	);
	//164
	render_sprite_cells(context);
	render_border_garbage(
		context,
		context->serial_address,
		context->tmp_buf_a, context->buf_a_off + 8,
		context->col_2
	);
	context->cycles += MCLKS_LINE;
	vdp_advance_line(context);
	src = context->compositebuf;
	if (!context->output) {
		return;
	}
	dst = context->output;
	if (test_layer) {
		for (int i = 0; i < (LINE_CHANGE_H40 - BG_START_SLOT) * 2; i++)
		{
			*(dst++) = context->colors[*(src++)];
		}
	} else {
		for (int i = 0; i < (LINE_CHANGE_H40 - BG_START_SLOT) * 2; i++)
		{
			if (*src & 0x3F) {
				*(dst++) = context->colors[*(src++)];
			} else {
				*(dst++) = context->colors[(*(src++) & 0xC0) | bgindex];
			}
		}
	}
}
static void vdp_h40(vdp_context * context, uint32_t target_cycles)
{
	uint16_t address;
	uint32_t mask;
	uint32_t const slot_cycles = MCLKS_SLOT_H40;
	uint8_t bgindex = context->regs[REG_BG_COLOR] & 0x3F;
	uint8_t test_layer = context->test_regs[0] >> 7 & 3;
	if (!context->output) {
		//This shouldn't happen normally, but it can theoretically
		//happen when doing border busting
		context->output = dummy_buffer;
	}
	switch(context->hslot)
	{
	for (;;)
	{
	case 165:
		//only consider doing a line at a time if the FIFO is empty, there are no pending reads and there is no DMA running
		if (context->fifo_read == -1 && !(context->flags & FLAG_DMA_RUN) && ((context->cd & 1) || (context->flags & FLAG_READ_FETCHED))) {
			while (target_cycles - context->cycles >= MCLKS_LINE && context->state != PREPARING && context->vcounter != context->inactive_start) {
				vdp_h40_line(context);
			}
			CHECK_ONLY
			if (!context->output) {
				//This shouldn't happen normally, but it can theoretically
				//happen when doing border busting
				context->output = dummy_buffer;
			}
		}
		OUTPUT_PIXEL(165)
		if (!(context->regs[REG_MODE_3] & BIT_VSCROLL)) {
			//TODO: Develop some tests on hardware to see when vscroll latch actually happens for full plane mode
			//See note in vdp_h32 for why this was originally moved out of read_map_scroll
			//Skitchin' has a similar problem, but uses H40 mode. It seems to be able to hit the extern slot at 232
			//pretty consistently
			context->vscroll_latch[0] = context->vsram[0];
			context->vscroll_latch[1] = context->vsram[1];
		}
		if (context->state == PREPARING) {
			external_slot(context);
		} else {
			render_sprite_cells(context);
		}
		CHECK_LIMIT
	case 166:
		OUTPUT_PIXEL(166)
		if (context->state == PREPARING) {
			external_slot(context);
		} else {
			render_sprite_cells(context);
		}
		if (context->vcounter == context->inactive_start) {
			context->hslot++;
			context->cycles += slot_cycles;
			return;
		}
		CHECK_LIMIT
	//sprite attribute table scan starts
	case 167:
		OUTPUT_PIXEL(167)
		context->sprite_index = 0x80;
		context->slot_counter = 0;
		render_sprite_cells(context);
		render_border_garbage(
			context,
			context->serial_address,
			context->tmp_buf_b, context->buf_b_off,
			context->col_1
		);
		scan_sprite_table(context->vcounter, context);
		CHECK_LIMIT
	SPRITE_RENDER_H40(168)
	SPRITE_RENDER_H40(169)
	SPRITE_RENDER_H40(170)
	SPRITE_RENDER_H40(171)
	SPRITE_RENDER_H40(172)
	SPRITE_RENDER_H40(173)
	SPRITE_RENDER_H40(174)
	SPRITE_RENDER_H40(175)
	SPRITE_RENDER_H40(176)
	SPRITE_RENDER_H40(177)//End of border?
	SPRITE_RENDER_H40(178)
	SPRITE_RENDER_H40(179)
	SPRITE_RENDER_H40(180)
	SPRITE_RENDER_H40(181)
	SPRITE_RENDER_H40(182)
	SPRITE_RENDER_H40(229)
	//!HSYNC asserted
	SPRITE_RENDER_H40(230)
	SPRITE_RENDER_H40(231)
	case 232:
		external_slot(context);
		CHECK_LIMIT_HSYNC(232)
	SPRITE_RENDER_H40(233)
	SPRITE_RENDER_H40(234)
	SPRITE_RENDER_H40(235)
	SPRITE_RENDER_H40(236)
	SPRITE_RENDER_H40(237)
	SPRITE_RENDER_H40(238)
	SPRITE_RENDER_H40(239)
	SPRITE_RENDER_H40(240)
	SPRITE_RENDER_H40(241)
	SPRITE_RENDER_H40(242)
	SPRITE_RENDER_H40(243) //provides "garbage" for border when plane A selected
	case 244:
		address = (context->regs[REG_HSCROLL] & 0x3F) << 10;
		mask = 0;
		if (context->regs[REG_MODE_3] & 0x2) {
			mask |= 0xF8;
		}
		if (context->regs[REG_MODE_3] & 0x1) {
			mask |= 0x7;
		}
		render_border_garbage(context, address, context->tmp_buf_a, context->buf_a_off+8, context->col_2);
		address += (context->vcounter & mask) * 4;
		context->hscroll_a = context->vdpmem[address] << 8 | context->vdpmem[address+1];
		context->hscroll_a_fine = context->hscroll_a & 0xF;
		context->hscroll_b = context->vdpmem[address+2] << 8 | context->vdpmem[address+3];
		context->hscroll_b_fine = context->hscroll_b & 0xF;
		//printf("%d: HScroll A: %d, HScroll B: %d\n", context->vcounter, context->hscroll_a, context->hscroll_b);
		/* scroll trace: HS_LOAD (H40 active) */
		if (scroll_trace_enabled(context, context->vcounter)) {
			scroll_trace_log_line(context, context->vcounter,
				"HS_LOAD mode=%u base=0x%04X hA=%u hB=%u fineA=%u fineB=%u nextLine=%u",
				context->regs[REG_MODE_3] & 0x3,
				(uint16_t)((context->regs[REG_HSCROLL] & 0x3F) << 10),
				context->hscroll_a, context->hscroll_b,
				context->hscroll_a_fine, context->hscroll_b_fine,
				context->vcounter);
		}
		if (context->flags & FLAG_DMA_RUN) { run_dma_src(context, -1); }
		context->hslot++;
		context->cycles += h40_hsync_cycles[14];
		CHECK_ONLY //provides "garbage" for border when plane A selected
	//!HSYNC high
	SPRITE_RENDER_H40(245)
	SPRITE_RENDER_H40(246)
	SPRITE_RENDER_H40(247) //provides "garbage" for border when plane B selected
	SPRITE_RENDER_H40(248) //provides "garbage" for border when plane B selected
	case 249:
		read_map_scroll_a(0, context->vcounter, context);
		CHECK_LIMIT
	SPRITE_RENDER_H40(250)
	case 251:
		render_map_1(context);
		scan_sprite_table(context->vcounter, context);//Just a guess
		CHECK_LIMIT
	case 252:
		render_map_2(context);
		scan_sprite_table(context->vcounter, context);//Just a guess
		CHECK_LIMIT
	case 253:
		read_map_scroll_b(0, context->vcounter, context);
		CHECK_LIMIT
	SPRITE_RENDER_H40(254)
	case 255:
		render_map_3(context);
		scan_sprite_table(context->vcounter, context);//Just a guess
		CHECK_LIMIT
	case 0:
		render_map_output(context->vcounter, 0, context);
		scan_sprite_table(context->vcounter, context);//Just a guess
		//seems like the sprite table scan fills a shift register
		//values are FIFO, but unused slots precede used slots
		//so we set cur_slot to slot_counter and let it wrap around to
		//the beginning of the list
		context->cur_slot = context->slot_counter;
		context->sprite_x_offset = 0;
		context->sprite_draws = MAX_SPRITES_LINE;
		CHECK_LIMIT
	COLUMN_RENDER_BLOCK(2, 1)
	COLUMN_RENDER_BLOCK(4, 9)
	COLUMN_RENDER_BLOCK(6, 17)
	COLUMN_RENDER_BLOCK_REFRESH(8, 25)
	COLUMN_RENDER_BLOCK(10, 33)
	COLUMN_RENDER_BLOCK(12, 41)
	COLUMN_RENDER_BLOCK(14, 49)
	COLUMN_RENDER_BLOCK_REFRESH(16, 57)
	COLUMN_RENDER_BLOCK(18, 65)
	COLUMN_RENDER_BLOCK(20, 73)
	COLUMN_RENDER_BLOCK(22, 81)
	COLUMN_RENDER_BLOCK_REFRESH(24, 89)
	COLUMN_RENDER_BLOCK(26, 97)
	COLUMN_RENDER_BLOCK(28, 105)
	COLUMN_RENDER_BLOCK(30, 113)
	COLUMN_RENDER_BLOCK_REFRESH(32, 121)
	COLUMN_RENDER_BLOCK(34, 129)
	COLUMN_RENDER_BLOCK(36, 137)
	COLUMN_RENDER_BLOCK(38, 145)
	COLUMN_RENDER_BLOCK_REFRESH(40, 153)
	case 161:
		OUTPUT_PIXEL(161)
		external_slot(context);
		CHECK_LIMIT
	case 162:
		OUTPUT_PIXEL(162)
		external_slot(context);
		CHECK_LIMIT
	//sprite render to line buffer starts
	case 163:
		OUTPUT_PIXEL(163)
		context->cur_slot = MAX_SPRITES_LINE-1;
		memset(context->linebuf, 0, LINEBUF_SIZE);
		context->flags &= ~FLAG_MASKED;
		while (context->sprite_draws) {
			context->sprite_draws--;
			context->sprite_draw_list[context->sprite_draws].x_pos = 0;
		}
		render_sprite_cells(context);
		render_border_garbage(
			context,
			context->serial_address,
			context->tmp_buf_a, context->buf_a_off,
			context->col_1
		);
		CHECK_LIMIT
	case 164:
		OUTPUT_PIXEL(164)
		render_sprite_cells(context);
		render_border_garbage(
			context,
			context->serial_address,
			context->tmp_buf_a, context->buf_a_off + 8,
			context->col_2
		);
		if (context->flags & FLAG_DMA_RUN) {
			run_dma_src(context, -1);
		}
		context->hslot++;
		context->cycles += slot_cycles;
		vdp_advance_line(context);
		CHECK_ONLY
	}
	default:
		context->hslot++;
		context->cycles += slot_cycles;
		return;
	}
}

static void vdp_h40_phony(vdp_context * context, uint32_t target_cycles)
{
	uint32_t const slot_cycles = MCLKS_SLOT_H40;
	switch(context->hslot)
	{
	for (;;)
	{
	case 165:
		if (context->state == PREPARING) {
			external_slot(context);
		} else {
			render_sprite_cells(context);
		}
		CHECK_LIMIT
	case 166:
		if (context->state == PREPARING) {
			external_slot(context);
		} else {
			render_sprite_cells(context);
		}
		if (context->vcounter == context->inactive_start) {
			context->hslot++;
			context->cycles += slot_cycles;
			return;
		}
		CHECK_LIMIT
	//sprite attribute table scan starts
	case 167:
		context->sprite_index = 0x80;
		context->slot_counter = 0;
		render_sprite_cells(context);
		scan_sprite_table(context->vcounter, context);
		CHECK_LIMIT
	SPRITE_RENDER_H40_PHONY(168)
	SPRITE_RENDER_H40_PHONY(169)
	SPRITE_RENDER_H40_PHONY(170)
	SPRITE_RENDER_H40_PHONY(171)
	SPRITE_RENDER_H40_PHONY(172)
	SPRITE_RENDER_H40_PHONY(173)
	SPRITE_RENDER_H40_PHONY(174)
	SPRITE_RENDER_H40_PHONY(175)
	SPRITE_RENDER_H40_PHONY(176)
	SPRITE_RENDER_H40_PHONY(177)//End of border?
	SPRITE_RENDER_H40_PHONY(178)
	SPRITE_RENDER_H40_PHONY(179)
	SPRITE_RENDER_H40_PHONY(180)
	SPRITE_RENDER_H40_PHONY(181)
	SPRITE_RENDER_H40_PHONY(182)
	SPRITE_RENDER_H40_PHONY(229)
	//!HSYNC asserted
	SPRITE_RENDER_H40_PHONY(230)
	SPRITE_RENDER_H40_PHONY(231)
	case 232:
		external_slot(context);
		CHECK_LIMIT_HSYNC(232)
	SPRITE_RENDER_H40_PHONY(233)
	SPRITE_RENDER_H40_PHONY(234)
	SPRITE_RENDER_H40_PHONY(235)
	SPRITE_RENDER_H40_PHONY(236)
	SPRITE_RENDER_H40_PHONY(237)
	SPRITE_RENDER_H40_PHONY(238)
	SPRITE_RENDER_H40_PHONY(239)
	SPRITE_RENDER_H40_PHONY(240)
	SPRITE_RENDER_H40_PHONY(241)
	SPRITE_RENDER_H40_PHONY(242)
	SPRITE_RENDER_H40_PHONY(243) //provides "garbage" for border when plane A selected
	case 244:
		if (context->flags & FLAG_DMA_RUN) { run_dma_src(context, -1); }
		context->hslot++;
		context->cycles += h40_hsync_cycles[14];
		CHECK_ONLY //provides "garbage" for border when plane A selected
	//!HSYNC high
	SPRITE_RENDER_H40_PHONY(245)
	SPRITE_RENDER_H40_PHONY(246)
	SPRITE_RENDER_H40_PHONY(247) //provides "garbage" for border when plane B selected
	SPRITE_RENDER_H40_PHONY(248) //provides "garbage" for border when plane B selected
	case 249:
		CHECK_LIMIT
	SPRITE_RENDER_H40_PHONY(250)
	case 251:
		scan_sprite_table(context->vcounter, context);//Just a guess
		CHECK_LIMIT
	case 252:
		scan_sprite_table(context->vcounter, context);//Just a guess
		CHECK_LIMIT
	case 253:
		CHECK_LIMIT
	SPRITE_RENDER_H40_PHONY(254)
	case 255:
		scan_sprite_table(context->vcounter, context);//Just a guess
		CHECK_LIMIT
	case 0:
		scan_sprite_table(context->vcounter, context);//Just a guess
		//seems like the sprite table scan fills a shift register
		//values are FIFO, but unused slots precede used slots
		//so we set cur_slot to slot_counter and let it wrap around to
		//the beginning of the list
		context->cur_slot = context->slot_counter;
		context->sprite_x_offset = 0;
		context->sprite_draws = MAX_SPRITES_LINE;
		CHECK_LIMIT
	COLUMN_RENDER_BLOCK_PHONY(2, 1)
	COLUMN_RENDER_BLOCK_PHONY(4, 9)
	COLUMN_RENDER_BLOCK_PHONY(6, 17)
	COLUMN_RENDER_BLOCK_REFRESH_PHONY(8, 25)
	COLUMN_RENDER_BLOCK_PHONY(10, 33)
	COLUMN_RENDER_BLOCK_PHONY(12, 41)
	COLUMN_RENDER_BLOCK_PHONY(14, 49)
	COLUMN_RENDER_BLOCK_REFRESH_PHONY(16, 57)
	COLUMN_RENDER_BLOCK_PHONY(18, 65)
	COLUMN_RENDER_BLOCK_PHONY(20, 73)
	COLUMN_RENDER_BLOCK_PHONY(22, 81)
	COLUMN_RENDER_BLOCK_REFRESH_PHONY(24, 89)
	COLUMN_RENDER_BLOCK_PHONY(26, 97)
	COLUMN_RENDER_BLOCK_PHONY(28, 105)
	COLUMN_RENDER_BLOCK_PHONY(30, 113)
	COLUMN_RENDER_BLOCK_REFRESH_PHONY(32, 121)
	COLUMN_RENDER_BLOCK_PHONY(34, 129)
	COLUMN_RENDER_BLOCK_PHONY(36, 137)
	COLUMN_RENDER_BLOCK_PHONY(38, 145)
	COLUMN_RENDER_BLOCK_REFRESH_PHONY(40, 153)
	case 161:
		external_slot(context);
		CHECK_LIMIT
	case 162:
		external_slot(context);
		CHECK_LIMIT
	//sprite render to line buffer starts
	case 163:
		context->cur_slot = MAX_SPRITES_LINE-1;
		memset(context->linebuf, 0, LINEBUF_SIZE);
		context->flags &= ~FLAG_MASKED;
		while (context->sprite_draws) {
			context->sprite_draws--;
			context->sprite_draw_list[context->sprite_draws].x_pos = 0;
		}
		render_sprite_cells(context);
		CHECK_LIMIT
	case 164:
		render_sprite_cells(context);
		if (context->flags & FLAG_DMA_RUN) {
			run_dma_src(context, -1);
		}
		context->hslot++;
		context->cycles += slot_cycles;
		vdp_advance_line(context);
		CHECK_ONLY
	}
	default:
		context->hslot++;
		context->cycles += slot_cycles;
		return;
	}
}

static void vdp_h32(vdp_context * context, uint32_t target_cycles)
{
	uint16_t address;
	uint32_t mask;
	uint32_t const slot_cycles = MCLKS_SLOT_H32;
	uint8_t bgindex = context->regs[REG_BG_COLOR] & 0x3F;
	uint8_t test_layer = context->test_regs[0] >> 7 & 3;
	if (!context->output) {
		//This shouldn't happen normally, but it can theoretically
		//happen when doing border busting
		context->output = dummy_buffer;
	}
	switch(context->hslot)
	{
	for (;;)
	{
	case 133:
		OUTPUT_PIXEL(133)
		if (context->state == PREPARING) {
			external_slot(context);
		} else {
			render_sprite_cells(context);
		}
		CHECK_LIMIT
	case 134:
		OUTPUT_PIXEL(134)
		if (context->state == PREPARING) {
			external_slot(context);
		} else {
			render_sprite_cells(context);
		}
		if (context->vcounter == context->inactive_start) {
			context->hslot++;
			context->cycles += slot_cycles;
			return;
		}
		CHECK_LIMIT
	//sprite attribute table scan starts
	case 135:
		OUTPUT_PIXEL(135)
		context->sprite_index = 0x80;
		context->slot_counter = 0;
		render_sprite_cells(context);
		render_border_garbage(
			context,
			context->serial_address,
			context->tmp_buf_b, context->buf_b_off,
			context->col_1
		);
		scan_sprite_table(context->vcounter, context);
		CHECK_LIMIT
	SPRITE_RENDER_H32(136)
	SPRITE_RENDER_H32(137)
	SPRITE_RENDER_H32(138)
	SPRITE_RENDER_H32(139)
	SPRITE_RENDER_H32(140)
	SPRITE_RENDER_H32(141)
	SPRITE_RENDER_H32(142)
	SPRITE_RENDER_H32(143)
	SPRITE_RENDER_H32(144)
	case 145:
		OUTPUT_PIXEL(145)
		external_slot(context);
		CHECK_LIMIT
	SPRITE_RENDER_H32(146)
	SPRITE_RENDER_H32(147)
	SPRITE_RENDER_H32(233)
	SPRITE_RENDER_H32(234)
	SPRITE_RENDER_H32(235)
	//HSYNC start
	SPRITE_RENDER_H32(236)
	SPRITE_RENDER_H32(237)
	SPRITE_RENDER_H32(238)
	SPRITE_RENDER_H32(239)
	SPRITE_RENDER_H32(240)
	SPRITE_RENDER_H32(241)
	SPRITE_RENDER_H32(242)
	case 243:
		if (!(context->regs[REG_MODE_3] & BIT_VSCROLL)) {
			//TODO: Develop some tests on hardware to see when vscroll latch actually happens for full plane mode
			//Top Gear 2 has a very efficient HINT routine that can occassionally hit this slot with a VSRAM write
			//Since CRAM-updatnig HINT routines seem to indicate that my HINT latency is perhaps slightly too high
			//the most reasonable explanation is that vscroll is latched before this slot, but tests are needed
			//to confirm that one way or another
			context->vscroll_latch[0] = context->vsram[0];
			context->vscroll_latch[1] = context->vsram[1];
		}
		external_slot(context);
		//provides "garbage" for border when plane A selected
		render_border_garbage(
				context,
				context->serial_address,
				context->tmp_buf_a,
				context->buf_a_off,
				context->col_1
			);
		CHECK_LIMIT
	case 244:
		address = (context->regs[REG_HSCROLL] & 0x3F) << 10;
		mask = 0;
		if (context->regs[REG_MODE_3] & 0x2) {
			mask |= 0xF8;
		}
		if (context->regs[REG_MODE_3] & 0x1) {
			mask |= 0x7;
		}
		render_border_garbage(context, address, context->tmp_buf_a, context->buf_a_off+8, context->col_2);
		address += (context->vcounter & mask) * 4;
		context->hscroll_a = context->vdpmem[address] << 8 | context->vdpmem[address+1];
		context->hscroll_a_fine = context->hscroll_a & 0xF;
		context->hscroll_b = context->vdpmem[address+2] << 8 | context->vdpmem[address+3];
		context->hscroll_b_fine = context->hscroll_b & 0xF;
		//printf("%d: HScroll A: %d, HScroll B: %d\n", context->vcounter, context->hscroll_a, context->hscroll_b);
		/* scroll trace: HS_LOAD (H32 active) */
		if (scroll_trace_enabled(context, context->vcounter)) {
			scroll_trace_log_line(context, context->vcounter,
				"HS_LOAD mode=%u base=0x%04X hA=%u hB=%u fineA=%u fineB=%u nextLine=%u",
				context->regs[REG_MODE_3] & 0x3,
				(uint16_t)((context->regs[REG_HSCROLL] & 0x3F) << 10),
				context->hscroll_a, context->hscroll_b,
				context->hscroll_a_fine, context->hscroll_b_fine,
				context->vcounter);
		}
		CHECK_LIMIT //provides "garbage" for border when plane A selected
	SPRITE_RENDER_H32(245)
	SPRITE_RENDER_H32(246)
	SPRITE_RENDER_H32(247) //provides "garbage" for border when plane B selected
	SPRITE_RENDER_H32(248) //provides "garbage" for border when plane B selected
	//!HSYNC high
	case 249:
		read_map_scroll_a(0, context->vcounter, context);
		CHECK_LIMIT
	SPRITE_RENDER_H32(250)
	case 251:
		render_map_1(context);
		scan_sprite_table(context->vcounter, context);//Just a guess
		CHECK_LIMIT
	case 252:
		render_map_2(context);
		scan_sprite_table(context->vcounter, context);//Just a guess
		CHECK_LIMIT
	case 253:
		read_map_scroll_b(0, context->vcounter, context);
		CHECK_LIMIT
	case 254:
		render_sprite_cells(context);
		scan_sprite_table(context->vcounter, context);
		CHECK_LIMIT
	case 255:
		render_map_3(context);
		scan_sprite_table(context->vcounter, context);//Just a guess
		CHECK_LIMIT
	case 0:
		render_map_output(context->vcounter, 0, context);
		scan_sprite_table(context->vcounter, context);//Just a guess
		//reverse context slot counter so it counts the number of sprite slots
		//filled rather than the number of available slots
		//context->slot_counter = MAX_SPRITES_LINE - context->slot_counter;
		context->cur_slot = context->slot_counter;
		context->sprite_x_offset = 0;
		context->sprite_draws = MAX_SPRITES_LINE_H32;
		CHECK_LIMIT
	COLUMN_RENDER_BLOCK(2, 1)
	COLUMN_RENDER_BLOCK(4, 9)
	COLUMN_RENDER_BLOCK(6, 17)
	COLUMN_RENDER_BLOCK_REFRESH(8, 25)
	COLUMN_RENDER_BLOCK(10, 33)
	COLUMN_RENDER_BLOCK(12, 41)
	COLUMN_RENDER_BLOCK(14, 49)
	COLUMN_RENDER_BLOCK_REFRESH(16, 57)
	COLUMN_RENDER_BLOCK(18, 65)
	COLUMN_RENDER_BLOCK(20, 73)
	COLUMN_RENDER_BLOCK(22, 81)
	COLUMN_RENDER_BLOCK_REFRESH(24, 89)
	COLUMN_RENDER_BLOCK(26, 97)
	COLUMN_RENDER_BLOCK(28, 105)
	COLUMN_RENDER_BLOCK(30, 113)
	COLUMN_RENDER_BLOCK_REFRESH(32, 121)
	case 129:
		OUTPUT_PIXEL(129)
		external_slot(context);
		CHECK_LIMIT
	case 130: {
		OUTPUT_PIXEL(130)
		external_slot(context);
		CHECK_LIMIT
	}
	//sprite render to line buffer starts
	case 131:
		OUTPUT_PIXEL(131)
		context->cur_slot = MAX_SPRITES_LINE_H32-1;
		memset(context->linebuf, 0, LINEBUF_SIZE);
		context->flags &= ~FLAG_MASKED;
		while (context->sprite_draws) {
			context->sprite_draws--;
			context->sprite_draw_list[context->sprite_draws].x_pos = 0;
		}
		render_sprite_cells(context);
		render_border_garbage(
			context,
			context->serial_address,
			context->tmp_buf_a, context->buf_a_off,
			context->col_1
		);
		CHECK_LIMIT
	case 132:
		OUTPUT_PIXEL(132)
		render_sprite_cells(context);
		render_border_garbage(
			context,
			context->serial_address,
			context->tmp_buf_a, context->buf_a_off + 8,
			context->col_2
		);
		if (context->flags & FLAG_DMA_RUN) {
			run_dma_src(context, -1);
		}
		context->hslot++;
		context->cycles += slot_cycles;
		vdp_advance_line(context);
		CHECK_ONLY
	}
	default:
		context->hslot++;
		context->cycles += MCLKS_SLOT_H32;
	}
}

static void vdp_h32_phony(vdp_context * context, uint32_t target_cycles)
{
	uint32_t const slot_cycles = MCLKS_SLOT_H32;
	switch(context->hslot)
	{
	for (;;)
	{
	case 133:
		if (context->state == PREPARING) {
			external_slot(context);
		} else {
			render_sprite_cells(context);
		}
		CHECK_LIMIT
	case 134:
		if (context->state == PREPARING) {
			external_slot(context);
		} else {
			render_sprite_cells(context);
		}
		if (context->vcounter == context->inactive_start) {
			context->hslot++;
			context->cycles += slot_cycles;
			return;
		}
		CHECK_LIMIT
	//sprite attribute table scan starts
	case 135:
		context->sprite_index = 0x80;
		context->slot_counter = 0;
		render_sprite_cells(context);
		scan_sprite_table(context->vcounter, context);
		CHECK_LIMIT
	SPRITE_RENDER_H32_PHONY(136)
	SPRITE_RENDER_H32_PHONY(137)
	SPRITE_RENDER_H32_PHONY(138)
	SPRITE_RENDER_H32_PHONY(139)
	SPRITE_RENDER_H32_PHONY(140)
	SPRITE_RENDER_H32_PHONY(141)
	SPRITE_RENDER_H32_PHONY(142)
	SPRITE_RENDER_H32_PHONY(143)
	SPRITE_RENDER_H32_PHONY(144)
	case 145:
		external_slot(context);
		CHECK_LIMIT
	SPRITE_RENDER_H32_PHONY(146)
	SPRITE_RENDER_H32_PHONY(147)
	SPRITE_RENDER_H32_PHONY(233)
	SPRITE_RENDER_H32_PHONY(234)
	SPRITE_RENDER_H32_PHONY(235)
	//HSYNC start
	SPRITE_RENDER_H32_PHONY(236)
	SPRITE_RENDER_H32_PHONY(237)
	SPRITE_RENDER_H32_PHONY(238)
	SPRITE_RENDER_H32_PHONY(239)
	SPRITE_RENDER_H32_PHONY(240)
	SPRITE_RENDER_H32_PHONY(241)
	SPRITE_RENDER_H32_PHONY(242)
	case 243:
		external_slot(context);
		CHECK_LIMIT
	case 244:
		CHECK_LIMIT //provides "garbage" for border when plane A selected
	SPRITE_RENDER_H32_PHONY(245)
	SPRITE_RENDER_H32_PHONY(246)
	SPRITE_RENDER_H32_PHONY(247) //provides "garbage" for border when plane B selected
	SPRITE_RENDER_H32_PHONY(248) //provides "garbage" for border when plane B selected
	//!HSYNC high
	case 249:
		CHECK_LIMIT
	SPRITE_RENDER_H32_PHONY(250)
	case 251:
		scan_sprite_table(context->vcounter, context);//Just a guess
		CHECK_LIMIT
	case 252:
		scan_sprite_table(context->vcounter, context);//Just a guess
		CHECK_LIMIT
	case 253:
		CHECK_LIMIT
	case 254:
		render_sprite_cells(context);
		scan_sprite_table(context->vcounter, context);
		CHECK_LIMIT
	case 255:
		scan_sprite_table(context->vcounter, context);//Just a guess
		CHECK_LIMIT
	case 0:
		scan_sprite_table(context->vcounter, context);//Just a guess
		//reverse context slot counter so it counts the number of sprite slots
		//filled rather than the number of available slots
		//context->slot_counter = MAX_SPRITES_LINE - context->slot_counter;
		context->cur_slot = context->slot_counter;
		context->sprite_x_offset = 0;
		context->sprite_draws = MAX_SPRITES_LINE_H32;
		CHECK_LIMIT
	COLUMN_RENDER_BLOCK_PHONY(2, 1)
	COLUMN_RENDER_BLOCK_PHONY(4, 9)
	COLUMN_RENDER_BLOCK_PHONY(6, 17)
	COLUMN_RENDER_BLOCK_REFRESH_PHONY(8, 25)
	COLUMN_RENDER_BLOCK_PHONY(10, 33)
	COLUMN_RENDER_BLOCK_PHONY(12, 41)
	COLUMN_RENDER_BLOCK_PHONY(14, 49)
	COLUMN_RENDER_BLOCK_REFRESH_PHONY(16, 57)
	COLUMN_RENDER_BLOCK_PHONY(18, 65)
	COLUMN_RENDER_BLOCK_PHONY(20, 73)
	COLUMN_RENDER_BLOCK_PHONY(22, 81)
	COLUMN_RENDER_BLOCK_REFRESH_PHONY(24, 89)
	COLUMN_RENDER_BLOCK_PHONY(26, 97)
	COLUMN_RENDER_BLOCK_PHONY(28, 105)
	COLUMN_RENDER_BLOCK_PHONY(30, 113)
	COLUMN_RENDER_BLOCK_REFRESH_PHONY(32, 121)
	case 129:
		external_slot(context);
		CHECK_LIMIT
	case 130: {
		external_slot(context);
		CHECK_LIMIT
	}
	//sprite render to line buffer starts
	case 131:
		context->cur_slot = MAX_SPRITES_LINE_H32-1;
		context->flags &= ~FLAG_MASKED;
		while (context->sprite_draws) {
			context->sprite_draws--;
			context->sprite_draw_list[context->sprite_draws].x_pos = 0;
		}
		render_sprite_cells(context);
		CHECK_LIMIT
	case 132:
		render_sprite_cells(context);
		if (context->flags & FLAG_DMA_RUN) {
			run_dma_src(context, -1);
		}
		context->hslot++;
		context->cycles += slot_cycles;
		vdp_advance_line(context);
		CHECK_ONLY
	}
	default:
		context->hslot++;
		context->cycles += MCLKS_SLOT_H32;
	}
}

static void vdp_h32_mode4(vdp_context * context, uint32_t target_cycles)
{
	uint16_t address;
	uint32_t mask;
	uint32_t const slot_cycles = MCLKS_SLOT_H32;
	uint8_t bgindex = 0x10 | (context->regs[REG_BG_COLOR] & 0xF) + MODE4_OFFSET;
	uint8_t test_layer = context->test_regs[0] >> 7 & 3;
	if (!context->output) {
		//This shouldn't happen normally, but it can theoretically
		//happen when doing border busting
		context->output = dummy_buffer;
	}
	switch(context->hslot)
	{
	for (;;)
	{
	//sprite rendering starts
	SPRITE_RENDER_H32_MODE4(137)
	SPRITE_RENDER_H32_MODE4(143)
	case 234:
		external_slot(context);
		CHECK_LIMIT
	case 235:
		external_slot(context);
		CHECK_LIMIT
	//!HSYNC low
	case 236:
		external_slot(context);
		CHECK_LIMIT
	case 237:
		external_slot(context);
		CHECK_LIMIT
	case 238:
		external_slot(context);
		CHECK_LIMIT
	SPRITE_RENDER_H32_MODE4(239)
	SPRITE_RENDER_H32_MODE4(245)
	case 251:
		external_slot(context);
		CHECK_LIMIT
	case 252:
		external_slot(context);
		if (context->regs[REG_MODE_1] & BIT_HSCRL_LOCK && context->vcounter < 16) {
			context->hscroll_a = 0;
		} else {
			context->hscroll_a = context->regs[REG_X_SCROLL];
		}
		CHECK_LIMIT
	case 253:
		context->sprite_index = 0;
		context->slot_counter = MAX_DRAWS_H32_MODE4;
		scan_sprite_table_mode4(context);
		CHECK_LIMIT
	case 254:
		scan_sprite_table_mode4(context);
		CHECK_LIMIT
	case 255:
		scan_sprite_table_mode4(context);
		CHECK_LIMIT
	case 0: {
		scan_sprite_table_mode4(context);
		CHECK_LIMIT
	}
	case 1:
		scan_sprite_table_mode4(context);
		CHECK_LIMIT
	case 2:
		scan_sprite_table_mode4(context);
		CHECK_LIMIT
	case 3:
		scan_sprite_table_mode4(context);
		CHECK_LIMIT
	case 4: {
		scan_sprite_table_mode4(context);
		context->buf_a_off = 8;
		memset(context->tmp_buf_a, 0, 8);
		CHECK_LIMIT
	}
	COLUMN_RENDER_BLOCK_MODE4(0, 5)
	COLUMN_RENDER_BLOCK_MODE4(1, 9)
	COLUMN_RENDER_BLOCK_MODE4(2, 13)
	COLUMN_RENDER_BLOCK_MODE4(3, 17)
	COLUMN_RENDER_BLOCK_MODE4(4, 21)
	COLUMN_RENDER_BLOCK_MODE4(5, 25)
	COLUMN_RENDER_BLOCK_MODE4(6, 29)
	COLUMN_RENDER_BLOCK_MODE4(7, 33)
	COLUMN_RENDER_BLOCK_MODE4(8, 37)
	COLUMN_RENDER_BLOCK_MODE4(9, 41)
	COLUMN_RENDER_BLOCK_MODE4(10, 45)
	COLUMN_RENDER_BLOCK_MODE4(11, 49)
	COLUMN_RENDER_BLOCK_MODE4(12, 53)
	COLUMN_RENDER_BLOCK_MODE4(13, 57)
	COLUMN_RENDER_BLOCK_MODE4(14, 61)
	COLUMN_RENDER_BLOCK_MODE4(15, 65)
	COLUMN_RENDER_BLOCK_MODE4(16, 69)
	COLUMN_RENDER_BLOCK_MODE4(17, 73)
	COLUMN_RENDER_BLOCK_MODE4(18, 77)
	COLUMN_RENDER_BLOCK_MODE4(19, 81)
	COLUMN_RENDER_BLOCK_MODE4(20, 85)
	COLUMN_RENDER_BLOCK_MODE4(21, 89)
	COLUMN_RENDER_BLOCK_MODE4(22, 93)
	COLUMN_RENDER_BLOCK_MODE4(23, 97)
	COLUMN_RENDER_BLOCK_MODE4(24, 101)
	COLUMN_RENDER_BLOCK_MODE4(25, 105)
	COLUMN_RENDER_BLOCK_MODE4(26, 109)
	COLUMN_RENDER_BLOCK_MODE4(27, 113)
	COLUMN_RENDER_BLOCK_MODE4(28, 117)
	COLUMN_RENDER_BLOCK_MODE4(29, 121)
	COLUMN_RENDER_BLOCK_MODE4(30, 125)
	COLUMN_RENDER_BLOCK_MODE4(31, 129)
	case 133:
		OUTPUT_PIXEL_MODE4(133)
		external_slot(context);
		CHECK_LIMIT
	case 134:
		OUTPUT_PIXEL_MODE4(134)
		external_slot(context);
		CHECK_LIMIT
	case 135:
		OUTPUT_PIXEL_MODE4(135)
		external_slot(context);
		CHECK_LIMIT
	case 136: {
		OUTPUT_PIXEL_MODE4(136)
		external_slot(context);
		//set things up for sprite rendering in the next slot
		memset(context->linebuf, 0, LINEBUF_SIZE);
		context->cur_slot = context->sprite_index = MAX_DRAWS_H32_MODE4-1;
		context->sprite_draws = MAX_DRAWS_H32_MODE4;
		CHECK_LIMIT
	}}
	default:
		context->hslot++;
		context->cycles += MCLKS_SLOT_H32;
	}
}


static void tms_fetch_pattern_name(vdp_context *context)
{
	uint16_t address = context->regs[REG_SCROLL_A] << 10 & 0x3C00;
	if (context->regs[REG_MODE_2] & BIT_M1) {
		//Text mode
		address |= (context->vcounter >> 3) * 40;
		address += (context->hslot - 4) / 3;
	} else {
		//Graphics/Multicolor
		address |= context->vcounter << 2 & 0x03E0;
		address |= context->hslot >> 2;
	}
	//TODO: 4K/16K mode address remapping when emulating TMS9918A
	address = mode4_address_map[address] ^ 1;
	context->col_1 = context->vdpmem[address];
}

static void tms_fetch_color(vdp_context *context)
{
	if (context->regs[REG_MODE_2] & BIT_M2) {
		//Multicolor
		external_slot(context);
		return;
	}
	uint16_t address = context->regs[REG_COLOR_TABLE] << 6;
	if (context->regs[REG_MODE_1] & BIT_M3) {
		//Graphics II
		uint16_t upper_vcounter_mask;
		uint16_t pattern_name_mask;
		if (context->type > VDP_SMS2) {
			//SMS1 and TMS9918A
			upper_vcounter_mask = address & 0x1800;
			pattern_name_mask = (address & 0x07C0) | 0x0038;
		} else {
			//SMS2 and Game Gear
			upper_vcounter_mask = 0x1800;
			pattern_name_mask = 0x07F8;
		}
		address &= 0x2000;
		address |= context->vcounter << 5 & upper_vcounter_mask;
		address |= context->col_1 << 3 & pattern_name_mask;
		address |= context->vcounter & 7;
	} else {
		address |= context->col_1 >> 3;
	}
	//TODO: 4K/16K mode address remapping when emulating TMS9918A
	address = mode4_address_map[address] ^ 1;
	context->col_2 = context->vdpmem[address];
}

static void tms_fetch_pattern_value(vdp_context *context)
{
	uint16_t address = context->regs[REG_PATTERN_GEN] << 11 & 0x3800;
	if (context->regs[REG_MODE_1] & BIT_M3) {
		//Graphics II
		uint16_t mask = context->type > VDP_SMS2 ? address & 0x1800 : 0x1800;
		address &= 0x2000;
		address |= context->vcounter << 5 & mask;
	}
	address |= context->col_1 << 3 & 0x7F8;
	if (context->regs[REG_MODE_2] & BIT_M2) {
		//Multicolor
		address |= context->vcounter >> 2 & 0x3;
	} else {
		address |= context->vcounter & 0x7;
	}

	//TODO: 4K/16K mode address remapping when emulating TMS9918A
	address = mode4_address_map[address] ^ 1;
	uint8_t value = context->vdpmem[address];
	if (context->regs[REG_MODE_2] & BIT_M2) {
		//Multicolor
		context->tmp_buf_a[0] = 0xF0;
		context->tmp_buf_b[0] = value;
	} else {
		context->tmp_buf_a[0] = value;
		context->tmp_buf_b[0] = context->col_2;
	}
}

static void tms_sprite_scan(vdp_context *context)
{
	if (context->sprite_draws > 4 || context->sprite_index == 32) {
		return;
	}
	uint16_t address = context->regs[REG_SAT] << 7 & 0x3F80;
	address |= context->sprite_index << 2;
	address = mode4_address_map[address] ^ 1;
	uint8_t y = context->vdpmem[address];
	if (y == 208) {
		context->sprite_index = 32;
		context->sprite_info_list[4].index = context->sprite_index;
	}
	uint8_t diff = context->vcounter + 1 - y;
	uint8_t size = 8;
	if (context->regs[REG_MODE_2] & BIT_SPRITE_SZ) {
		size *= 2;
	}
	if (context->regs[REG_MODE_2] & BIT_SPRITE_ZM) {
		size *= 2;
	}
	if (diff < size) {
		context->sprite_info_list[context->sprite_draws++].index = context->sprite_index;
		if (context->sprite_draws == 5) {
			context->flags |= FLAG_SPRITE_OFLOW;
		}
	} else {
		context->sprite_info_list[4].index = context->sprite_index;
	}
	context->sprite_index++;
}

static void tms_sprite_vert(vdp_context *context)
{
	if (context->sprite_index >= 4 || context->sprite_index >= context->sprite_draws) {
		return;
	}
	uint16_t address = context->regs[REG_SAT] << 7 & 0x3F80;
	address |= context->sprite_info_list[context->sprite_index].index << 2;
	address = mode4_address_map[address] ^ 1;
	context->sprite_info_list[context->sprite_index].y = context->vdpmem[address];
}

static void tms_sprite_horiz(vdp_context *context)
{
	if (context->sprite_index >= 4 || context->sprite_index >= context->sprite_draws) {
		return;
	}
	uint16_t address = context->regs[REG_SAT] << 7 & 0x3F80;
	address |= context->sprite_info_list[context->sprite_index].index << 2 | 1;
	address = mode4_address_map[address] ^ 1;
	context->sprite_draw_list[context->sprite_index].x_pos = context->vdpmem[address];
}

static void tms_sprite_name(vdp_context *context)
{
	if (context->sprite_index >= 4 || context->sprite_index >= context->sprite_draws) {
		return;
	}
	uint16_t address = context->regs[REG_SAT] << 7 & 0x3F80;
	address |= context->sprite_info_list[context->sprite_index].index << 2 | 2;
	address = context->vdpmem[mode4_address_map[address] ^ 1] << 3;
	address |= context->regs[REG_STILE_BASE] << 11 & 0x3800;
	uint8_t diff = context->vcounter + 1 - context->sprite_info_list[context->sprite_index].y;
	if (context->regs[REG_MODE_2] & BIT_SPRITE_ZM) {
		diff >>= 1;
	}
	address += diff;
	context->sprite_draw_list[context->sprite_index].address = address;
}

static void tms_sprite_tag(vdp_context *context)
{
	if (context->sprite_index >= 4 || context->sprite_index >= context->sprite_draws) {
		return;
	}
	uint16_t address = context->regs[REG_SAT] << 7 & 0x3F80;
	address |= context->sprite_info_list[context->sprite_index].index << 2 | 3;
	address = mode4_address_map[address] ^ 1;
	uint8_t tag = context->vdpmem[address];
	if (tag & 0x80) {
		//early clock flag
		context->sprite_draw_list[context->sprite_index].x_pos -= 32;
	}
	context->sprite_draw_list[context->sprite_index].pal_priority = tag & 0xF;
	context->col_1 = 0;
}

static void tms_sprite_pattern1(vdp_context *context)
{
	if (context->sprite_index >= 4 || context->sprite_index >= context->sprite_draws) {
		return;
	}
	context->col_1 = context->vdpmem[mode4_address_map[context->sprite_draw_list[context->sprite_index].address] ^ 1] << 8;
	context->sprite_draw_list[context->sprite_index].address += 16;
}

static void tms_sprite_pattern2(vdp_context *context)
{
	if (context->sprite_index >= 4 || context->sprite_index >= context->sprite_draws) {
		return;
	}
	uint16_t pixels = context->col_1;
	if (context->regs[REG_MODE_2] & BIT_SPRITE_SZ) {
		pixels |= context->vdpmem[mode4_address_map[context->sprite_draw_list[context->sprite_index].address] ^ 1];
	}
	context->sprite_draw_list[context->sprite_index++].address = pixels;
}

static uint8_t tms_sprite_clock(vdp_context *context, int16_t offset)
{
	int16_t x = context->hslot << 1;
	if (x > 294) {
		x -= 512 + 8;
	} else {
		x -= 8;
	}
	x += offset;
	uint8_t output = 0;
	for (int i = 0; i < 4; i++) {
		if (x >= context->sprite_draw_list[i].x_pos) {
			if (context->sprite_draw_list[i].address & 0x8000) {
				if (output) {
					context->flags2 |= FLAG2_SPRITE_COLLIDE;
				} else {
					output = context->sprite_draw_list[i].pal_priority;
				}
			}
			if (!(context->regs[REG_MODE_2] & BIT_SPRITE_ZM) || ((x - context->sprite_draw_list[i].x_pos) & 1)) {
				context->sprite_draw_list[i].address <<= 1;
			}
		}
	}
	return output;
}

static void tms_border(vdp_context *context)
{
	if (context->hslot <  (256 + BORDER_LEFT - (BORDER_LEFT-8))/2 || context->hslot > 256-32) {
		tms_sprite_clock(context, 0);
		tms_sprite_clock(context, 1);
	}
	if (!context->output) {
		if ((context->hslot * 2 - 6 + BORDER_LEFT) == (256 + BORDER_LEFT + BORDER_RIGHT)) {
			advance_output_line(context);
		}
		if (!context->output) {
			return;
		}
	}
	pixel_t color;
	if (context->type == VDP_GAMEGEAR) {
		//Game Gear uses CRAM entries 16-31 for TMS9918A modes
		color = context->colors[(context->regs[REG_BG_COLOR] & 0xF) + 16 + MODE4_OFFSET];
	} else {
		color = context->regs[REG_BG_COLOR] << 1 & 0x1E;
		color = (color & 0xE) | (color << 1 & 0x20);
		color = context->color_map[color | FBUF_TMS];
	}
	if (context->hslot == (520 - BORDER_LEFT) / 2) {
		context->output[0] = color;
		return;
	}
	if (context->hslot < (256 + BORDER_LEFT + BORDER_RIGHT - (BORDER_LEFT - 8)) / 2) {
		context->output[context->hslot * 2 - 8 + BORDER_LEFT] = color;
		context->output[context->hslot * 2 - 7 + BORDER_LEFT] = color;
		if ((context->hslot * 2 - 6 + BORDER_LEFT) == (256 + BORDER_LEFT + BORDER_RIGHT)) {
			advance_output_line(context);
		}
	} else {
		int slot = (context->hslot - (520 - BORDER_LEFT) / 2) * 2 - 1;
		context->output[slot] = color;
		context->output[slot + 1] = color;
	}
}

static void tms_composite(vdp_context *context)
{
	if (context->state == PREPARING) {
		tms_border(context);
		return;
	}
	uint8_t color = tms_sprite_clock(context, 0);
	if (!context->output) {
		tms_sprite_clock(context, 1);
		return;
	}
	uint8_t fg,bg;
	if (context->regs[REG_MODE_2] & BIT_M1) {
		//Text mode uses TC and BD colors
		fg = context->regs[REG_BG_COLOR] >> 4;
		bg = context->regs[REG_BG_COLOR] & 0xF;
	} else {
		fg = context->tmp_buf_b[0] >> 4;
		bg = context->tmp_buf_b[0] & 0xF;
		if (!bg) {
			bg = context->regs[REG_BG_COLOR] & 0xF;
		}
	}
	uint8_t pattern = context->tmp_buf_a[0] & 0x80;
	context->tmp_buf_a[0] <<= 1;
	if (!color) {
		color = pattern ? fg : bg;
	}
	//TODO: composite debug output
	context->output[context->hslot * 2 - 8 + BORDER_LEFT] = tms_map_color(context, color);
	color = tms_sprite_clock(context, 1);
	pattern = context->tmp_buf_a[0] & 0x80;
	context->tmp_buf_a[0] <<= 1;
	if (!color) {
		color = pattern ? fg : bg;
	}
	//TODO: composite debug output
	context->output[context->hslot * 2 - 7 + BORDER_LEFT] = tms_map_color(context, color);
}

#define TMS_OUTPUT(slot) if ((slot) < 4 || (slot) > (256 + BORDER_LEFT - 8) / 2) { tms_border(context); } else { tms_composite(context); }
#define TMS_OUTPUT_RIGHT(slot) \
	if ((slot) < (256 + BORDER_LEFT - (BORDER_LEFT - 8))/2) {\
		tms_composite(context);\
	} else if ((slot < (256 + BORDER_LEFT + BORDER_RIGHT -(BORDER_LEFT - 8))/2)) {\
		tms_border(context);\
	}
#define TMS_CHECK_LIMIT context->hslot++; context->cycles += MCLKS_SLOT_H32; if (context->cycles >= target_cycles) { return; }
#define TMS_GRAPHICS_PATTERN_CPU_BLOCK(slot) \
	case slot:\
		TMS_OUTPUT(slot)\
		tms_fetch_pattern_name(context);\
		TMS_CHECK_LIMIT \
	case slot+1:\
		TMS_OUTPUT(slot+1)\
		external_slot(context);\
		TMS_CHECK_LIMIT \
	case slot+2:\
		TMS_OUTPUT(slot+2)\
		tms_fetch_color(context);\
		TMS_CHECK_LIMIT \
	case slot+3:\
		TMS_OUTPUT(slot+3)\
		tms_fetch_pattern_value(context);\
		TMS_CHECK_LIMIT

#define TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(slot) \
	case slot:\
		TMS_OUTPUT(slot)\
		tms_fetch_pattern_name(context);\
		TMS_CHECK_LIMIT \
	case slot+1:\
		TMS_OUTPUT(slot+1)\
		tms_sprite_scan(context);\
		TMS_CHECK_LIMIT \
	case slot+2:\
		TMS_OUTPUT(slot+2)\
		tms_fetch_color(context);\
		TMS_CHECK_LIMIT \
	case slot+3:\
		TMS_OUTPUT(slot+3)\
		tms_fetch_pattern_value(context);\
		TMS_CHECK_LIMIT

#define TMS_SPRITE_SCAN_SLOT(slot) \
	case slot:\
		if (context->hslot >= (520 - BORDER_LEFT) / 2) {\
			tms_border(context);\
		} else {\
			tms_sprite_clock(context, 0);\
			tms_sprite_clock(context, 1);\
		}\
		tms_sprite_scan(context);\
		TMS_CHECK_LIMIT

#define TMS_SPRITE_BLOCK(slot) \
	case slot:\
		TMS_OUTPUT_RIGHT(slot)\
		tms_sprite_vert(context);\
		TMS_CHECK_LIMIT \
	case slot+1:\
		TMS_OUTPUT_RIGHT(slot+1)\
		tms_sprite_horiz(context);\
		TMS_CHECK_LIMIT \
	case slot+2:\
		TMS_OUTPUT_RIGHT(slot+2)\
		tms_sprite_name(context);\
		TMS_CHECK_LIMIT \
	case slot+3:\
		TMS_OUTPUT_RIGHT(slot+3)\
		tms_sprite_tag(context);\
		TMS_CHECK_LIMIT \
	case slot+4:\
		TMS_OUTPUT_RIGHT(slot+4)\
		tms_sprite_pattern1(context);\
		TMS_CHECK_LIMIT \
	case slot+5:\
		TMS_OUTPUT_RIGHT(slot+5)\
		tms_sprite_pattern2(context);\
		TMS_CHECK_LIMIT

static void vdp_tms_graphics(vdp_context * context, uint32_t target_cycles)
{
	switch (context->hslot)
	{
	for (;;)
	{
	TMS_GRAPHICS_PATTERN_CPU_BLOCK(0)
	TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(4)
	TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(8)
	TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(12)
	TMS_GRAPHICS_PATTERN_CPU_BLOCK(16)
	TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(20)
	TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(24)
	TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(28)
	TMS_GRAPHICS_PATTERN_CPU_BLOCK(32)
	TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(36)
	TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(40)
	TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(44)
	TMS_GRAPHICS_PATTERN_CPU_BLOCK(48)
	TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(52)
	TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(56)
	TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(60)
	TMS_GRAPHICS_PATTERN_CPU_BLOCK(64)
	TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(68)
	TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(72)
	TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(76)
	TMS_GRAPHICS_PATTERN_CPU_BLOCK(80)
	TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(84)
	TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(88)
	TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(92)
	TMS_GRAPHICS_PATTERN_CPU_BLOCK(96)
	TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(100)
	TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(104)
	TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(108)
	TMS_GRAPHICS_PATTERN_CPU_BLOCK(112)
	TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(116)
	TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(120)
	TMS_GRAPHICS_PATTERN_SPRITE_BLOCK(124)
	case 128:
		tms_composite(context);
		external_slot(context);
		TMS_CHECK_LIMIT
	case 129:
		tms_composite(context);
		external_slot(context);
		context->sprite_index = 0;
		TMS_CHECK_LIMIT

	TMS_SPRITE_BLOCK(130)
	TMS_SPRITE_BLOCK(136)
	case 142:
		tms_sprite_vert(context);
		TMS_CHECK_LIMIT
	case 143:
		tms_sprite_horiz(context);
		TMS_CHECK_LIMIT
	case 145:
		external_slot(context);
		TMS_CHECK_LIMIT
	case 146:
		external_slot(context);
		TMS_CHECK_LIMIT
	case 147:
		external_slot(context);
		context->hslot = 233;
		context->cycles += MCLKS_SLOT_H32;
		if (context->cycles >= target_cycles) { return; }
	case 233:
		external_slot(context);
		TMS_CHECK_LIMIT
	case 234:
		tms_sprite_name(context);
		TMS_CHECK_LIMIT
	case 235:
		tms_sprite_tag(context);
		TMS_CHECK_LIMIT
	case 236:
		tms_sprite_pattern1(context);
		TMS_CHECK_LIMIT
	case 237:
		tms_sprite_pattern2(context);
		TMS_CHECK_LIMIT
	TMS_SPRITE_BLOCK(238)
	case 244:
		tms_sprite_clock(context, 0);
		tms_sprite_clock(context, 1);
		external_slot(context);
		TMS_CHECK_LIMIT
	case 245:
		tms_sprite_clock(context, 0);
		tms_sprite_clock(context, 1);
		external_slot(context);
		TMS_CHECK_LIMIT
	case 246:
		tms_sprite_clock(context, 0);
		tms_sprite_clock(context, 1);
		external_slot(context);
		TMS_CHECK_LIMIT
	case 247:
		tms_sprite_clock(context, 0);
		tms_sprite_clock(context, 1);
		external_slot(context);
		vdp_advance_line(context);
		context->sprite_index = context->sprite_draws = 0;
		if (context->vcounter == 192) {
			context->state = INACTIVE;
			return;
		}
		TMS_CHECK_LIMIT
	TMS_SPRITE_SCAN_SLOT(248)
	TMS_SPRITE_SCAN_SLOT(249)
	TMS_SPRITE_SCAN_SLOT(250)
	TMS_SPRITE_SCAN_SLOT(251)
	TMS_SPRITE_SCAN_SLOT(252)
	TMS_SPRITE_SCAN_SLOT(253)
	TMS_SPRITE_SCAN_SLOT(254)
	TMS_SPRITE_SCAN_SLOT(255)
	}
	default:
		context->hslot++;
		context->cycles += MCLKS_SLOT_H32;
	}
}

#define TMS_TEXT_OUTPUT(slot) if ((slot) < 8) { tms_border(context); } else { tms_composite(context); }
#define TMS_TEXT_BLOCK(slot) \
	case slot:\
		TMS_TEXT_OUTPUT(slot)\
		tms_fetch_pattern_name(context);\
		TMS_CHECK_LIMIT \
	case slot+1:\
		TMS_TEXT_OUTPUT(slot+1)\
		external_slot(context);\
		TMS_CHECK_LIMIT \
	case slot+2:\
		TMS_TEXT_OUTPUT(slot+2)\
		tms_fetch_pattern_value(context);\
		TMS_CHECK_LIMIT

static void vdp_tms_text(vdp_context * context, uint32_t target_cycles)
{
	switch (context->hslot)
	{
	for (;;)
	{
	case 0:
		tms_border(context);
		external_slot(context);
		TMS_CHECK_LIMIT
	case 1:
		tms_border(context);
		external_slot(context);
		TMS_CHECK_LIMIT
	case 2:
		tms_border(context);
		external_slot(context);
		TMS_CHECK_LIMIT
	case 3:
		tms_border(context);
		external_slot(context);
		TMS_CHECK_LIMIT
	case 4:
		tms_border(context);
		external_slot(context);
		TMS_CHECK_LIMIT
	TMS_TEXT_BLOCK(5)
	TMS_TEXT_BLOCK(8)
	TMS_TEXT_BLOCK(11)
	TMS_TEXT_BLOCK(14)
	TMS_TEXT_BLOCK(17)
	TMS_TEXT_BLOCK(20)
	TMS_TEXT_BLOCK(23)
	TMS_TEXT_BLOCK(26)
	TMS_TEXT_BLOCK(29)
	TMS_TEXT_BLOCK(32)
	TMS_TEXT_BLOCK(35)
	TMS_TEXT_BLOCK(38)
	TMS_TEXT_BLOCK(41)
	TMS_TEXT_BLOCK(44)
	TMS_TEXT_BLOCK(47)
	TMS_TEXT_BLOCK(50)
	TMS_TEXT_BLOCK(53)
	TMS_TEXT_BLOCK(56)
	TMS_TEXT_BLOCK(59)
	TMS_TEXT_BLOCK(62)
	TMS_TEXT_BLOCK(65)
	TMS_TEXT_BLOCK(68)
	TMS_TEXT_BLOCK(71)
	TMS_TEXT_BLOCK(74)
	TMS_TEXT_BLOCK(77)
	TMS_TEXT_BLOCK(80)
	TMS_TEXT_BLOCK(83)
	TMS_TEXT_BLOCK(86)
	TMS_TEXT_BLOCK(89)
	TMS_TEXT_BLOCK(92)
	TMS_TEXT_BLOCK(95)
	TMS_TEXT_BLOCK(98)
	TMS_TEXT_BLOCK(101)
	TMS_TEXT_BLOCK(104)
	TMS_TEXT_BLOCK(107)
	TMS_TEXT_BLOCK(110)
	TMS_TEXT_BLOCK(113)
	TMS_TEXT_BLOCK(116)
	TMS_TEXT_BLOCK(119)
	TMS_TEXT_BLOCK(122)
	case 125:
		tms_composite(context);
		external_slot(context);
		TMS_CHECK_LIMIT
	case 126:
		tms_composite(context);
		external_slot(context);
		TMS_CHECK_LIMIT
	case 127:
		tms_composite(context);
		external_slot(context);
		TMS_CHECK_LIMIT
	default:
		while (context->hslot < 139)
		{
			tms_border(context);
			external_slot(context);
			TMS_CHECK_LIMIT
		}
		while (context->hslot < 147)
		{
			external_slot(context);
			TMS_CHECK_LIMIT
		}
		if (context->hslot == 147) {
			external_slot(context);
			context->hslot = 233;
			context->cycles += MCLKS_SLOT_H32;
			if (context->cycles >= target_cycles) { return; }
		}
		while (context->hslot > 147) {
			if (context->hslot >= 233) {
				external_slot(context);
				if (context->hslot + 1 == LINE_CHANGE_MODE4) {
					vdp_advance_line(context);
					if (context->vcounter == 192) {
						context->state = INACTIVE;
						return;
					}
				}
			}
			TMS_CHECK_LIMIT
		}
	}
	}
}

static void inactive_test_output(vdp_context *context, uint8_t is_h40, uint8_t test_layer)
{
	uint8_t max_slot = is_h40 ? 169 : 136;
	if (context->hslot > max_slot) {
		return;
	}
	uint8_t *dst = context->compositebuf + (context->hslot >> 3) * SCROLL_BUFFER_DRAW;
	int32_t len;
	uint32_t src_off;
	if (context->hslot) {
		dst -= SCROLL_BUFFER_DRAW - BORDER_LEFT;
		src_off = 0;
		len = context->hslot == max_slot ? BORDER_RIGHT : SCROLL_BUFFER_DRAW;
	} else {
		src_off = SCROLL_BUFFER_DRAW - BORDER_LEFT;
		len = BORDER_LEFT;
	}
	uint8_t *src = NULL;
	if (test_layer == 2) {
		//plane A
		src_off += context->buf_a_off - (context->hscroll_a & 0xF);
		src = context->tmp_buf_a;
	} else if (test_layer == 3){
		//plane B
		src_off += context->buf_b_off - (context->hscroll_b & 0xF);
		src = context->tmp_buf_b;
	} else {
		//sprite layer
		memset(dst, 0, len);
		dst += len;
		len = 0;
	}
	if (src) {
		for (; len >=0; len--, dst++, src_off++)
		{
			*dst = src[src_off & SCROLL_BUFFER_MASK] & 0x3F;
		}
	}
	context->done_composite = dst;
	context->buf_a_off = (context->buf_a_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_DRAW;
	context->buf_b_off = (context->buf_b_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_DRAW;
}

static void check_switch_inactive(vdp_context *context, uint8_t is_h40)
{
	//technically the second hcounter check should be different for H40, but this is probably close enough for now
	if (context->state == ACTIVE && context->vcounter == context->inactive_start && (context->hslot >= (is_h40 ? 167 : 135) || context->hslot < 133)) {
		context->state = INACTIVE;
		context->cur_slot = MAX_SPRITES_LINE-1;
		context->sprite_x_offset = 0;
	}
}

static void vdp_inactive(vdp_context *context, uint32_t target_cycles, uint8_t is_h40, uint8_t mode_5)
{
	uint8_t buf_clear_slot, index_reset_slot, bg_end_slot, vint_slot, line_change, jump_start, jump_dest, latch_slot;
	uint8_t index_reset_value, max_draws, max_sprites;
	uint16_t vint_line, active_line;

	if (mode_5) {
		if (is_h40) {
			latch_slot = 165;
			buf_clear_slot = 163;
			index_reset_slot = 167;
			bg_end_slot = BG_START_SLOT + LINEBUF_SIZE/2;
			max_draws = MAX_SPRITES_LINE-1;
			max_sprites = MAX_SPRITES_LINE;
			index_reset_value = 0x80;
			vint_slot = VINT_SLOT_H40;
			line_change = LINE_CHANGE_H40;
			jump_start = 182;
			jump_dest = 229;
		} else {
			bg_end_slot = BG_START_SLOT + (256+HORIZ_BORDER)/2;
			max_draws = MAX_SPRITES_LINE_H32-1;
			max_sprites = MAX_SPRITES_LINE_H32;
			buf_clear_slot = 128;
			index_reset_slot = 132;
			index_reset_value = 0x80;
			vint_slot = VINT_SLOT_H32;
			line_change = LINE_CHANGE_H32;
			jump_start = 147;
			jump_dest = 233;
			latch_slot = 243;
		}
		vint_line = context->inactive_start;
		active_line = 0x1FF;
		if (context->regs[REG_MODE_3] & BIT_VSCROLL) {
			latch_slot = 220;
		}
	} else {
		latch_slot = 220;
		bg_end_slot = BG_START_SLOT + (256+HORIZ_BORDER)/2;
		max_draws = MAX_DRAWS_H32_MODE4;
		max_sprites = 8;
		buf_clear_slot = 136;
		index_reset_slot = 253;
		index_reset_value = 0;
		vint_line = context->inactive_start + 1;
		vint_slot = VINT_SLOT_MODE4;
		line_change = LINE_CHANGE_MODE4;
		jump_start = 147;
		jump_dest = 233;
		if ((context->regs[REG_MODE_1] & BIT_MODE_4) || context->type != VDP_GENESIS) {
			active_line = 0x1FF;
		} else {
			//never active unless either mode 4 or mode 5 is turned on
			active_line = 0x200;
		}
	}
	pixel_t *dst;
	uint8_t *debug_dst;
	if (context->output && context->hslot >= BG_START_SLOT && context->hslot <= bg_end_slot) {
		dst = context->output + 2 * (context->hslot - BG_START_SLOT);
		debug_dst = context->layer_debug_buf + 2 * (context->hslot - BG_START_SLOT);
	} else {
		dst = NULL;
	}

	uint8_t test_layer = context->test_regs[0] >> 7 & 3;

	while(context->cycles < target_cycles)
	{
		check_switch_inactive(context, is_h40);
		if (context->hslot == BG_START_SLOT && context->output) {
			dst = context->output + (context->hslot - BG_START_SLOT) * 2;
			debug_dst = context->layer_debug_buf + 2 * (context->hslot - BG_START_SLOT);
		}
		//this will need some tweaking to properly interact with 128K mode,
		//but this should be good enough for now
		context->serial_address += 1024;
		if (test_layer) {
			switch (context->hslot & 7)
			{
			case 3:
				render_border_garbage(context, context->serial_address, context->tmp_buf_a, context->buf_a_off, context->col_1);
				break;
			case 4:
				render_border_garbage(context, context->serial_address, context->tmp_buf_a, context->buf_a_off+8, context->col_2);
				break;
			case 7:
				render_border_garbage(context, context->serial_address, context->tmp_buf_b, context->buf_b_off, context->col_1);
				break;
			case 0:
				render_border_garbage(context, context->serial_address, context->tmp_buf_b, context->buf_b_off+8, context->col_2);
				break;
			case 1:
				inactive_test_output(context, is_h40, test_layer);
				break;
			}
		}

		if (context->hslot == buf_clear_slot) {
			if (mode_5) {
				context->cur_slot = max_draws;
			} else if ((context->regs[REG_MODE_1] & BIT_MODE_4) || context->type == VDP_GENESIS) {
				context->cur_slot = context->sprite_index = MAX_DRAWS_H32_MODE4-1;
				context->sprite_draws = MAX_DRAWS_H32_MODE4;
			} else {
				context->sprite_draws = 0;
			}
			memset(context->linebuf, 0, LINEBUF_SIZE);
		} else if (context->hslot == index_reset_slot) {
			context->sprite_index = index_reset_value;
			context->slot_counter = mode_5 ? 0 : max_sprites;
		} else if (context->hslot == latch_slot) {
			//it seems unlikely to me that vscroll actually gets latched when the display is off
			//but it's the only straightforward way to reconcile what I'm seeing between Skitchin
			//(which seems to expect vscroll to be latched early) and the intro of Gunstar Heroes
			//(which disables the display and ends up with garbage if vscroll is latched during that period)
			//without it. Some more tests are definitely needed
			context->vscroll_latch[0] = context->vsram[0];
			context->vscroll_latch[1] = context->vsram[1];
		} else if (context->vcounter == vint_line && context->hslot == vint_slot) {
			context->flags2 |= FLAG2_VINT_PENDING;
			context->pending_vint_start = context->cycles;
		} else if (context->vcounter == context->inactive_start && context->hslot == 1 && (context->regs[REG_MODE_4] & BIT_INTERLACE)) {
			context->flags2 ^= FLAG2_EVEN_FIELD;
		}

		if (dst) {
			uint8_t bg_index;
			pixel_t bg_color;
			if (mode_5) {
				bg_index = context->regs[REG_BG_COLOR] & 0x3F;
				bg_color = context->colors[bg_index];
			} else if ((context->regs[REG_MODE_1] & BIT_MODE_4) || context->type != VDP_GENESIS) {
				bg_index = 0x10 + (context->regs[REG_BG_COLOR] & 0xF);
				bg_color = context->colors[MODE4_OFFSET + bg_index];
			} else {
				bg_color = context->color_map[0];
			}
			if (context->done_composite) {
				uint8_t pixel = context->compositebuf[dst-context->output];
				if (!(pixel & 0x3F | test_layer)) {
					pixel = pixel & 0xC0 | bg_index;
				}
				*(dst++) = context->colors[pixel];
				if ((dst - context->output) == (context->done_composite - context->compositebuf)) {
					context->done_composite = NULL;
					memset(context->compositebuf, 0, sizeof(context->compositebuf));
				}
			} else {
				*(dst++) = bg_color;
				*(debug_dst++) = DBG_SRC_BG;
			}
			if (context->hslot != bg_end_slot) {
				if (context->done_composite) {
					uint8_t pixel = context->compositebuf[dst-context->output];
					if (!(pixel & 0x3F | test_layer)) {
						pixel = pixel & 0xC0 | bg_index;
					}
					*(dst++) = context->colors[pixel];
					if ((dst - context->output) == (context->done_composite - context->compositebuf)) {
						context->done_composite = NULL;
						memset(context->compositebuf, 0, sizeof(context->compositebuf));
					}
				} else {
					*(dst++) = bg_color;
					*(debug_dst++) = DBG_SRC_BG;
				}
			}
		}
		if (context->hslot == bg_end_slot) {
			advance_output_line(context);
			dst = NULL;
		}

		if (!is_refresh(context, context->hslot)) {
			external_slot(context);
			if (context->flags & FLAG_DMA_RUN && !is_refresh(context, context->hslot)) {
				run_dma_src(context, context->hslot);
			}
		}

		if (is_h40) {
			if (context->hslot >= HSYNC_SLOT_H40 && context->hslot < HSYNC_END_H40) {
				context->cycles += h40_hsync_cycles[context->hslot - HSYNC_SLOT_H40];
			} else {
				context->cycles += MCLKS_SLOT_H40;
			}
		} else {
			context->cycles += MCLKS_SLOT_H32;
		}
		if (context->hslot == jump_start) {
			context->hslot = jump_dest;
		} else {
			context->hslot++;
		}
		if (context->hslot == line_change) {
			vdp_advance_line(context);
			if (context->vcounter == active_line) {
				context->state = PREPARING;
				if (!context->done_composite) {
					memset(context->compositebuf, 0, sizeof(context->compositebuf));
				}
				return;
			}
		}
	}
}

static void vdp_inactive_phony(vdp_context *context, uint32_t target_cycles, uint8_t is_h40, uint8_t mode_5)
{
	uint8_t buf_clear_slot, index_reset_slot, bg_end_slot, vint_slot, line_change, jump_start, jump_dest, latch_slot;
	uint8_t index_reset_value, max_draws, max_sprites;
	uint16_t vint_line, active_line;

	if (mode_5) {
		if (is_h40) {
			latch_slot = 165;
			buf_clear_slot = 163;
			index_reset_slot = 167;
			bg_end_slot = BG_START_SLOT + LINEBUF_SIZE/2;
			max_draws = MAX_SPRITES_LINE-1;
			max_sprites = MAX_SPRITES_LINE;
			index_reset_value = 0x80;
			vint_slot = VINT_SLOT_H40;
			line_change = LINE_CHANGE_H40;
			jump_start = 182;
			jump_dest = 229;
		} else {
			bg_end_slot = BG_START_SLOT + (256+HORIZ_BORDER)/2;
			max_draws = MAX_SPRITES_LINE_H32-1;
			max_sprites = MAX_SPRITES_LINE_H32;
			buf_clear_slot = 128;
			index_reset_slot = 132;
			index_reset_value = 0x80;
			vint_slot = VINT_SLOT_H32;
			line_change = LINE_CHANGE_H32;
			jump_start = 147;
			jump_dest = 233;
			latch_slot = 243;
		}
		vint_line = context->inactive_start;
		active_line = 0x1FF;
		if (context->regs[REG_MODE_3] & BIT_VSCROLL) {
			latch_slot = 220;
		}
	} else {
		latch_slot = 220;
		bg_end_slot = BG_START_SLOT + (256+HORIZ_BORDER)/2;
		max_draws = MAX_DRAWS_H32_MODE4;
		max_sprites = 8;
		buf_clear_slot = 136;
		index_reset_slot = 253;
		index_reset_value = 0;
		vint_line = context->inactive_start + 1;
		vint_slot = VINT_SLOT_MODE4;
		line_change = LINE_CHANGE_MODE4;
		jump_start = 147;
		jump_dest = 233;
		if ((context->regs[REG_MODE_1] & BIT_MODE_4) || context->type != VDP_GENESIS) {
			active_line = 0x1FF;
		} else {
			//never active unless either mode 4 or mode 5 is turned on
			active_line = 0x200;
		}
	}

	while(context->cycles < target_cycles)
	{
		check_switch_inactive(context, is_h40);
		//this will need some tweaking to properly interact with 128K mode,
		//but this should be good enough for now
		context->serial_address += 1024;

		if (context->hslot == buf_clear_slot) {
			if (mode_5) {
				context->cur_slot = max_draws;
			} else if ((context->regs[REG_MODE_1] & BIT_MODE_4) || context->type == VDP_GENESIS) {
				context->cur_slot = context->sprite_index = MAX_DRAWS_H32_MODE4-1;
				context->sprite_draws = MAX_DRAWS_H32_MODE4;
			} else {
				context->sprite_draws = 0;
			}
			memset(context->linebuf, 0, LINEBUF_SIZE);
		} else if (context->hslot == index_reset_slot) {
			context->sprite_index = index_reset_value;
			context->slot_counter = mode_5 ? 0 : max_sprites;
		} else if (context->vcounter == vint_line && context->hslot == vint_slot) {
			context->flags2 |= FLAG2_VINT_PENDING;
			context->pending_vint_start = context->cycles;
		} else if (context->vcounter == context->inactive_start && context->hslot == 1 && (context->regs[REG_MODE_4] & BIT_INTERLACE)) {
			context->flags2 ^= FLAG2_EVEN_FIELD;
		}

		if (!is_refresh(context, context->hslot)) {
			external_slot(context);
			if (context->flags & FLAG_DMA_RUN && !is_refresh(context, context->hslot)) {
				run_dma_src(context, context->hslot);
			}
		}

		if (is_h40) {
			if (context->hslot >= HSYNC_SLOT_H40 && context->hslot < HSYNC_END_H40) {
				context->cycles += h40_hsync_cycles[context->hslot - HSYNC_SLOT_H40];
			} else {
				context->cycles += MCLKS_SLOT_H40;
			}
		} else {
			context->cycles += MCLKS_SLOT_H32;
		}
		if (context->hslot == jump_start) {
			context->hslot = jump_dest;
		} else {
			context->hslot++;
		}
		if (context->hslot == line_change) {
			vdp_advance_line(context);
			if (context->vcounter == active_line) {
				context->state = PREPARING;
				return;
			}
		}
	}
}

void vdp_run_context_full(vdp_context * context, uint32_t target_cycles)
{
	uint8_t is_h40 = context->regs[REG_MODE_4] & BIT_H40;
	uint8_t mode_5 = context->regs[REG_MODE_2] & BIT_MODE_5;
	if (context->renderer) {
		while(context->cycles < target_cycles)
		{
			check_switch_inactive(context, is_h40);

			if (is_active(context)) {
				if (mode_5) {
					if (is_h40) {
						vdp_h40_phony(context, target_cycles);
					} else {
						vdp_h32_phony(context, target_cycles);
					}
				} else if (context->regs[REG_MODE_1] & BIT_MODE_4) {
					//TODO: phonyfy this
					vdp_h32_mode4(context, target_cycles);
				} else if (context->regs[REG_MODE_2] & BIT_M1) {
						vdp_tms_text(context, target_cycles);
				} else {
					vdp_tms_graphics(context, target_cycles);
				}
			} else {
				vdp_inactive_phony(context, target_cycles, is_h40, mode_5);
			}
		}
	} else {
		while(context->cycles < target_cycles)
		{
			check_switch_inactive(context, is_h40);

			if (is_active(context)) {
				if (mode_5) {
					if (is_h40) {
						vdp_h40(context, target_cycles);
					} else {
						vdp_h32(context, target_cycles);
					}
				} else if (context->regs[REG_MODE_1] & BIT_MODE_4) {
					vdp_h32_mode4(context, target_cycles);
				} else if (context->regs[REG_MODE_2] & BIT_M1) {
						vdp_tms_text(context, target_cycles);
				} else {
					vdp_tms_graphics(context, target_cycles);
				}
			} else {
				vdp_inactive(context, target_cycles, is_h40, mode_5);
			}
		}
	}
}

void vdp_run_context(vdp_context *context, uint32_t target_cycles)
{
	//TODO: Deal with H40 hsync shenanigans
	uint32_t slot_cyc = context->regs[REG_MODE_4] & BIT_H40 ? 15 : 19;
	if (target_cycles < slot_cyc) {
		//avoid overflow
		return;
	}
	vdp_run_context_full(context, target_cycles - slot_cyc);
}

uint32_t vdp_run_to_vblank(vdp_context * context)
{
	uint32_t old_frame = context->frame;
	while (context->frame == old_frame) {
		vdp_run_context_full(context, context->cycles + MCLKS_LINE);
	}
	return context->cycles;
}

void vdp_run_dma_done(vdp_context * context, uint32_t target_cycles)
{
	for(;;) {
		uint32_t dmalen = (context->regs[REG_DMALEN_H] << 8) | context->regs[REG_DMALEN_L];
		if (!dmalen) {
			dmalen = 0x10000;
		}
		uint32_t min_dma_complete = dmalen * (context->regs[REG_MODE_4] & BIT_H40 ? 16 : 20);
		if (
			(context->regs[REG_DMASRC_H] & DMA_TYPE_MASK) == DMA_COPY
			|| (((context->cd & 0xF) == VRAM_WRITE) && !(context->regs[REG_MODE_2] & BIT_128K_VRAM))) {
			//DMA copies take twice as long to complete since they require a read and a write
			//DMA Fills and transfers to VRAM also take twice as long as it requires 2 writes for a single word
			//unless 128KB mode is enabled
			min_dma_complete *= 2;
		}
		min_dma_complete += context->cycles;
		if (target_cycles < min_dma_complete) {
			vdp_run_context_full(context, target_cycles);
			return;
		} else {
			vdp_run_context_full(context, min_dma_complete);
			if (!(context->flags & FLAG_DMA_RUN)) {
				return;
			}
		}
	}
}

static uint16_t get_ext_vcounter(vdp_context *context)
{
	uint16_t line= context->vcounter;
	if (context->regs[REG_MODE_4] & BIT_INTERLACE) {
		if (context->double_res) {
			line <<= 1;
		} else {
			line &= 0x1FE;
		}
		if (line & 0x100) {
			line |= 1;
		}
	}
	return line << 8;
}

void vdp_latch_hv(vdp_context *context)
{
	context->hv_latch = context->hslot | get_ext_vcounter(context);
}

uint16_t vdp_hv_counter_read(vdp_context * context)
{
	if ((context->regs[REG_MODE_2] & BIT_MODE_5) && (context->regs[REG_MODE_1] & BIT_HVC_LATCH)) {
		return context->hv_latch;
	}
	uint16_t hv;
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		hv = context->hslot;
	} else {
		hv = context->hv_latch & 0xFF;
	}
	hv |= get_ext_vcounter(context);

	return hv;
}

void vdp_reg_write(vdp_context *context, uint16_t reg, uint16_t value)
{
	uint8_t mode_5 = context->regs[REG_MODE_2] & BIT_MODE_5;
	if (reg < (mode_5 ? VDP_REGS : 0xB)) {
		//printf("register %d set to %X\n", reg, value & 0xFF);
		if (reg == REG_MODE_1 && (value & BIT_HVC_LATCH) && !(context->regs[reg] & BIT_HVC_LATCH)) {
			vdp_latch_hv(context);
		} else if (reg == REG_BG_COLOR) {
			value &= 0x3F;
		} else if (reg == REG_MODE_2 && context->type != VDP_GENESIS) {
			// only the Genesis VDP does anything with this bit
			// so just clear it to prevent Mode 5 selection if we're not emulating that chip
			value &= ~BIT_MODE_5;
		}
		/*if (reg == REG_MODE_4 && ((value ^ context->regs[reg]) & BIT_H40)) {
			printf("Mode changed from H%d to H%d @ %d, frame: %d\n", context->regs[reg] & BIT_H40 ? 40 : 32, value & BIT_H40 ? 40 : 32, context->cycles, context->frame);
		}*/
		uint8_t buffer[2] = {reg, value};
		event_log(EVENT_VDP_REG, context->cycles, sizeof(buffer), buffer);
		context->regs[reg] = value;
		if (reg == REG_MODE_1 || reg == REG_MODE_2 || reg == REG_MODE_4) {
			update_video_params(context);
		}
	} else if (context->type == VDP_GENESIS) {
		// Apparently Bart vs. the Space Mutants for SMS/GG writes to the timer KMOD timer register
		// Probably need to add some sort of config toggle for KMOD registers generally, but this is a quick fix
		if (reg == REG_KMOD_CTRL) {
			if (!(value & 0xFF)) {
				context->system->enter_debugger = 1;
			}
		} else if (reg == REG_KMOD_MSG) {
			char c = value;
			if (c) {
				context->kmod_buffer_length++;
				if ((context->kmod_buffer_length + 1) > context->kmod_buffer_storage) {
					context->kmod_buffer_storage = context->kmod_buffer_storage ? context->kmod_buffer_storage * 2 : 128;
					context->kmod_msg_buffer = realloc(context->kmod_msg_buffer, context->kmod_buffer_storage);
				}
				context->kmod_msg_buffer[context->kmod_buffer_length - 1] = c;
			} else if (context->kmod_buffer_length) {
				context->kmod_msg_buffer[context->kmod_buffer_length] = 0;
				if (is_stdout_enabled()) {
					init_terminal();
					printf("KDEBUG MESSAGE: %s\n", context->kmod_msg_buffer);
				} else {
					// GDB remote debugging is enabled, use stderr instead
					fprintf(stderr, "KDEBUG MESSAGE: %s\n", context->kmod_msg_buffer);
				}
				context->kmod_buffer_length = 0;
			}
		} else if (reg == REG_KMOD_TIMER) {
			if (!(value & 0x80)) {
				if (is_stdout_enabled()) {
					init_terminal();
					printf("KDEBUG TIMER: %d\n", (context->cycles - context->timer_start_cycle) / 7);
				} else {
					// GDB remote debugging is enabled, use stderr instead
					fprintf(stderr, "KDEBUG TIMER: %d\n", (context->cycles - context->timer_start_cycle) / 7);
				}
			}
			if (value & 0xC0) {
				context->timer_start_cycle = context->cycles;
			}
		}
	}
}

int vdp_control_port_write(vdp_context * context, uint16_t value, uint32_t cpu_cycle)
{
	//printf("control port write: %X at %d\n", value, context->cycles);
	if (context->flags & FLAG_PENDING) {
		context->address_latch = value << 14 & 0x1C000;
		context->address = (context->address & 0x3FFF) | context->address_latch;
		//It seems like the DMA enable bit doesn't so much enable DMA so much
		//as it enables changing CD5 from control port writes
		if (context->regs[REG_MODE_2] & BIT_DMA_ENABLE) {
			context->cd = (context->cd & 0x3) | ((value >> 2) & ~0x3 & 0xFF);
		} else {
			context->cd = (context->cd & 0x23) | ((value >> 2) & ~0x23 & 0xFF);
		}
		context->flags &= ~FLAG_PENDING;
		//Should these be taken care of here or after the first write?
		context->flags &= ~FLAG_READ_FETCHED;
		context->flags2 &= ~FLAG2_READ_PENDING;
		if (!(context->cd & 1)) {
			context->read_latency = cpu_cycle + ((context->regs[REG_MODE_4] & BIT_H40) ? 16 : 20)*READ_LATENCY;
		}
		//printf("New Address: %X, New CD: %X\n", context->address, context->cd);
		if (context->cd & 0x20) {
			//
			if((context->regs[REG_DMASRC_H] & DMA_TYPE_MASK) != DMA_FILL) {
				//DMA copy or 68K -> VDP, transfer starts immediately
				//printf("DMA start (length: %X) at cycle %d, frame: %d, vcounter: %d, hslot: %d\n", (context->regs[REG_DMALEN_H] << 8) | context->regs[REG_DMALEN_L], context->cycles, context->frame, context->vcounter, context->hslot);
				if (!(context->regs[REG_DMASRC_H] & 0x80)) {
					//printf("DMA Address: %X, New CD: %X, Source: %X, Length: %X\n", context->address, context->cd, (context->regs[REG_DMASRC_H] << 17) | (context->regs[REG_DMASRC_M] << 9) | (context->regs[REG_DMASRC_L] << 1), context->regs[REG_DMALEN_H] << 8 | context->regs[REG_DMALEN_L]);
					//68K -> VDP DMA takes a few slots to actually start reading even though it acquires the bus immediately
					//logic analyzer captures made it seem like the proper value is 4 slots, but that seems to cause trouble with the Nemesis' FIFO Wait State test
					//only captures are from a direct color DMA demo which will generally start DMA at a very specific point in display so other values are plausible
					//sticking with 3 slots for now until I can do some more captures
					vdp_run_context_full(context, context->cycles + 12 * ((context->regs[REG_MODE_2] & BIT_MODE_5) && (context->regs[REG_MODE_4] & BIT_H40) ? 4 : 5));
					vdp_dma_started();
					context->flags |= FLAG_DMA_RUN;
					if (refdata_enabled) {
						refdata_dma_start(context);
					}
					if (context->dma_hook) {
						context->dma_hook(context);
					}
					return 1;
				} else {
					context->flags |= FLAG_DMA_RUN;
					if (refdata_enabled) {
						refdata_dma_start(context);
					}
					if (context->dma_hook) {
						context->dma_hook(context);
					}
					//printf("DMA Copy Address: %X, New CD: %X, Source: %X\n", context->address, context->cd, (context->regs[REG_DMASRC_M] << 8) | context->regs[REG_DMASRC_L]);
				}
			} else {
				//printf("DMA Fill Address: %X, New CD: %X\n", context->address, context->cd);
			}
		}
	} else {
		uint8_t mode_5 = context->regs[REG_MODE_2] & BIT_MODE_5;
		context->address = context->address_latch | (value & 0x3FFF);
		context->cd = (context->cd & 0x3C) | (value >> 14);
		if ((value & 0xC000) == 0x8000) {
			//Register write
			uint16_t reg = (value >> 8) & 0x1F;
			if (context->reg_hook) {
				context->reg_hook(context, reg, value);
			}
			vdp_reg_write(context, reg, value);
		} else if (mode_5) {
			context->flags |= FLAG_PENDING;
			//Should these be taken care of here or after the second write?
			//context->flags &= ~FLAG_READ_FETCHED;
			//context->flags2 &= ~FLAG2_READ_PENDING;
		} else {
			context->flags &= ~FLAG_READ_FETCHED;
			context->flags2 &= ~FLAG2_READ_PENDING;
		}
	}
	return 0;
}

void vdp_control_port_write_pbc(vdp_context *context, uint8_t value)
{
	if (context->flags2 & FLAG2_BYTE_PENDING) {
		uint16_t full_val = value << 8 | context->pending_byte;
		context->flags2 &= ~FLAG2_BYTE_PENDING;
		//TODO: Deal with fact that Vbus->VDP DMA doesn't do anything in PBC mode
		vdp_control_port_write(context, full_val, context->cycles);
		if (context->cd == VRAM_READ) {
			context->cd = VRAM_READ8;
		}
	} else {
		context->pending_byte = value;
		context->flags2 |= FLAG2_BYTE_PENDING;
	}
}

void vdp_data_port_write(vdp_context * context, uint16_t value)
{
	//printf("data port write: %X at %d\n", value, context->cycles);
	if (context->flags & FLAG_PENDING) {
		context->flags &= ~FLAG_PENDING;
		//Should these be cleared here?
		context->flags &= ~FLAG_READ_FETCHED;
		context->flags2 &= ~FLAG2_READ_PENDING;
	}
	/*if (context->fifo_cur == context->fifo_end) {
		printf("FIFO full, waiting for space before next write at cycle %X\n", context->cycles);
	}*/
	if (context->cd & 0x20 && (context->regs[REG_DMASRC_H] & DMA_TYPE_MASK) == DMA_FILL) {
		context->flags &= ~FLAG_DMA_RUN;
	}
	while (context->fifo_write == context->fifo_read) {
		vdp_run_context_full(context, context->cycles + ((context->regs[REG_MODE_4] & BIT_H40) ? 16 : 20));
	}
	if (context->data_hook) {
		context->data_hook(context, value);
	}
	fifo_entry * cur = context->fifo + context->fifo_write;
	cur->cycle = context->cycles + ((context->regs[REG_MODE_4] & BIT_H40) ? 16 : 20)*FIFO_LATENCY;
	cur->address = context->address;
	cur->value = value;
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		cur->cd = context->cd;
	} else {
		cur->cd = (context->cd & 2) | 1;
	}
	cur->partial = 0;
	if (context->fifo_read < 0) {
		context->fifo_read = context->fifo_write;
	}
	context->fifo_write = (context->fifo_write + 1) & (FIFO_SIZE-1);
	increment_address(context);
}

void vdp_data_port_write_pbc(vdp_context * context, uint8_t value)
{
	if (context->flags & FLAG_PENDING) {
		context->flags &= ~FLAG_PENDING;
		//Should these be cleared here?
		context->flags &= ~FLAG_READ_FETCHED;
		context->flags2 &= ~FLAG2_READ_PENDING;
	}
	context->flags2 &= ~FLAG2_BYTE_PENDING;
	/*if (context->fifo_cur == context->fifo_end) {
		printf("FIFO full, waiting for space before next write at cycle %X\n", context->cycles);
	}*/
	if (context->cd & 0x20 && (context->regs[REG_DMASRC_H] & DMA_TYPE_MASK) == DMA_FILL) {
		context->flags &= ~FLAG_DMA_RUN;
	}
	while (context->fifo_write == context->fifo_read) {
		vdp_run_context_full(context, context->cycles + ((context->regs[REG_MODE_4] & BIT_H40) ? 16 : 20));
	}
	fifo_entry * cur = context->fifo + context->fifo_write;
	cur->cycle = context->cycles + ((context->regs[REG_MODE_4] & BIT_H40) ? 16 : 20)*FIFO_LATENCY;
	cur->address = context->address;
	cur->value = value;
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		cur->cd = context->cd;
	} else {
		if ((context->cd & 3) == CRAM_WRITE) {
			cur->cd = CRAM_WRITE;
		} else {
			cur->cd = VRAM_WRITE;
		}
	}
	cur->partial = 3;
	if (context->fifo_read < 0) {
		context->fifo_read = context->fifo_write;
	}
	context->fifo_write = (context->fifo_write + 1) & (FIFO_SIZE-1);
	increment_address(context);
}

void vdp_test_port_select(vdp_context * context, uint16_t value)
{
	context->selected_test_reg = value >> 8 & 0xF;
}

void vdp_test_port_write(vdp_context * context, uint16_t value)
{
	if (context->selected_test_reg < 8) {
		context->test_regs[context->selected_test_reg] = value;
	}
}

uint16_t vdp_status(vdp_context *context)
{
	//Bits 15-10 are not fixed like Charles MacDonald's doc suggests, but instead open bus values that reflect 68K prefetch
	uint16_t value = context->system->get_open_bus_value(context->system) & 0xFC00;
	if (context->fifo_read < 0) {
		value |= 0x200;
	}
	if (context->fifo_read == context->fifo_write) {
		value |= 0x100;
	}
	if (context->flags2 & FLAG2_VINT_PENDING) {
		value |= 0x80;
	}
	if (context->flags & FLAG_SPRITE_OFLOW) {
		value |= 0x40;
	}
	if (context->flags2 & FLAG2_SPRITE_COLLIDE) {
		value |= 0x20;
	}
	if ((context->regs[REG_MODE_4] & BIT_INTERLACE) && !(context->flags2 & FLAG2_EVEN_FIELD)) {
		value |= 0x10;
	}
	uint32_t slot = context->hslot;
	if (!is_active(context)) {
		value |= 0x8;
	}
	if (context->regs[REG_MODE_4] & BIT_H40) {
		if (slot < HBLANK_END_H40 || slot > HBLANK_START_H40) {
			value |= 0x4;
		}
	} else {
		if (slot < HBLANK_END_H32 || slot > HBLANK_START_H32) {
			value |= 0x4;
		}
	}
	if (context->cd & 0x20) {
		value |= 0x2;
	}
	if (context->flags2 & FLAG2_REGION_PAL) {
		value |= 0x1;
	}
	return value;
}

uint16_t vdp_control_port_read(vdp_context * context)
{
	uint16_t value = vdp_status(context);
	context->flags &= ~(FLAG_SPRITE_OFLOW|FLAG_PENDING);
	context->flags2 &= ~(FLAG2_SPRITE_COLLIDE|FLAG2_BYTE_PENDING);
	//printf("status read at cycle %d returned %X\n", context->cycles, value);
	return value;
}

uint16_t vdp_data_port_read(vdp_context * context, uint32_t *cpu_cycle, uint32_t cpu_divider)
{
	if (context->flags & FLAG_PENDING) {
		context->flags &= ~FLAG_PENDING;
		//Should these be cleared here?
		context->flags &= ~FLAG_READ_FETCHED;
		context->flags2 &= ~FLAG2_READ_PENDING;
	}
	if (context->cd & 1) {
		warning("Read from VDP data port while writes are configured, CPU is now frozen. VDP Address: %X, CD: %X\n", context->address, context->cd);
		context->system->enter_debugger = 1;
		return context->prefetch;
	}
	switch (context->cd)
	{
	case VRAM_READ:
	case VSRAM_READ:
	case CRAM_READ:
	case VRAM_READ8:
		break;
	default:
		warning("Read from VDP data port with invalid source, CPU is now frozen. VDP Address: %X, CD: %X\n", context->address, context->cd);
		context->system->enter_debugger = 1;
		return context->prefetch;
	}
	uint32_t starting_cycle = context->cycles;
	while (!(context->flags & FLAG_READ_FETCHED)) {
		vdp_run_context_full(context, context->cycles + ((context->regs[REG_MODE_4] & BIT_H40) ? 16 : 20));
	}
	context->flags &= ~FLAG_READ_FETCHED;
	//TODO: Make some logic analyzer captures to better characterize what's happening with read latency here
	if (context->cycles != starting_cycle) {
		uint32_t delta = context->cycles - *cpu_cycle;
		uint32_t cpu_delta = delta / cpu_divider;
		if (delta % cpu_divider) {
			cpu_delta++;
		}
		*cpu_cycle += cpu_delta * cpu_divider;
		if (*cpu_cycle - context->cycles < 2) {
			context->read_latency = context->cycles + ((context->regs[REG_MODE_4] & BIT_H40) ? 16 : 20)*(READ_LATENCY - 1);
		} else {
			context->read_latency = context->cycles + ((context->regs[REG_MODE_4] & BIT_H40) ? 16 : 20)*READ_LATENCY;
		}
	} else {
		context->read_latency = *cpu_cycle + ((context->regs[REG_MODE_4] & BIT_H40) ? 16 : 20)*(READ_LATENCY - 1);
	}
	return context->prefetch;
}

uint8_t vdp_data_port_read_pbc(vdp_context * context)
{
	context->flags &= ~(FLAG_PENDING | FLAG_READ_FETCHED);
	context->flags2 &= ~FLAG2_BYTE_PENDING;

	context->cd = VRAM_READ8;
	return context->prefetch;
}

void vdp_adjust_cycles(vdp_context * context, uint32_t deduction)
{
	context->cycles -= deduction;
	if (context->pending_vint_start >= deduction) {
		context->pending_vint_start -= deduction;
	} else {
		context->pending_vint_start = 0;
	}
	if (context->pending_hint_start >= deduction) {
		context->pending_hint_start -= deduction;
	} else {
		context->pending_hint_start = 0;
	}
	if (context->fifo_read >= 0) {
		int32_t idx = context->fifo_read;
		do {
			if (context->fifo[idx].cycle >= deduction) {
				context->fifo[idx].cycle -= deduction;
			} else {
				context->fifo[idx].cycle = 0;
			}
			idx = (idx+1) & (FIFO_SIZE-1);
		} while(idx != context->fifo_write);
	}
	if (context->read_latency >= deduction) {
		context->read_latency -= deduction;
	} else {
		context->read_latency = 0;
	}
}

static uint32_t vdp_cycles_hslot_wrap_h40(vdp_context * context)
{
	if (context->hslot < 183) {
		return MCLKS_LINE - context->hslot * MCLKS_SLOT_H40;
	} else if (context->hslot < HSYNC_END_H40) {
		uint32_t before_hsync = context->hslot < HSYNC_SLOT_H40 ? (HSYNC_SLOT_H40 - context->hslot) * MCLKS_SLOT_H40 : 0;
		uint32_t hsync = 0;
		for (int i = context->hslot <= HSYNC_SLOT_H40 ? 0 : context->hslot - HSYNC_SLOT_H40; i < sizeof(h40_hsync_cycles)/sizeof(uint32_t); i++)
		{
			hsync += h40_hsync_cycles[i];
		}
		uint32_t after_hsync = (256- HSYNC_END_H40) * MCLKS_SLOT_H40;
		return before_hsync + hsync + after_hsync;
	} else {
		return (256-context->hslot) * MCLKS_SLOT_H40;
	}
}

static uint32_t vdp_cycles_next_line(vdp_context * context)
{
	if (context->regs[REG_MODE_4] & BIT_H40) {
		//TODO: Handle "illegal" Mode 4/H40 combo
		if (context->hslot < LINE_CHANGE_H40) {
			return (LINE_CHANGE_H40 - context->hslot) * MCLKS_SLOT_H40;
		} else {
			return vdp_cycles_hslot_wrap_h40(context) + LINE_CHANGE_H40 * MCLKS_SLOT_H40;
		}
	} else {
		if (context->regs[REG_MODE_2] & BIT_MODE_5) {
			if (context->hslot < LINE_CHANGE_H32) {
				return (LINE_CHANGE_H32 - context->hslot) * MCLKS_SLOT_H32;
			} else if (context->hslot < 148) {
				return MCLKS_LINE - (context->hslot - LINE_CHANGE_H32) * MCLKS_SLOT_H32;
			} else {
				return (256-context->hslot + LINE_CHANGE_H32) * MCLKS_SLOT_H32;
			}
		} else {
			if (context->hslot < 148) {
				return (148 - context->hslot + LINE_CHANGE_MODE4 - 233) * MCLKS_SLOT_H32;
			} else if (context->hslot < LINE_CHANGE_MODE4) {
				return (LINE_CHANGE_MODE4 - context->hslot) * MCLKS_SLOT_H32;
			} else {
				return MCLKS_LINE - (context->hslot - LINE_CHANGE_MODE4) * MCLKS_SLOT_H32;
			}
		}
	}
}

static void get_jump_params(vdp_context *context, uint32_t *jump_start, uint32_t *jump_dst)
{
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		if (context->flags2 & FLAG2_REGION_PAL) {
			if (context->regs[REG_MODE_2] & BIT_PAL) {
				*jump_start = 0x10B;
				*jump_dst = 0x1D2;
			} else {
				*jump_start = 0x103;
				*jump_dst = 0x1CA;
			}
		} else {
			if (context->regs[REG_MODE_2] & BIT_PAL) {
				*jump_start = 0x100;
				*jump_dst = 0x1FA;
			} else {
				*jump_start = 0xEB;
				*jump_dst = 0x1E5;
			}
		}
	} else {
		*jump_start = 0xDB;
		*jump_dst = 0x1D5;
	}
}

static uint32_t vdp_cycles_to_line(vdp_context * context, uint32_t target)
{
	uint32_t jump_start, jump_dst;
	get_jump_params(context, &jump_start, &jump_dst);
	uint32_t lines;
	if (context->vcounter < target) {
		if (target < jump_start || context->vcounter > jump_start) {
			lines = target - context->vcounter;
		} else {
			lines = jump_start - context->vcounter + target - jump_dst;
		}
	} else {
		if (context->vcounter < jump_start) {
			lines = jump_start - context->vcounter + 512 - jump_dst;
		} else {
			lines = 512 - context->vcounter;
		}
		if (target < jump_start) {
			lines += target;
		} else {
			lines += jump_start + target - jump_dst;
		}
	}
	return MCLKS_LINE * (lines - 1) + vdp_cycles_next_line(context);
}

uint32_t vdp_cycles_to_frame_end(vdp_context * context)
{
	return context->cycles + vdp_cycles_to_line(context, context->inactive_start);
}

uint32_t vdp_next_hint(vdp_context * context)
{
	if (!(context->regs[REG_MODE_1] & BIT_HINT_EN)) {
		return 0xFFFFFFFF;
	}
	if (context->flags2 & FLAG2_HINT_PENDING) {
		return context->pending_hint_start;
	}
	uint32_t hint_line;
	if (context->state != ACTIVE) {
		hint_line = context->regs[REG_HINT];
		if (hint_line > context->inactive_start) {
			return 0xFFFFFFFF;
		}
	} else {
		hint_line = context->vcounter + context->hint_counter + 1;
		if (context->vcounter < context->inactive_start) {
			if (hint_line > context->inactive_start) {
				hint_line = context->regs[REG_HINT];
				if (hint_line > context->inactive_start) {
					return 0xFFFFFFFF;
				}
				if (hint_line >= context->vcounter) {
					//Next interrupt is for a line in the next frame that
					//is higher than the line we're on now so just passing
					//that line number to vdp_cycles_to_line will yield the wrong
					//result
					return context->cycles + vdp_cycles_to_line(context,  0) + hint_line * MCLKS_LINE;
				}
			}
		} else {
			uint32_t jump_start, jump_dst;
			get_jump_params(context, &jump_start, &jump_dst);
			if (hint_line >= jump_start && context->vcounter < jump_dst) {
				hint_line = (hint_line + jump_dst - jump_start) & 0x1FF;
			}
			if (hint_line < context->vcounter && hint_line > context->inactive_start) {
				return 0xFFFFFFFF;
			}
		}
	}
	return context->cycles + vdp_cycles_to_line(context, hint_line);
}

static uint32_t vdp_next_vint_real(vdp_context * context)
{
	if (!(context->regs[REG_MODE_2] & BIT_VINT_EN)) {
		return 0xFFFFFFFF;
	}
	if (context->flags2 & FLAG2_VINT_PENDING) {
		return context->pending_vint_start;
	}


	return vdp_next_vint_z80(context);
}

uint32_t vdp_next_vint(vdp_context *context)
{
	uint32_t ret = vdp_next_vint_real(context);
#ifdef TIMING_DEBUG
	static uint32_t last = 0xFFFFFFFF;
	if (last != ret) {
		printf("vdp_next_vint is %d at frame %d, line %d, hslot %d\n", ret, context->frame, context->vcounter, context->hslot);
	}
	last = ret;
#endif
	return ret;
}

uint32_t vdp_next_vint_z80(vdp_context * context)
{
	uint16_t vint_line = (context->regs[REG_MODE_2] & BIT_MODE_5) ? context->inactive_start : context->inactive_start + 1;
	if (context->vcounter == vint_line) {
		if (context->regs[REG_MODE_2] & BIT_MODE_5) {
			if (context->regs[REG_MODE_4] & BIT_H40) {
				if (context->hslot >= LINE_CHANGE_H40 || context->hslot <= VINT_SLOT_H40) {
					uint32_t cycles = context->cycles;
					if (context->hslot >= LINE_CHANGE_H40) {
						if (context->hslot < 183) {
							cycles += (183 - context->hslot) * MCLKS_SLOT_H40;
						}

						if (context->hslot < HSYNC_SLOT_H40) {
							cycles += (HSYNC_SLOT_H40 - (context->hslot >= 229 ? context->hslot : 229)) * MCLKS_SLOT_H40;
						}
						for (int slot = context->hslot <= HSYNC_SLOT_H40 ? HSYNC_SLOT_H40 : context->hslot; slot < HSYNC_END_H40; slot++ )
						{
							cycles += h40_hsync_cycles[slot - HSYNC_SLOT_H40];
						}
						cycles += (256 - (context->hslot > HSYNC_END_H40 ? context->hslot : HSYNC_END_H40)) * MCLKS_SLOT_H40;
					}

					cycles += (VINT_SLOT_H40 - (context->hslot >= LINE_CHANGE_H40 ? 0 : context->hslot)) * MCLKS_SLOT_H40;
					return cycles;
				}
			} else {
				if (context->hslot >= LINE_CHANGE_H32 || context->hslot <= VINT_SLOT_H32) {
					if (context->hslot <= VINT_SLOT_H32) {
						return context->cycles + (VINT_SLOT_H32 - context->hslot) * MCLKS_SLOT_H32;
					} else if (context->hslot < 233) {
						return context->cycles + (VINT_SLOT_H32 + 256 - 233 + 148 - context->hslot) * MCLKS_SLOT_H32;
					} else {
						return context->cycles + (VINT_SLOT_H32 + 256 - context->hslot) * MCLKS_SLOT_H32;
					}
				}
			}
		} else {
			if (context->hslot >= LINE_CHANGE_MODE4) {
				return context->cycles + (VINT_SLOT_MODE4 + 256 - context->hslot) * MCLKS_SLOT_H32;
			}
			if (context->hslot <= VINT_SLOT_MODE4) {
				return context->cycles + (VINT_SLOT_MODE4 - context->hslot) * MCLKS_SLOT_H32;
			}
		}
	}
	int32_t cycles_to_vint = vdp_cycles_to_line(context, vint_line);
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		if (context->regs[REG_MODE_4] & BIT_H40) {
			cycles_to_vint += MCLKS_LINE - (LINE_CHANGE_H40 - VINT_SLOT_H40) * MCLKS_SLOT_H40;
		} else {
			cycles_to_vint += (VINT_SLOT_H32 + 256 - 233 + 148 - LINE_CHANGE_H32) * MCLKS_SLOT_H32;
		}
	} else {
		cycles_to_vint += (256 - LINE_CHANGE_MODE4 + VINT_SLOT_MODE4) * MCLKS_SLOT_H32;
	}
	return context->cycles + cycles_to_vint;
}

uint32_t vdp_next_nmi(vdp_context *context)
{
	if (!(context->flags2 & FLAG2_PAUSE)) {
		return 0xFFFFFFFF;
	}
	return context->cycles + vdp_cycles_to_line(context, 0x1FF);
}

void vdp_pbc_pause(vdp_context *context)
{
	context->flags2 |= FLAG2_PAUSE;
}

void vdp_int_ack(vdp_context * context)
{
	//CPU interrupt acknowledge is only used in Mode 5
	if (context->regs[REG_MODE_2] & BIT_MODE_5) {
		//Apparently the VDP interrupt controller is not very smart
		//Instead of paying attention to what interrupt is being acknowledged it just
		//clears the pending flag for whatever interrupt it is currently asserted
		//which may be different from the interrupt it was asserting when the 68k
		//started the interrupt process. The window for this is narrow and depends
		//on the latency between the int enable register write and the interrupt being
		//asserted, but Fatal Rewind depends on this due to some buggy code
		if ((context->flags2 & FLAG2_VINT_PENDING) && (context->regs[REG_MODE_2] & BIT_VINT_EN)) {
			context->flags2 &= ~FLAG2_VINT_PENDING;
		} else if((context->flags2 & FLAG2_HINT_PENDING) && (context->regs[REG_MODE_1] & BIT_HINT_EN)) {
			context->flags2 &= ~FLAG2_HINT_PENDING;
		}
	}
}

#define VDP_STATE_VERSION 5
void vdp_serialize(vdp_context *context, serialize_buffer *buf)
{
	save_int8(buf, VDP_STATE_VERSION);
	save_int8(buf, VRAM_SIZE / 1024);//VRAM size in KB, needed for future proofing
	save_buffer8(buf, context->vdpmem, VRAM_SIZE);
	save_buffer16(buf, context->cram, CRAM_SIZE);
	save_buffer16(buf, context->vsram, MAX_VSRAM_SIZE);
	save_buffer8(buf, context->sat_cache, SAT_CACHE_SIZE);
	for (int i = 0; i <= REG_DMASRC_H; i++)
	{
		save_int8(buf, context->regs[i]);
	}
	save_int32(buf, context->address);
	save_int32(buf, context->serial_address);
	save_int8(buf, context->cd);
	uint8_t fifo_size;
	if (context->fifo_read < 0) {
		fifo_size = 0;
	} else if (context->fifo_write > context->fifo_read) {
		fifo_size = context->fifo_write - context->fifo_read;
	} else {
		fifo_size = context->fifo_write + FIFO_SIZE - context->fifo_read;
	}
	save_int8(buf, fifo_size);
	for (int i = 0, cur = context->fifo_read; i < fifo_size; i++)
	{
		fifo_entry *entry = context->fifo + cur;
		cur = (cur + 1) & (FIFO_SIZE - 1);
		save_int32(buf, entry->cycle);
		save_int32(buf, entry->address);
		save_int16(buf, entry->value);
		save_int8(buf, entry->cd);
		save_int8(buf, entry->partial);
	}
	//FIXME: Flag bits should be rearranged for maximum correspondence to status reg
	save_int16(buf, context->flags2 << 8 | context->flags);
	save_int32(buf, context->frame);
	save_int16(buf, context->vcounter);
	save_int8(buf, context->hslot);
	save_int16(buf, context->hv_latch);
	save_int8(buf, context->state);
	save_int16(buf, context->hscroll_a);
	save_int16(buf, context->hscroll_b);
	save_int16(buf, context->vscroll_latch[0]);
	save_int16(buf, context->vscroll_latch[1]);
	save_int16(buf, context->col_1);
	save_int16(buf, context->col_2);
	save_int16(buf, context->test_regs[0]);
	save_buffer8(buf, context->tmp_buf_a, SCROLL_BUFFER_SIZE);
	save_buffer8(buf, context->tmp_buf_b, SCROLL_BUFFER_SIZE);
	save_int8(buf, context->buf_a_off);
	save_int8(buf, context->buf_b_off);
	//FIXME: Sprite rendering state is currently a mess
	save_int8(buf, context->sprite_index);
	save_int8(buf, context->sprite_draws);
	save_int8(buf, context->slot_counter);
	save_int8(buf, context->cur_slot);
	for (int i = 0; i < MAX_SPRITES_LINE; i++)
	{
		sprite_draw *draw = context->sprite_draw_list + i;
		save_int16(buf, draw->address);
		save_int16(buf, draw->x_pos);
		save_int8(buf, draw->pal_priority);
		save_int8(buf, draw->h_flip);
		save_int8(buf, draw->width);
		save_int8(buf, draw->height);
	}
	for (int i = 0; i < MAX_SPRITES_LINE; i++)
	{
		sprite_info *info = context->sprite_info_list + i;
		save_int8(buf, info->size);
		save_int8(buf, info->index);
		save_int16(buf, info->y);
	}
	save_buffer8(buf, context->linebuf, LINEBUF_SIZE);

	save_int32(buf, context->cycles);
	save_int32(buf, context->pending_vint_start);
	save_int32(buf, context->pending_hint_start);
	save_int32(buf, context->address_latch);
	//was cd_latch, for compatibility with older builds that expect it
	save_int8(buf, context->cd);
	save_int8(buf, context->window_h_latch);
	save_int8(buf, context->window_v_latch);
	save_buffer16(buf, context->test_regs + 1, 7);
	save_int8(buf, context->selected_test_reg);
}

void vdp_deserialize(deserialize_buffer *buf, void *vcontext)
{
	vdp_context *context = vcontext;
	uint8_t version = load_int8(buf);
	uint8_t vramk;
	if (version == 64) {
		vramk = version;
		version = 0;
	} else {
		vramk = load_int8(buf);
	}
	if (version > VDP_STATE_VERSION) {
		warning("Save state has VDP version %d, but this build only understands versions %d and lower", version, VDP_STATE_VERSION);
	}
	load_buffer8(buf, context->vdpmem, (vramk * 1024) <= VRAM_SIZE ? vramk * 1024 : VRAM_SIZE);
	if ((vramk * 1024) > VRAM_SIZE) {
		buf->cur_pos += (vramk * 1024) - VRAM_SIZE;
	}
	load_buffer16(buf, context->cram, CRAM_SIZE);
	for (int i = 0; i < CRAM_SIZE; i++)
	{
		update_color_map(context, i, context->cram[i]);
	}
	load_buffer16(buf, context->vsram, version > 1 ? MAX_VSRAM_SIZE : MIN_VSRAM_SIZE);
	load_buffer8(buf, context->sat_cache, SAT_CACHE_SIZE);
	for (int i = 0; i <= REG_DMASRC_H; i++)
	{
		context->regs[i] = load_int8(buf);
	}
	context->address = load_int32(buf);
	context->serial_address = load_int32(buf);
	context->cd = load_int8(buf);
	uint8_t fifo_size = load_int8(buf);
	if (fifo_size > FIFO_SIZE) {
		fatal_error("Invalid fifo size %d", fifo_size);
	}
	if (fifo_size) {
		context->fifo_read = 0;
		context->fifo_write = fifo_size & (FIFO_SIZE - 1);
		for (int i = 0; i < fifo_size; i++)
		{
			fifo_entry *entry = context->fifo + i;
			entry->cycle = load_int32(buf);
			entry->address = load_int32(buf);
			entry->value = load_int16(buf);
			entry->cd = load_int8(buf);
			entry->partial = load_int8(buf);
		}
	} else {
		context->fifo_read = -1;
		context->fifo_write = 0;
	}
	uint16_t flags = load_int16(buf);
	context->flags2 = flags >> 8;
	context->flags = flags;
	context->frame = load_int32(buf);
	context->vcounter = load_int16(buf);
	context->hslot = load_int8(buf);
	context->hv_latch = load_int16(buf);
	context->state = load_int8(buf);
	context->hscroll_a = load_int16(buf);
	context->hscroll_b = load_int16(buf);
	context->vscroll_latch[0] = load_int16(buf);
	context->vscroll_latch[1] = load_int16(buf);
	context->col_1 = load_int16(buf);
	context->col_2 = load_int16(buf);
	context->test_regs[0] = load_int16(buf);
	load_buffer8(buf, context->tmp_buf_a, SCROLL_BUFFER_SIZE);
	load_buffer8(buf, context->tmp_buf_b, SCROLL_BUFFER_SIZE);
	context->buf_a_off = load_int8(buf) & SCROLL_BUFFER_MASK;
	context->buf_b_off = load_int8(buf) & SCROLL_BUFFER_MASK;
	context->sprite_index = load_int8(buf);
	context->sprite_draws = load_int8(buf);
	context->slot_counter = load_int8(buf);
	context->cur_slot = load_int8(buf);
	if (version == 0) {
		int cur_draw = 0;
		for (int i = 0; i < MAX_SPRITES_LINE * 2; i++)
		{
			if (cur_draw < MAX_SPRITES_LINE) {
				sprite_draw *last = cur_draw ? context->sprite_draw_list + cur_draw - 1 : NULL;
				sprite_draw *draw = context->sprite_draw_list + cur_draw++;
				draw->address = load_int16(buf);
				draw->x_pos = load_int16(buf);
				draw->pal_priority = load_int8(buf);
				draw->h_flip = load_int8(buf);
				draw->width = 1;
				draw->height = 8;

				if (last && last->width < 4 && last->h_flip == draw->h_flip && last->pal_priority == draw->pal_priority) {
					int adjust_x = draw->x_pos + draw->h_flip ? -8 : 8;
					int height = draw->address - last->address /4;
					if (last->x_pos == adjust_x && (
						(last->width > 1 && height == last->height) ||
						(last->width == 1 && (height == 8 || height == 16 || height == 24 || height == 32))
					)) {
						//current draw appears to be part of the same sprite as the last one, combine it
						cur_draw--;
						last->width++;
					}
				}
			} else {
				load_int16(buf);
				load_int16(buf);
				load_int8(buf);
				load_int8(buf);
			}
		}
	} else {
		for (int i = 0; i < MAX_SPRITES_LINE; i++)
		{
			sprite_draw *draw = context->sprite_draw_list + i;
			draw->address = load_int16(buf);
			draw->x_pos = load_int16(buf);
			draw->pal_priority = load_int8(buf);
			draw->h_flip = load_int8(buf);
			draw->width = load_int8(buf);
			draw->height = load_int8(buf);
		}
	}
	for (int i = 0; i < MAX_SPRITES_LINE; i++)
	{
		sprite_info *info = context->sprite_info_list + i;
		info->size = load_int8(buf);
		info->index = load_int8(buf);
		info->y = load_int16(buf);
	}
	load_buffer8(buf, context->linebuf, LINEBUF_SIZE);

	context->cycles = load_int32(buf);
	context->pending_vint_start = load_int32(buf);
	context->pending_hint_start = load_int32(buf);
	context->window_h_latch = context->regs[REG_WINDOW_H];
	context->window_v_latch = context->regs[REG_WINDOW_V];
	if (version > 2) {
		context->address_latch = load_int32(buf);
		//was cd_latch, no longer used
		load_int8(buf);
		if (version > 3) {
			context->window_h_latch = load_int8(buf);
			context->window_v_latch = load_int8(buf);
		}
	} else {
		context->address_latch = context->address;
	}
	if (version > 4) {
		load_buffer16(buf, context->test_regs + 1, 7);
		context->selected_test_reg = load_int8(buf);
	} else {
		memset(context->test_regs + 1, 0, 7 * sizeof(uint16_t));
		context->selected_test_reg = 0;
	}
	update_video_params(context);
}

static vdp_context *current_vdp;
static void vdp_debug_window_close(uint8_t which)
{
	//TODO: remove need for current_vdp global, and find the VDP via current_system instead
	for (int i = 0; i < NUM_DEBUG_TYPES; i++)
	{
		if (current_vdp->enabled_debuggers & (1 << i) && which == current_vdp->debug_fb_indices[i]) {
			vdp_toggle_debug_view(current_vdp, i);
			break;
		}
	}
}

void vdp_toggle_debug_view(vdp_context *context, uint8_t debug_type)
{
	if (context->enabled_debuggers & 1 << debug_type) {
		render_destroy_window(context->debug_fb_indices[debug_type]);
		context->enabled_debuggers &= ~(1 << debug_type);
	} else {
		uint32_t width,height;
		uint8_t fetch_immediately = 0;
		char *caption;
		switch(debug_type)
		{
		case DEBUG_PLANE:
			caption = "BlastEm - VDP Plane Debugger";
			if (context->type == VDP_GENESIS) {
				width = height = 1024;
			} else {
				width = height = 512;
			}
			break;
		case DEBUG_VRAM:
			caption = "BlastEm - VDP VRAM Debugger";
			width = 1024;
			height = 512;
			break;
		case DEBUG_CRAM:
			caption = "BlastEm - VDP CRAM Debugger";
			width = 512;
			height = 512;
			fetch_immediately = 1;
			break;
		case DEBUG_COMPOSITE:
			caption = "BlastEm - VDP Plane Composition Debugger";
			width = LINEBUF_SIZE;
			height = context->inactive_start + context->border_top + context->border_bot;
			fetch_immediately = 1;
			break;
		default:
			return;
		}
		current_vdp = context;
#ifdef DISABLE_NUKLEAR
		context->debug_fb_indices[debug_type] = render_create_window(caption, width, height, vdp_debug_window_close);
#else
		context->debug_fb_indices[debug_type] = debug_create_window(debug_type, caption, width, height, vdp_debug_window_close);
#endif
		if (context->debug_fb_indices[debug_type]) {
			context->enabled_debuggers |= 1 << debug_type;
		}
		if (fetch_immediately) {
			context->debug_fbs[debug_type] = render_get_framebuffer(context->debug_fb_indices[debug_type], &context->debug_fb_pitch[debug_type]);
		}
	}
}

void vdp_inc_debug_mode(vdp_context *context)
{
	uint8_t active = render_get_active_framebuffer();
	if (active < FRAMEBUFFER_USER_START) {
		return;
	}
	for (int i = 0; i < NUM_DEBUG_TYPES; i++)
	{
		if (context->enabled_debuggers & (1 << i) && context->debug_fb_indices[i] == active) {
			context->debug_modes[i]++;
			return;
		}
	}
}

void vdp_replay_event(vdp_context *context, uint8_t event, event_reader *reader)
{
	uint32_t address;
	deserialize_buffer *buffer = &reader->buffer;
	switch (event)
	{
	case EVENT_VRAM_BYTE:
		reader_ensure_data(reader, 3);
		address = load_int16(buffer);
		break;
	case EVENT_VRAM_BYTE_DELTA:
		reader_ensure_data(reader, 2);
		address = reader->last_byte_address + load_int8(buffer);
		break;
	case EVENT_VRAM_BYTE_ONE:
		reader_ensure_data(reader, 1);
		address = reader->last_byte_address + 1;
		break;
	case EVENT_VRAM_BYTE_AUTO:
		reader_ensure_data(reader, 1);
		address = reader->last_byte_address + context->regs[REG_AUTOINC];
		break;
	case EVENT_VRAM_WORD:
		reader_ensure_data(reader, 4);
		address = load_int8(buffer) << 16;
		address |= load_int16(buffer);
		break;
	case EVENT_VRAM_WORD_DELTA:
		reader_ensure_data(reader, 3);
		address = reader->last_word_address + load_int8(buffer);
		break;
	case EVENT_VDP_REG:
	case EVENT_VDP_INTRAM:
		reader_ensure_data(reader, event == EVENT_VDP_REG ? 2 : 3);
		address = load_int8(buffer);
		break;
	}

	switch (event)
	{
	case EVENT_VDP_REG: {
		uint8_t value = load_int8(buffer);
		context->regs[address] = value;
		if (address == REG_MODE_4) {
			context->double_res = (value & (BIT_INTERLACE | BIT_DOUBLE_RES)) == (BIT_INTERLACE | BIT_DOUBLE_RES);
			if (!context->double_res) {
				context->flags2 &= ~FLAG2_EVEN_FIELD;
			}
		}
		if (address == REG_MODE_1 || address == REG_MODE_2 || address == REG_MODE_4) {
			update_video_params(context);
		}
		break;
	}
	case EVENT_VRAM_BYTE:
	case EVENT_VRAM_BYTE_DELTA:
	case EVENT_VRAM_BYTE_ONE:
	case EVENT_VRAM_BYTE_AUTO: {
		uint8_t byte = load_int8(buffer);
		reader->last_byte_address = address;
		vdp_check_update_sat_byte(context, address ^ 1, byte);
		write_vram_byte(context, address ^ 1, byte);
		break;
	}
	case EVENT_VRAM_WORD:
	case EVENT_VRAM_WORD_DELTA: {
		uint16_t value = load_int16(buffer);
		reader->last_word_address = address;
		vdp_check_update_sat(context, address, value);
		write_vram_word(context, address, value);
		break;
	}
	case EVENT_VDP_INTRAM:
		if (address < 128) {
			write_cram(context, address, load_int16(buffer));
		} else {
			context->vsram[address&63] = load_int16(buffer);
		}
		break;
	}
}
