#include "video-software.h"

#include "gba.h"
#include "gba-io.h"

#include <stdlib.h>
#include <string.h>

static void GBAVideoSoftwareRendererInit(struct GBAVideoRenderer* renderer);
static void GBAVideoSoftwareRendererDeinit(struct GBAVideoRenderer* renderer);
static void GBAVideoSoftwareRendererWritePalette(struct GBAVideoRenderer* renderer, uint32_t address, uint16_t value);
static uint16_t GBAVideoSoftwareRendererWriteVideoRegister(struct GBAVideoRenderer* renderer, uint32_t address, uint16_t value);
static void GBAVideoSoftwareRendererDrawScanline(struct GBAVideoRenderer* renderer, int y);
static void GBAVideoSoftwareRendererFinishFrame(struct GBAVideoRenderer* renderer);

static void GBAVideoSoftwareRendererUpdateDISPCNT(struct GBAVideoSoftwareRenderer* renderer);
static void GBAVideoSoftwareRendererWriteBGCNT(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBLDCNT(struct GBAVideoSoftwareRenderer* renderer, uint16_t value);

static void _drawScanline(struct GBAVideoSoftwareRenderer* renderer, int y);
static void _drawBackgroundMode0(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int y);
static void _drawTransformedSprite(struct GBAVideoSoftwareRenderer* renderer, struct GBATransformedObj* sprite, int y);
static void _drawSprite(struct GBAVideoSoftwareRenderer* renderer, struct GBAObj* sprite, int y);

static void _updatePalettes(struct GBAVideoSoftwareRenderer* renderer);
static inline uint32_t _brighten(uint32_t color, int y);
static inline uint32_t _darken(uint32_t color, int y);
static uint32_t _mix(int weightA, uint32_t colorA, int weightB, uint32_t colorB);

static void _sortBackgrounds(struct GBAVideoSoftwareRenderer* renderer);
static int _backgroundComparator(const void* a, const void* b);

void GBAVideoSoftwareRendererCreate(struct GBAVideoSoftwareRenderer* renderer) {
	renderer->d.init = GBAVideoSoftwareRendererInit;
	renderer->d.deinit = GBAVideoSoftwareRendererDeinit;
	renderer->d.writeVideoRegister = GBAVideoSoftwareRendererWriteVideoRegister;
	renderer->d.writePalette = GBAVideoSoftwareRendererWritePalette;
	renderer->d.drawScanline = GBAVideoSoftwareRendererDrawScanline;
	renderer->d.finishFrame = GBAVideoSoftwareRendererFinishFrame;

	renderer->d.turbo = 0;
	renderer->d.framesPending = 0;

	renderer->sortedBg[0] = &renderer->bg[0];
	renderer->sortedBg[1] = &renderer->bg[1];
	renderer->sortedBg[2] = &renderer->bg[2];
	renderer->sortedBg[3] = &renderer->bg[3];

	{
		pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
		renderer->mutex = mutex;
		pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
		renderer->upCond = cond;
		renderer->downCond = cond;
	}
}

static void GBAVideoSoftwareRendererInit(struct GBAVideoRenderer* renderer) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	int i;

	softwareRenderer->dispcnt.packed = 0x0080;

	softwareRenderer->target1Obj = 0;
	softwareRenderer->target1Bd = 0;
	softwareRenderer->target2Obj = 0;
	softwareRenderer->target2Bd = 0;
	softwareRenderer->blendEffect = BLEND_NONE;
	memset(softwareRenderer->normalPalette, 0, sizeof(softwareRenderer->normalPalette));
	memset(softwareRenderer->variantPalette, 0, sizeof(softwareRenderer->variantPalette));

	softwareRenderer->blda = 0;
	softwareRenderer->bldb = 0;
	softwareRenderer->bldy = 0;

	for (i = 0; i < 4; ++i) {
		struct GBAVideoSoftwareBackground* bg = &softwareRenderer->bg[i];
		bg->index = i;
		bg->enabled = 0;
		bg->priority = 0;
		bg->charBase = 0;
		bg->mosaic = 0;
		bg->multipalette = 0;
		bg->screenBase = 0;
		bg->overflow = 0;
		bg->size = 0;
		bg->target1 = 0;
		bg->target2 = 0;
		bg->x = 0;
		bg->y = 0;
		bg->refx = 0;
		bg->refy = 0;
		bg->dx = 256;
		bg->dmx = 0;
		bg->dy = 0;
		bg->dmy = 256;
		bg->sx = 0;
		bg->sy = 0;
	}

	pthread_mutex_init(&softwareRenderer->mutex, 0);
	pthread_cond_init(&softwareRenderer->upCond, 0);
	pthread_cond_init(&softwareRenderer->downCond, 0);
}

