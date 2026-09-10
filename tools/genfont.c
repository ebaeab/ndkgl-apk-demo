/*
 * genfont.c —— 位图字体生成器(仅在 Termux 主机上运行, 不进入 APK)
 *
 * 用途: 读取 /system/fonts 下的字体, 把 jni/ui_strings.h 里出现过的
 *       所有 ASCII(0x20..0x7E)与 CJK 字符光栅化成 8bit 灰度位图,
 *       输出 jni/font_bitmap.h 供 logview_ui.c 内嵌使用。
 *
 * 用法:
 *   clang -O2 -o /tmp/genfont tools/genfont.c \
 *         -I$PREFIX/include/freetype2 -L$PREFIX/lib -lfreetype
 *   /tmp/genfont > jni/font_bitmap.h
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "../jni/ui_strings.h"

/* 覆盖所有出现过的文案; genfont 会从中提取唯一字符 */
static const char* const ALL_STRINGS[] = {
    S_BTN_OPEN, S_BTN_REFRESH, S_BTN_PARSE, S_BTN_STOP, S_BTN_SEARCH, S_BTN_HELP,
    S_TITLE_LOG, S_TITLE_SEARCH,
    S_STATUS_PREFIX, S_STATUS_READY, S_STATUS_LOADED, S_STATUS_RUN, S_STATUS_STOP,
    S_EMPTY_LOG, S_NO_MATCH, S_SEARCH_HINT, S_MATCH_FMT,
    S_HELP_TITLE, S_HELP_L1, S_HELP_L2, S_HELP_L3, S_HELP_L4, S_HELP_L5, S_HELP_L6, S_HELP_L7,
    NULL
};

#define ASCII_FONT "/system/fonts/DroidSansMono.ttf"
#define CJK_FONT   "/system/fonts/NotoSansCJK-Regular.ttc"

#define ASC_W 10
#define ASC_H 16
#define CJK_W 16
#define CJK_H 16
#define MAX_CJK 512

/* 收集到的唯一 CJK 码点(按 UTF-8 解码) */
static unsigned int gCjk[MAX_CJK];
static int gCjkCount = 0;

/* ---- UTF-8 解码: 返回码点并推进 p ---- */
static unsigned int utf8_next(const char** p) {
    const unsigned char* s = (const unsigned char*)*p;
    unsigned int cp;
    if (s[0] < 0x80) { cp = s[0]; *p += 1; return cp; }
    if ((s[0] & 0xE0) == 0xC0) { cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F); *p += 2; return cp; }
    if ((s[0] & 0xF0) == 0xE0) {
        cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        *p += 3; return cp;
    }
    cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    *p += 4; return cp;
}

static int cjk_seen(unsigned int cp) {
    for (int i = 0; i < gCjkCount; i++) if (gCjk[i] == cp) return 1;
    return 0;
}

static void collect(void) {
    for (int i = 0; ALL_STRINGS[i]; i++) {
        const char* p = ALL_STRINGS[i];
        while (*p) {
            unsigned int cp = utf8_next(&p);
            if (cp >= 0x80 && !cjk_seen(cp) && gCjkCount < MAX_CJK)
                gCjk[gCjkCount++] = cp;
        }
    }
}

/* 选择 .ttc 中简中文 face: 优先 family 含 "SC", 其次含 "CJK", 否则 0 */
static int pick_sc_face(FT_Library lib) {
    FT_Face f;
    int fallback = -1;
    for (int i = 0; i < 16; i++) {
        if (FT_New_Face(lib, CJK_FONT, i, &f) != 0) break;
        if (f->family_name) {
            if (strstr(f->family_name, "SC")) { FT_Done_Face(f); return i; }
            if (fallback < 0 && strstr(f->family_name, "CJK")) fallback = i;
        }
        FT_Done_Face(f);
    }
    return fallback < 0 ? 0 : fallback;
}