static void GBAVideoSoftwareRendererDeinit(struct GBAVideoRenderer* renderer) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;

	pthread_mutex_destroy(&softwareRenderer->mutex);
	pthread_cond_destroy(&softwareRenderer->upCond);
	pthread_cond_destroy(&softwareRenderer->downCond);
}

static uint16_t GBAVideoSoftwareRendererWriteVideoRegister(struct GBAVideoRenderer* renderer, uint32_t address, uint16_t value) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	switch (address) {
	case REG_DISPCNT:
		value &= 0xFFFB;
		softwareRenderer->dispcnt.packed = value;
		GBAVideoSoftwareRendererUpdateDISPCNT(softwareRenderer);
		break;
	case REG_BG0CNT:
		value &= 0xFFCF;
		GBAVideoSoftwareRendererWriteBGCNT(softwareRenderer, &softwareRenderer->bg[0], value);
		break;
	case REG_BG1CNT:
		value &= 0xFFCF;
		GBAVideoSoftwareRendererWriteBGCNT(softwareRenderer, &softwareRenderer->bg[1], value);
		break;
	case REG_BG2CNT:
		value &= 0xFFCF;
		GBAVideoSoftwareRendererWriteBGCNT(softwareRenderer, &softwareRenderer->bg[2], value);
		break;
	case REG_BG3CNT:
		value &= 0xFFCF;
		GBAVideoSoftwareRendererWriteBGCNT(softwareRenderer, &softwareRenderer->bg[3], value);
		break;
	case REG_BG0HOFS:
		value &= 0x01FF;
		softwareRenderer->bg[0].x = value;
		break;
	case REG_BG0VOFS:
		value &= 0x01FF;
		softwareRenderer->bg[0].y = value;
		break;
	case REG_BG1HOFS:
		value &= 0x01FF;
		softwareRenderer->bg[1].x = value;
		break;
	case REG_BG1VOFS:
		value &= 0x01FF;
		softwareRenderer->bg[1].y = value;
		break;
	case REG_BG2HOFS:
		value &= 0x01FF;
		softwareRenderer->bg[2].x = value;
		break;
	case REG_BG2VOFS:
		value &= 0x01FF;
		softwareRenderer->bg[2].y = value;
		break;
	case REG_BG3HOFS:
		value &= 0x01FF;
		softwareRenderer->bg[3].x = value;
		break;
	case REG_BG3VOFS:
		value &= 0x01FF;
		softwareRenderer->bg[3].y = value;
		break;
	case REG_BLDCNT:
		GBAVideoSoftwareRendererWriteBLDCNT(softwareRenderer, value);
		break;
	case REG_BLDALPHA:
		softwareRenderer->blda = value & 0x1F;
		if (softwareRenderer->blda > 0x10) {
			softwareRenderer->blda = 0x10;
		}
		softwareRenderer->bldb = (value >> 8) & 0x1F;
		if (softwareRenderer->bldb > 0x10) {
			softwareRenderer->bldb = 0x10;
		}
		break;
	case REG_BLDY:
		softwareRenderer->bldy = value & 0x1F;
		if (softwareRenderer->bldy > 0x10) {
			softwareRenderer->bldy = 0x10;
		}
		_updatePalettes(softwareRenderer);
		break;
	default:
		GBALog(GBA_LOG_STUB, "Stub video register write: %03x", address);
	}
	return value;
}

static void GBAVideoSoftwareRendererWritePalette(struct GBAVideoRenderer* renderer, uint32_t address, uint16_t value) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	uint32_t color32 = 0;
	color32 |= (value << 3) & 0xF8;
	color32 |= (value << 6) & 0xF800;
	color32 |= (value << 9) & 0xF80000;
	softwareRenderer->normalPalette[address >> 1] = color32;
}

static void GBAVideoSoftwareRendererDrawScanline(struct GBAVideoRenderer* renderer, int y) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	uint32_t* row = &softwareRenderer->outputBuffer[softwareRenderer->outputBufferStride * y];
	if (softwareRenderer->dispcnt.forcedBlank) {
		for (int x = 0; x < VIDEO_HORIZONTAL_PIXELS; ++x) {
			row[x] = GBA_COLOR_WHITE;
		}
		return;
	}

	memset(softwareRenderer->flags, 0, sizeof(softwareRenderer->flags));
	softwareRenderer->row = row;

	softwareRenderer->start = 0;
	softwareRenderer->end = VIDEO_HORIZONTAL_PIXELS;
	_drawScanline(softwareRenderer, y);
}

static void GBAVideoSoftwareRendererFinishFrame(struct GBAVideoRenderer* renderer) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;

	pthread_mutex_lock(&softwareRenderer->mutex);
	renderer->framesPending++;
	pthread_cond_broadcast(&softwareRenderer->upCond);
	if (!renderer->turbo) {
		pthread_cond_wait(&softwareRenderer->downCond, &softwareRenderer->mutex);
	}
	pthread_mutex_unlock(&softwareRenderer->mutex);
}

static void GBAVideoSoftwareRendererUpdateDISPCNT(struct GBAVideoSoftwareRenderer* renderer) {
	renderer->bg[0].enabled = renderer->dispcnt.bg0Enable;
	renderer->bg[1].enabled = renderer->dispcnt.bg1Enable;
	renderer->bg[2].enabled = renderer->dispcnt.bg2Enable;
	renderer->bg[3].enabled = renderer->dispcnt.bg3Enable;
}

static void GBAVideoSoftwareRendererWriteBGCNT(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	union GBARegisterBGCNT reg = { .packed = value };
	bg->priority = reg.priority;
	bg->charBase = reg.charBase << 14;
	bg->mosaic = reg.mosaic;
	bg->multipalette = reg.multipalette;
	bg->screenBase = reg.screenBase << 11;
	bg->overflow = reg.overflow;
	bg->size = reg.size;

	_sortBackgrounds(renderer);
}

static void GBAVideoSoftwareRendererWriteBLDCNT(struct GBAVideoSoftwareRenderer* renderer, uint16_t value) {
	union {
		struct {
			unsigned target1Bg0 : 1;
			unsigned target1Bg1 : 1;
			unsigned target1Bg2 : 1;
			unsigned target1Bg3 : 1;
			unsigned target1Obj : 1;
			unsigned target1Bd : 1;
			enum BlendEffect effect : 2;
			unsigned target2Bg0 : 1;
			unsigned target2Bg1 : 1;
			unsigned target2Bg2 : 1;
			unsigned target2Bg3 : 1;
			unsigned target2Obj : 1;
			unsigned target2Bd : 1;
		};
		uint16_t packed;
	} bldcnt = { .packed = value };

	enum BlendEffect oldEffect = renderer->blendEffect;

	renderer->bg[0].target1 = bldcnt.target1Bg0;
	renderer->bg[1].target1 = bldcnt.target1Bg1;
	renderer->bg[2].target1 = bldcnt.target1Bg2;
	renderer->bg[3].target1 = bldcnt.target1Bg3;
	renderer->bg[0].target2 = bldcnt.target2Bg0;
	renderer->bg[1].target2 = bldcnt.target2Bg1;
	renderer->bg[2].target2 = bldcnt.target2Bg2;
	renderer->bg[3].target2 = bldcnt.target2Bg3;

	renderer->blendEffect = bldcnt.effect;
	renderer->target1Obj = bldcnt.target1Obj;
	renderer->target1Bd = bldcnt.target1Bd;
	renderer->target2Obj = bldcnt.target2Obj;
	renderer->target2Bd = bldcnt.target2Bd;

	if (oldEffect != renderer->blendEffect) {
		_updatePalettes(renderer);
	}
}