/* 把一个 FT 灰度位图按基线/水平 bearing 塞进 w x h 的 8bit cell 里 */
static void blit(FT_Bitmap* bmp, unsigned char* cell, int w, int h, int baseline,
                 int left, int top) {
    memset(cell, 0, w * h);
    int bw = (int)bmp->width;
    int bh = (int)bmp->rows;
    if (bw > w) bw = w;
    if (bh > h) bh = h;
    int ox = left;                         /* 水平 bearing, 让字母左右对齐 */
    if (ox < 0) ox = 0;
    if (ox + bw > w) ox = w - bw;
    if (ox < 0) ox = 0;
    int oy = baseline - top;               /* 基线对齐: top 是基线到字形顶的距离 */
    for (int y = 0; y < bh; y++) {
        int dsty = oy + y;
        if (dsty < 0 || dsty >= h) continue;
        const unsigned char* src = bmp->buffer + y * bmp->pitch;
        unsigned char* dst = cell + dsty * w + ox;
        if (bmp->pixel_mode == FT_PIXEL_MODE_MONO) {
            for (int x = 0; x < bw; x++)
                dst[x] = (src[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0;
        } else { /* FT_PIXEL_MODE_GRAY */
            for (int x = 0; x < bw; x++) dst[x] = src[x];
        }
    }
}

int main(void) {
    collect();

    FT_Library lib;
    if (FT_Init_FreeType(&lib)) { fprintf(stderr, "FT_Init failed\n"); return 1; }

    /* ---- ASCII ---- */
    FT_Face af;
    if (FT_New_Face(lib, ASCII_FONT, 0, &af)) { fprintf(stderr, "open ascii font failed\n"); return 1; }
    FT_Set_Pixel_Sizes(af, ASC_W, ASC_H - 1);  /* 字号比 cell 小 1px, 使 ascender+descender 正好塞进 16px */
    /* 基线 = 所有字形里最大的 bitmap_top(即最高的字形顶部), 这样同字体共用同一条基线 */
    int ascBaseline = 0;
    for (int c = 0x20; c <= 0x7E; c++) {
        FT_Load_Char(af, c, FT_LOAD_RENDER);
        if (af->glyph->bitmap_top > ascBaseline) ascBaseline = af->glyph->bitmap_top;
    }
    if (ascBaseline > ASC_H) ascBaseline = ASC_H;

    /* ---- CJK ---- */
    FT_Face cf;
    int sc = pick_sc_face(lib);
    if (FT_New_Face(lib, CJK_FONT, sc, &cf)) { fprintf(stderr, "open cjk font failed\n"); return 1; }
    FT_Set_Pixel_Sizes(cf, 0, CJK_H);
    int cjkBaseline = 0;
    for (int i = 0; i < gCjkCount; i++) {
        FT_Load_Char(cf, gCjk[i], FT_LOAD_RENDER);
        if (cf->glyph->bitmap_top > cjkBaseline) cjkBaseline = cf->glyph->bitmap_top;
    }
    if (cjkBaseline > CJK_H) cjkBaseline = CJK_H;
    fprintf(stderr, "ascii=%s baseline=%d  cjk face %d (%s) baseline=%d  cjk chars=%d\n",
            af->family_name, ascBaseline, sc, cf->family_name, cjkBaseline, gCjkCount);

    FILE* out = stdout;
    fprintf(out,
        "/*\n * font_bitmap.h —— 由 tools/genfont.c 自动生成, 勿手动修改\n"
        " * ASCII: %dx%d 灰度  DroidSansMono\n"
        " * CJK:   %dx%d 灰度 NotoSansCJK (Simplified Chinese)\n"
        " */\n"
        "#ifndef FONT_BITMAP_H\n#define FONT_BITMAP_H\n\n"
        "#define FONT_ASC_W %d\n#define FONT_ASC_H %d\n"
        "#define FONT_CJK_W %d\n#define FONT_CJK_H %d\n"
        "#define FONT_ASC_COUNT 95\n"
        "#define FONT_CJK_COUNT ",
        ASC_W, ASC_H, CJK_W, CJK_H, ASC_W, ASC_H, CJK_W, CJK_H);
    fprintf(out, "%d\n\n", gCjkCount);

    /* ASCII 数组 */
    fputs("static const unsigned char FONT_ASC_BMP[FONT_ASC_COUNT][FONT_ASC_W*FONT_ASC_H] = {\n", out);
    unsigned char cell[ASC_W * ASC_H];
    for (int c = 0x20; c <= 0x7E; c++) {
        FT_Load_Char(af, c, FT_LOAD_RENDER);
        blit(&af->glyph->bitmap, cell, ASC_W, ASC_H, ascBaseline,
             af->glyph->bitmap_left, af->glyph->bitmap_top);
        fputs("  {", out);
        for (int i = 0; i < ASC_W * ASC_H; i++) { fprintf(out, "%d,", cell[i]); }
        fputs("},\n", out);
    }
    fputs("};\n\n", out);

    /* CJK 码点数组 */
    fputs("static const unsigned int FONT_CJK_CHARS[FONT_CJK_COUNT] = {\n  ", out);
    for (int i = 0; i < gCjkCount; i++)
        fprintf(out, "0x%04X,%s", gCjk[i], (i % 8 == 7) ? "\n  " : "");
    fputs("\n};\n\n", out);

    /* CJK 位图数组 */
    fputs("static const unsigned char FONT_CJK_BMP[FONT_CJK_COUNT][FONT_CJK_W*FONT_CJK_H] = {\n", out);
    unsigned char ccell[CJK_W * CJK_H];
    for (int i = 0; i < gCjkCount; i++) {
        FT_Load_Char(cf, gCjk[i], FT_LOAD_RENDER);
        blit(&cf->glyph->bitmap, ccell, CJK_W, CJK_H, cjkBaseline,
             cf->glyph->bitmap_left, cf->glyph->bitmap_top);
        fputs("  {", out);
        for (int j = 0; j < CJK_W * CJK_H; j++) { fprintf(out, "%d,", ccell[j]); }
        fputs("},\n", out);
    }
    fputs("};\n\n#endif /* FONT_BITMAP_H */\n", out);

    FT_Done_Face(af); FT_Done_Face(cf); FT_Done_FreeType(lib);
    fprintf(stderr, "done -> jni/font_bitmap.h\n");
    return 0;
}