static void _drawScanline(struct GBAVideoSoftwareRenderer* renderer, int y) {
	int i;
	if (renderer->dispcnt.objEnable) {
		for (i = 0; i < 128; ++i) {
			struct GBAObj* sprite = &renderer->d.oam->obj[i];
			if (sprite->transformed) {
				_drawTransformedSprite(renderer, &renderer->d.oam->tobj[i], y);
			} else if (!sprite->disable) {
				_drawSprite(renderer, sprite, y);
			}
		}
	}

	for (i = 0; i < 4; ++i) {
		if (renderer->sortedBg[i]->enabled) {
			_drawBackgroundMode0(renderer, renderer->sortedBg[i], y);
		}
	}
}

#define BACKGROUND_DRAW_PIXEL_16 \
	if (tileData & 0xF) { \
		renderer->row[outX] = renderer->normalPalette[(tileData & 0xF) | (mapData.palette << 4)]; \
	} \
	tileData >>= 4;

#define BACKGROUND_TEXT_SELECT_CHARACTER \
	localX = tileX * 8 + inX; \
	xBase = localX & 0xF8; \
	if (background->size & 1) { \
		xBase += (localX & 0x100) << 5; \
	} \
	screenBase = (background->screenBase >> 1) + (xBase >> 3) + (yBase << 2); \
	mapData.packed = renderer->d.vram[screenBase]; \
	if (!mapData.vflip) { \
		localY = inY & 0x7; \
	} else { \
		localY = 7 - (inY & 0x7); \
	} \

static void _drawBackgroundMode0(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int y) {
	int start = renderer->start;
	int end = renderer->end;
	int inX = start + background->x;
	int inY = y + background->y;
	union GBATextMapData mapData;

	unsigned yBase = inY & 0xF8;
	if (background->size & 2) {
		yBase += inY & 0x100;
	} else if (background->size == 3) {
		yBase += (inY & 0x100) << 1;
	}

	int localX;
	int localY;

	unsigned xBase;

	uint32_t screenBase;
	uint32_t charBase;

	int outX = 0;
	int tileX = 0;
	if (inX & 0x7) {
		int end = 0x8 - (inX & 0x7);
		uint32_t tileData;
		BACKGROUND_TEXT_SELECT_CHARACTER;
		charBase = ((background->charBase + (mapData.tile << 5)) >> 2) + localY;
		tileData = ((uint32_t*)renderer->d.vram)[charBase];
		tileData >>= 4 * (inX & 0x7);
		for (outX = 0; outX < end; ++outX) {
			BACKGROUND_DRAW_PIXEL_16;
		}

		tileX = 30;
		BACKGROUND_TEXT_SELECT_CHARACTER;
		charBase = ((background->charBase + (mapData.tile << 5)) >> 2) + localY;
		tileData = ((uint32_t*)renderer->d.vram)[charBase];
		for (outX = VIDEO_HORIZONTAL_PIXELS - 8 + end; outX < VIDEO_HORIZONTAL_PIXELS; ++outX) {
			BACKGROUND_DRAW_PIXEL_16;
		}

		tileX = 1;
		outX = end;
	}

	for (tileX; tileX < 30; ++tileX) {
		BACKGROUND_TEXT_SELECT_CHARACTER;
		charBase = ((background->charBase + (mapData.tile << 5)) >> 2) + localY;
		uint32_t tileData = ((uint32_t*)renderer->d.vram)[charBase];
		if (tileData) {
			BACKGROUND_DRAW_PIXEL_16;
			++outX;
			BACKGROUND_DRAW_PIXEL_16;
			++outX;
			BACKGROUND_DRAW_PIXEL_16;
			++outX;
			BACKGROUND_DRAW_PIXEL_16;
			++outX;
			BACKGROUND_DRAW_PIXEL_16;
			++outX;
			BACKGROUND_DRAW_PIXEL_16;
			++outX;
			BACKGROUND_DRAW_PIXEL_16;
			++outX;
			BACKGROUND_DRAW_PIXEL_16;
			++outX;
		} else {
			outX += 8;
		}
	}
}

static const int _objSizes[32] = {
	8, 8,
	16, 16,
	32, 32,
	64, 64,
	16, 8,
	32, 8,
	32, 16,
	64, 32,
	8, 16,
	8, 32,
	16, 32,
	32, 64,
	0, 0,
	0, 0,
	0, 0,
	0, 0
};

static void _drawSprite(struct GBAVideoSoftwareRenderer* renderer, struct GBAObj* sprite, int y) {
	int width = _objSizes[sprite->shape * 8 + sprite->size * 2];
	int height = _objSizes[sprite->shape * 8 + sprite->size * 2 + 1];
	int start = renderer->start;
	int end = renderer->end;
	if ((y < sprite->y && (sprite->y + height - 256 < 0 || y >= sprite->y + height - 256)) || y >= sprite->y + height) {
		return;
	}
	struct PixelFlags flags = {
		.priority = sprite->priority,
		.isSprite = 1,
		.target1 = (renderer->target1Obj && renderer->blendEffect == BLEND_ALPHA) || sprite->mode == OBJ_MODE_SEMITRANSPARENT,
		.target2 = renderer->target2Obj
	};
	int x = sprite->x;
	int inY = y - sprite->y;
	if (sprite->y + height - 256 >= 0) {
		inY += 256;
	}
	if (sprite->vflip) {
		inY = height - inY - 1;
	}
	unsigned charBase = BASE_TILE + sprite->tile * 0x20;
	unsigned yBase = (inY & ~0x7) * (renderer->dispcnt.objCharacterMapping ? width >> 1 : 0x80) + (inY & 0x7) * 4;
	for (int outX = x >= start ? x : start; outX < x + width && outX < end; ++outX) {
		int inX = outX - x;
		if (sprite->hflip) {
			inX = width - inX - 1;
		}
		if (renderer->flags[outX].isSprite) {
			continue;
		}
		unsigned xBase = (inX & ~0x7) * 4 + ((inX >> 1) & 2);
		uint16_t tileData = renderer->d.vram[(yBase + charBase + xBase) >> 1];
		tileData = (tileData >> ((inX & 3) << 2)) & 0xF;
		if (tileData) {
			if (!renderer->target1Obj) {
				renderer->spriteLayer[outX] = renderer->normalPalette[0x100 | tileData | (sprite->palette << 4)];
			} else {
				renderer->spriteLayer[outX] = renderer->variantPalette[0x100 | tileData | (sprite->palette << 4)];
			}
			renderer->flags[outX] = flags;
		}
	}
}

static void _drawTransformedSprite(struct GBAVideoSoftwareRenderer* renderer, struct GBATransformedObj* sprite, int y) {
	int width = _objSizes[sprite->shape * 8 + sprite->size * 2];
	int totalWidth = width << sprite->doublesize;
	int height = _objSizes[sprite->shape * 8 + sprite->size * 2 + 1];
	int totalHeight = height << sprite->doublesize;
	int start = renderer->start;
	int end = renderer->end;
	if ((y < sprite->y && (sprite->y + totalHeight - 256 < 0 || y >= sprite->y + totalHeight - 256)) || y >= sprite->y + totalHeight) {
		return;
	}
	struct PixelFlags flags = {
		.priority = sprite->priority,
		.isSprite = 1,
		.target1 = (renderer->target1Obj && renderer->blendEffect == BLEND_ALPHA) || sprite->mode == OBJ_MODE_SEMITRANSPARENT,
		.target2 = renderer->target2Obj
	};
	int x = sprite->x;
	unsigned charBase = BASE_TILE + sprite->tile * 0x20;
	struct GBAOAMMatrix* mat = &renderer->d.oam->mat[sprite->matIndex];
	for (int outX = x >= start ? x : start; outX < x + totalWidth && outX < end; ++outX) {
		if (renderer->flags[outX].isSprite) {
			continue;
		}
		int inY = y - sprite->y;
		int inX = outX - x;
		int localX = ((mat->a * (inX - (totalWidth >> 1)) + mat->b * (inY - (totalHeight >> 1))) >> 8) + (width >> 1);
		int localY = ((mat->c * (inX - (totalWidth >> 1)) + mat->d * (inY - (totalHeight >> 1))) >> 8) + (height >> 1);

		if (localX < 0 || localX >= width || localY < 0 || localY >= height) {
			continue;
		}

		unsigned yBase = (localY & ~0x7) * (renderer->dispcnt.objCharacterMapping ? width >> 1 : 0x80) + (localY & 0x7) * 4;
		unsigned xBase = (localX & ~0x7) * 4 + ((localX >> 1) & 2);
		uint16_t tileData = renderer->d.vram[(yBase + charBase + xBase) >> 1];
		tileData = (tileData >> ((localX & 3) << 2)) & 0xF;
		if (tileData) {
			if (!renderer->target1Obj) {
				renderer->spriteLayer[outX] = renderer->normalPalette[0x100 | tileData | (sprite->palette << 4)];
			} else {
				renderer->spriteLayer[outX] = renderer->variantPalette[0x100 | tileData | (sprite->palette << 4)];
			}
			renderer->flags[outX] = flags;
		}
	}
}

static void _updatePalettes(struct GBAVideoSoftwareRenderer* renderer) {
	if (renderer->blendEffect == BLEND_BRIGHTEN) {
		for (int i = 0; i < 512; ++i) {
			renderer->variantPalette[i] = _brighten(renderer->normalPalette[i], renderer->bldy);
		}
	} else if (renderer->blendEffect == BLEND_DARKEN) {
		for (int i = 0; i < 512; ++i) {
			renderer->variantPalette[i] = _darken(renderer->normalPalette[i], renderer->bldy);
		}
	} else {
		for (int i = 0; i < 512; ++i) {
			renderer->variantPalette[i] = renderer->normalPalette[i];
		}
	}
}

static inline uint32_t _brighten(uint32_t color, int y) {
	uint32_t c = 0;
	uint32_t a;
	a = color & 0xF8;
	c |= (a + ((0xF8 - a) * y) / 16) & 0xF8;

	a = color & 0xF800;
	c |= (a + ((0xF800 - a) * y) / 16) & 0xF800;

	a = color & 0xF80000;
	c |= (a + ((0xF80000 - a) * y) / 16) & 0xF80000;
	return c;
}

static inline uint32_t _darken(uint32_t color, int y) {
	uint32_t c = 0;
	uint32_t a;
	a = color & 0xF8;
	c |= (a - (a * y) / 16) & 0xF8;

	a = color & 0xF800;
	c |= (a - (a * y) / 16) & 0xF800;

	a = color & 0xF80000;
	c |= (a - (a * y) / 16) & 0xF80000;
	return c;
}

static uint32_t _mix(int weightA, uint32_t colorA, int weightB, uint32_t colorB) {
	uint32_t c = 0;
	uint32_t a, b;
	a = colorA & 0xF8;
	b = colorB & 0xF8;
	c |= ((a * weightA + b * weightB) / 16) & 0x1F8;
	if (c & 0x00000100) {
		c = 0x000000F8;
	}

	a = colorA & 0xF800;
	b = colorB & 0xF800;
	c |= ((a * weightA + b * weightB) / 16) & 0x1F800;
	if (c & 0x00010000) {
		c = (c & 0x000000F8) | 0x0000F800;
	}

	a = colorA & 0xF80000;
	b = colorB & 0xF80000;
	c |= ((a * weightA + b * weightB) / 16) & 0x1F80000;
	if (c & 0x01000000) {
		c = (c & 0x0000F8F8) | 0x00F80000;
	}
	return c;
}

static void _sortBackgrounds(struct GBAVideoSoftwareRenderer* renderer) {
	qsort(renderer->sortedBg, 4, sizeof(struct GBAVideoSoftwareBackground*), _backgroundComparator);
}

static int _backgroundComparator(const void* a, const void* b) {
	const struct GBAVideoSoftwareBackground* bgA = *(const struct GBAVideoSoftwareBackground**) a;
	const struct GBAVideoSoftwareBackground* bgB = *(const struct GBAVideoSoftwareBackground**) b;
	if (bgA->priority != bgB->priority) {
		return bgA->priority - bgB->priority;
	} else {
		return bgA->index - bgB->index;
	}
}
