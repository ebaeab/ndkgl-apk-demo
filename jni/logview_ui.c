/*
 * logview_ui.c —— 原生 OpenGL ES2 日志查看器 UI
 *
 * EGL/线程/swap 交给 GLSurfaceView, native 只负责绘制与交互。
 * 本文件实现一个带「工具栏 + 日志显示子窗口 + 搜索关键字子窗口」的工具界面:
 *
 *   工具栏:  [打开] [刷新] [运行解析] [停止] [搜索] [帮助]
 *   日志窗口: 白底黑字, 按 I/W/E 等级着色, 超宽自动换行, 可滚动
 *   搜索窗口: 显示当前关键字与匹配行数(关键字由 Java 侧 EditText 输入)
 *   帮助面板: 点击 [帮助] 弹出说明
 *
 * 文件内容: 整个文本读入 gText, 建立行索引(gLineOff/gLineLen), 行数不限(动态增长)。
 *   [打开] 通过 SAF 选文件后显示开头; [刷新] 重读同一文件并跳到末尾(tail)。
 *
 * 文字渲染: 内嵌位图字体(jni/font_bitmap.h, 由 tools/genfont.c 生成)。
 * 加载: System.loadLibrary("logview")  ->  liblogview.so
 */
#include <jni.h>
#include <android/log.h>
#include <GLES2/gl2.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>

#include "ui_strings.h"
#include "font_bitmap.h"

#define TAG "LOGVIEW"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define MAX_BYTES (50 * 1024 * 1024)   /* 文件大小上限 50MB */

/* ================= 全局状态 ================= */
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static int gW = 1, gH = 1;
static int gInited = 0;

#define BTN_OPEN    0
#define BTN_REFRESH 1
#define BTN_PARSE   2
#define BTN_STOP    3
#define BTN_SEARCH  4
#define BTN_HELP    5
#define BTN_COUNT   6
static const char* const BTN_LABELS[BTN_COUNT] = {
    S_BTN_OPEN, S_BTN_REFRESH, S_BTN_PARSE, S_BTN_STOP, S_BTN_SEARCH, S_BTN_HELP
};

static int gRunning = 0;      /* 是否正在“运行解析”(逐行流入) */
static int gHelpOpen = 0;     /* 帮助面板 */
static int gSearchFocus = 0;  /* 搜索窗口高亮 */
static char gKeyword[64] = "";

/* ---- 文件内容(动态, 行数不限) ---- */
static char*  gText = NULL;       /* 整个文件文本(UTF-8, 0 结尾) */
static size_t gTextLen = 0;
static int*   gLineOff = NULL;    /* 每行起始偏移 */
static int*   gLineLen = NULL;    /* 每行长度(不含 \n / \r) */
static int    gLineCount = 0;
static int    gLineCap = 0;

/* ---- 解析/显示 ---- */
static int gDisplayed = 0;        /* 已“流入”显示的行数(解析动画进度) */
static int gScroll = 0;           /* 上滚行数(按源行计, 0=末尾) */

/* ---- 过滤匹配 ---- */
static int* gMatchIdx = NULL;
static int  gMatchCount = 0;
static int  gMatchCap = 0;
static int  gFilterDirty = 1;

static const char* const STATUS_TEXTS[] = {
    S_STATUS_READY, S_STATUS_LOADED, S_STATUS_RUN, S_STATUS_STOP
};
static int gStatus = 0;       /* 0 就绪 1 已载入 2 解析中 3 已停止 */

/* 触摸状态 */
static int gDownX = 0, gDownY = 0, gDownBtn = -1, gDownInLog = 0, gLastY = 0;
static int gScrollDrag = 0;   /* 正在拖动滚动条 */

/* ================= GL 对象 ================= */
static GLuint gProgSolid = 0, gProgTex = 0;
static GLint  gUSolidHalf = -1;
static GLint  gASolidP = -1, gASolidC = -1;
static GLint  gUTexHalf = -1, gUTexSampler = -1;
static GLint  gATexP = -1, gATexUV = -1, gATexC = -1;
static GLuint gTexAsc = 0, gTexCjk = 0;

/* JNI 回调 */
static jclass    gCls = NULL;
static jmethodID gReqFocus = NULL;     /* 搜索 -> 弹软键盘 */
static jmethodID gReqOpenFile = NULL;  /* 打开 -> 系统文件选择器 */
static jmethodID gReqRefreshFile = NULL;/* 刷新 -> 重读文件 */

/* ================= 绘制批次缓冲 ================= */
#define MAX_RECTS 256
#define MAX_TEXT  8192
static float gRectV[MAX_RECTS * 36];   /* 每矩形 6 顶点 * (x,y,r,g,b,a) */
static int   gRectN = 0;
static float gAscV[MAX_TEXT * 48];     /* 每字形 6 顶点 * (x,y,u,v,r,g,b,a) */
static int   gAscN = 0;
static float gCjkV[MAX_TEXT * 48];
static int   gCjkN = 0;

/* ================= 布局 ================= */
typedef struct {
    float S, margin;
    float toolbarY, toolbarH, statusY, statusH;
    float logX, logY, logW, logH;
    float searchX, searchY, searchW, searchH;
    float btnX[BTN_COUNT], btnY, btnW, btnH;
    float lineH, titlePad;
} Layout;

static void computeLayout(Layout* L) {
    float W = (float)gW, H = (float)gH;
    L->S = H / 720.0f;
    if (L->S < 0.8f)  L->S = 0.8f;
    if (L->S > 2.5f)  L->S = 2.5f;

    L->margin    = 8.0f * L->S;
    L->toolbarH  = 52.0f * L->S;
    L->toolbarY  = L->margin;
    L->statusH   = 18.0f * L->S;
    L->statusY   = L->toolbarY + L->toolbarH + 4.0f * L->S;
    L->searchH   = 44.0f * L->S;
    L->searchY   = H - L->margin - L->searchH;
    L->logX      = L->margin;
    L->logY      = L->statusY + L->statusH + 6.0f * L->S;
    L->logW      = W - 2.0f * L->margin;
    L->logH      = L->searchY - 6.0f * L->S - L->logY;
    if (L->logH < 40.0f * L->S) L->logH = 40.0f * L->S;
    L->searchX   = L->margin;
    L->searchW   = W - 2.0f * L->margin;

    float gap = 4.0f * L->S;
    L->btnW = (W - 2.0f * L->margin - (float)(BTN_COUNT - 1) * gap) / (float)BTN_COUNT;
    L->btnH = 38.0f * L->S;
    L->btnY = L->toolbarY + (L->toolbarH - L->btnH) / 2.0f;
    for (int i = 0; i < BTN_COUNT; i++)
        L->btnX[i] = L->margin + (float)i * (L->btnW + gap);

    L->lineH    = 18.0f * L->S;
    L->titlePad = 26.0f * L->S;
}

/* ================= 着色器 ================= */
static GLuint compileShader(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; log[0] = 0;
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        LOGE("shader compile failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint buildProgram(const char* vs, const char* fs) {
    GLuint v = compileShader(GL_VERTEX_SHADER, vs);
    GLuint f = compileShader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; log[0] = 0;
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        LOGE("program link failed: %s", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static const char* SOLID_VS =
    "attribute vec2 aP; attribute vec4 aC; varying vec4 vC; uniform vec2 uHalf;\n"
    "void main(){ gl_Position = vec4(aP.x/uHalf.x - 1.0, 1.0 - aP.y/uHalf.y, 0.0, 1.0); vC = aC; }\n";
static const char* SOLID_FS =
    "precision mediump float; varying vec4 vC;\n"
    "void main(){ gl_FragColor = vC; }\n";
static const char* TEX_VS =
    "attribute vec2 aP; attribute vec2 aUV; attribute vec4 aC;\n"
    "varying vec2 vUV; varying vec4 vC; uniform vec2 uHalf;\n"
    "void main(){ gl_Position = vec4(aP.x/uHalf.x - 1.0, 1.0 - aP.y/uHalf.y, 0.0, 1.0); vUV = aUV; vC = aC; }\n";
static const char* TEX_FS =
    "precision mediump float; varying vec2 vUV; varying vec4 vC; uniform sampler2D uTex;\n"
    "void main(){ gl_FragColor = vec4(vC.rgb, vC.a * texture2D(uTex, vUV).a); }\n";

/* ================= 位图字体 -> 纹理 ================= */
static GLuint makeFontTexture(const unsigned char* data, int count,
                              int cellW, int cellH, int cols, int* outW, int* outH) {
    int rows = (count + cols - 1) / cols;
    int atlasW = cols * cellW;
    int atlasH = rows * cellH;
    unsigned char* buf = (unsigned char*)calloc(1, (size_t)atlasW * atlasH);
    if (!buf) return 0;
    for (int i = 0; i < count; i++) {
        int c = i % cols, r = i / cols;
        const unsigned char* src = data + (size_t)i * cellW * cellH;
        for (int y = 0; y < cellH; y++) {
            unsigned char* dst = buf + ((size_t)(r * cellH + y) * atlasW) + c * cellW;
            memcpy(dst, src + (size_t)y * cellW, cellW);
        }
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, atlasW, atlasH, 0,
                 GL_ALPHA, GL_UNSIGNED_BYTE, buf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(buf);
    if (outW) *outW = atlasW;
    if (outH) *outH = atlasH;
    return tex;
}

static int gAscW = 160, gAscH = 96;
static int gCjkW = 256, gCjkH = 80;

/* ================= 小工具 ================= */
static unsigned int utf8Next(const char** p) {
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

static int cjkIndex(unsigned int cp) {
    for (int i = 0; i < FONT_CJK_COUNT; i++)
        if (FONT_CJK_CHARS[i] == cp) return i;
    return -1;
}

static float textWidth(const char* s, float scale) {
    float w = 0; const char* p = s;
    while (*p) {
        unsigned int cp = utf8Next(&p);
        w += (cp < 0x80 ? FONT_ASC_W : FONT_CJK_W) * scale;
    }
    return w;
}

/* 大小写不敏感(仅 ASCII 字母)的长度感知子串匹配 */
static int containsNoCase(const char* hay, size_t haylen, const char* needle) {
    if (!needle || !needle[0]) return 1;
    size_t n = strlen(needle);
    if (haylen < n) return 0;
    for (size_t i = 0; i + n <= haylen; i++) {
        size_t k = 0;
        for (; k < n; k++) {
            char ca = hay[i + k], cb = needle[k];
            if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
            if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
            if (ca != cb) break;
        }
        if (k == n) return 1;
    }
    return 0;
}

static int memfind(const char* hay, int haylen, const char* needle) {
    size_t n = strlen(needle);
    if (haylen < (int)n) return 0;
    for (int i = 0; i + (int)n <= haylen; i++)
        if (memcmp(hay + i, needle, n) == 0) return 1;
    return 0;
}

/* ================= 批次写入 ================= */
static void rectf(float x, float y, float w, float h,
                  float r, float g, float b, float a) {
    if (gRectN >= MAX_RECTS) return;
    float* v = gRectV + gRectN * 36;
    float x1 = x + w, y1 = y + h;
    float t[6][6] = {
        {x,  y,  r,g,b,a}, {x1, y,  r,g,b,a}, {x1, y1, r,g,b,a},
        {x,  y,  r,g,b,a}, {x1, y1, r,g,b,a}, {x,  y1, r,g,b,a},
    };
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 6; j++) v[i*6 + j] = t[i][j];
    gRectN++;
}

static void glyph(float* arr, int* n, float x, float y, float w, float h,
                  float u0, float v0, float u1, float v1,
                  float r, float g, float b, float a) {
    if (*n >= MAX_TEXT) return;
    float* v = arr + (*n) * 48;
    float x1 = x + w, y1 = y + h;
    float t[6][8] = {
        {x,  y,  u0, v0, r,g,b,a}, {x1, y,  u1, v0, r,g,b,a}, {x1, y1, u1, v1, r,g,b,a},
        {x,  y,  u0, v0, r,g,b,a}, {x1, y1, u1, v1, r,g,b,a}, {x,  y1, u0, v1, r,g,b,a},
    };
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 8; j++) v[i*8 + j] = t[i][j];
    (*n)++;
}

/* 画一段文字(长度限定), 自动按 ASCII/中文 分到两张纹理批次 */
static void drawTextN(float x, float y, const char* s, int len, float scale,
                      float r, float g, float b, float a) {
    const char* p = s;
    const char* end = s + len;
    while (p < end) {
        unsigned int cp = utf8Next(&p);
        if (cp < 0x80) {
            int idx = (int)cp - 0x20;
            if (idx < 0 || idx >= FONT_ASC_COUNT) { x += FONT_ASC_W * scale; continue; }
            int col = idx % 16, row = idx / 16;
            float u0 = (col * FONT_ASC_W) / (float)gAscW;
            float u1 = ((col + 1) * FONT_ASC_W) / (float)gAscW;
            float v0 = (row * FONT_ASC_H) / (float)gAscH;
            float v1 = ((row + 1) * FONT_ASC_H) / (float)gAscH;
            glyph(gAscV, &gAscN, x, y, FONT_ASC_W * scale, FONT_ASC_H * scale,
                  u0, v0, u1, v1, r, g, b, a);
            x += FONT_ASC_W * scale;
        } else {
            int idx = cjkIndex(cp);
            if (idx < 0) { x += FONT_CJK_W * scale; continue; }
            int col = idx % 16, row = idx / 16;
            float u0 = (col * FONT_CJK_W) / (float)gCjkW;
            float u1 = ((col + 1) * FONT_CJK_W) / (float)gCjkW;
            float v0 = (row * FONT_CJK_H) / (float)gCjkH;
            float v1 = ((row + 1) * FONT_CJK_H) / (float)gCjkH;
            glyph(gCjkV, &gCjkN, x, y, FONT_CJK_W * scale, FONT_CJK_H * scale,
                  u0, v0, u1, v1, r, g, b, a);
            x += FONT_CJK_W * scale;
        }
    }
}

static void drawText(float x, float y, const char* s, float scale,
                     float r, float g, float b, float a) {
    drawTextN(x, y, s, (int)strlen(s), scale, r, g, b, a);
}

/* ================= 提交绘制 ================= */
static void resetBatches(void) { gRectN = 0; gAscN = 0; gCjkN = 0; }

static void flushBatches(void) {
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (gRectN > 0) {
        glUseProgram(gProgSolid);
        glUniform2f(gUSolidHalf, gW / 2.0f, gH / 2.0f);
        glEnableVertexAttribArray(gASolidP);
        glEnableVertexAttribArray(gASolidC);
        glVertexAttribPointer(gASolidP, 2, GL_FLOAT, GL_FALSE, 6*sizeof(float), gRectV);
        glVertexAttribPointer(gASolidC, 4, GL_FLOAT, GL_FALSE, 6*sizeof(float), gRectV + 2);
        glDrawArrays(GL_TRIANGLES, 0, gRectN * 6);
        glDisableVertexAttribArray(gASolidP);
        glDisableVertexAttribArray(gASolidC);
    }

    if (gAscN > 0 || gCjkN > 0) {
        glUseProgram(gProgTex);
        glUniform2f(gUTexHalf, gW / 2.0f, gH / 2.0f);
        glUniform1i(gUTexSampler, 0);
        glEnableVertexAttribArray(gATexP);
        glEnableVertexAttribArray(gATexUV);
        glEnableVertexAttribArray(gATexC);
        if (gAscN > 0) {
            glBindTexture(GL_TEXTURE_2D, gTexAsc);
            glVertexAttribPointer(gATexP,  2, GL_FLOAT, GL_FALSE, 8*sizeof(float), gAscV);
            glVertexAttribPointer(gATexUV, 2, GL_FLOAT, GL_FALSE, 8*sizeof(float), gAscV + 2);
            glVertexAttribPointer(gATexC,  4, GL_FLOAT, GL_FALSE, 8*sizeof(float), gAscV + 4);
            glDrawArrays(GL_TRIANGLES, 0, gAscN * 6);
        }
        if (gCjkN > 0) {
            glBindTexture(GL_TEXTURE_2D, gTexCjk);
            glVertexAttribPointer(gATexP,  2, GL_FLOAT, GL_FALSE, 8*sizeof(float), gCjkV);
            glVertexAttribPointer(gATexUV, 2, GL_FLOAT, GL_FALSE, 8*sizeof(float), gCjkV + 2);
            glVertexAttribPointer(gATexC,  4, GL_FLOAT, GL_FALSE, 8*sizeof(float), gCjkV + 4);
            glDrawArrays(GL_TRIANGLES, 0, gCjkN * 6);
        }
        glDisableVertexAttribArray(gATexP);
        glDisableVertexAttribArray(gATexUV);
        glDisableVertexAttribArray(gATexC);
    }
    resetBatches();
}

/* ================= 配色 ================= */
#define CLR_BG        0.10f, 0.11f, 0.14f
#define CLR_TOOLBAR   0.15f, 0.17f, 0.21f
#define CLR_TEXT      0.88f, 0.90f, 0.93f
#define CLR_DIM       0.55f, 0.58f, 0.62f
#define CLR_HL        1.00f, 0.80f, 0.25f

static void logLineColor(const char* line, int len, float* r, float* g, float* b) {
    if (memfind(line, len, " E ")) { *r = 0.72f; *g = 0.06f; *b = 0.06f; return; }
    if (memfind(line, len, " W ")) { *r = 0.72f; *g = 0.45f; *b = 0.00f; return; }
    if (memfind(line, len, " I ")) { *r = 0.05f; *g = 0.30f; *b = 0.60f; return; }
    *r = 0.05f; *g = 0.06f; *b = 0.08f;
}

/* ================= 文件内容 / 过滤 ================= */
static void ensureLineCap(int need) {
    if (need <= gLineCap) return;
    int cap = gLineCap ? gLineCap : 4096;
    while (cap < need) cap *= 2;
    gLineOff = (int*)realloc(gLineOff, (size_t)cap * sizeof(int));
    gLineLen = (int*)realloc(gLineLen, (size_t)cap * sizeof(int));
    gLineCap = cap;
}

static void ensureMatchCap(int need) {
    if (need <= gMatchCap) return;
    int cap = gMatchCap ? gMatchCap : 4096;
    while (cap < need) cap *= 2;
    gMatchIdx = (int*)realloc(gMatchIdx, (size_t)cap * sizeof(int));
    gMatchCap = cap;
}

/* 扫描 gText 建立行索引 */
static void buildLineIndex(void) {
    gLineCount = 0;
    const char* p = gText;
    const char* end = gText + gTextLen;
    while (p < end) {
        const char* nl = (const char*)memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (len > 0 && p[len-1] == '\r') len--;   /* 去掉 CRLF 的 \r */
        ensureLineCap(gLineCount + 1);
        gLineOff[gLineCount] = (int)(p - gText);
        gLineLen[gLineCount] = (int)len;
        gLineCount++;
        if (!nl) break;
        p = nl + 1;
    }
}

static void appendMatches(int from, int to) {
    int filter = gKeyword[0] != 0;
    for (int i = from; i < to; i++) {
        if (filter && !containsNoCase(gText + gLineOff[i], (size_t)gLineLen[i], gKeyword))
            continue;
        ensureMatchCap(gMatchCount + 1);
        gMatchIdx[gMatchCount++] = i;
    }
}

static void rebuildMatches(void) {
    gMatchCount = 0;
    appendMatches(0, gDisplayed);
}

/* 载入文件内容; tail=1 跳到末尾(刷新), tail=0 显示开头(打开) */
static void applyFileContent(const char* data, size_t len, int tail) {
    free(gText); gText = NULL; gTextLen = 0;
    free(gLineOff); gLineOff = NULL;
    free(gLineLen); gLineLen = NULL;
    gLineCap = 0; gLineCount = 0;

    if (!data || len == 0) {
        gDisplayed = 0; gScroll = 0; gRunning = 0;
        gFilterDirty = 1; gStatus = 0;
        return;
    }

    gText = (char*)malloc(len + 1);
    if (!gText) return;
    memcpy(gText, data, len);
    gText[len] = 0;
    gTextLen = len;

    buildLineIndex();

    gDisplayed = gLineCount;
    gScroll = tail ? 0 : INT_MAX;   /* tail 看末尾, 否则看开头 */
    gRunning = 0;
    gStatus = 1;
    gFilterDirty = 1;
}

/* 开始“运行解析”: 清空显示后逐行流入(大文件按比例快速流入) */
static void startParse(void) {
    if (gLineCount == 0) return;
    gDisplayed = 0;
    gScroll = 0;
    gRunning = 1;
    gStatus = 2;
    gFilterDirty = 1;
}

/* ================= 日志窗口绘制(自动换行) ================= */
static float drawWrapped(float x, float y, const char* s, int len, float scale,
                         float maxW, float lineH, float bottom,
                         float r, float g, float b) {
    const char* end = s + len;
    const char* p = s;
    while (p < end && y + FONT_ASC_H * scale <= bottom) {
        const char* q = p;
        const char* segEnd = p;
        float w = 0;
        while (q < end) {
            const char* t = q;
            unsigned int cp = utf8Next(&q);
            float cw = (cp < 0x80 ? FONT_ASC_W : FONT_CJK_W) * scale;
            if (w + cw > maxW && segEnd != p) { q = t; break; }  /* 放不下且已有字符 -> 回退 */
            w += cw;
            segEnd = q;
            if (w >= maxW) break;                                /* 已填满 */
        }
        drawTextN(x, y, p, (int)(segEnd - p), scale, r, g, b, 1.0f);
        y += lineH;
        p = segEnd;
    }
    if (len == 0) y += lineH;   /* 空行也占一行, 避免行号/内容重叠 */
    return y;
}

static void drawLogWindow(Layout* L, float S, float pad) {
    /* 行号槽宽: 按总行数的位数 */
    int digits = 1, n = gLineCount;
    while (n >= 10) { n /= 10; digits++; }
    float gutterW = (digits + 1) * FONT_ASC_W * S;
    float sbW = 8.0f * S;                            /* 滚动条预留宽度 */
    float textX = L->logX + pad + gutterW;
    float maxW = L->logW - 2.0f * pad - gutterW - sbW;
    if (maxW < FONT_ASC_W * S) maxW = FONT_ASC_W * S;

    int visibleLines = (int)((L->logH - L->titlePad - 6.0f * S) / L->lineH);
    if (visibleLines < 1) visibleLines = 1;

    if (gMatchCount == 0) {
        const char* msg = gLineCount ? S_NO_MATCH : S_EMPTY_LOG;
        float tx = L->logX + (L->logW - textWidth(msg, S)) / 2.0f;
        float ty = L->logY + L->titlePad + L->lineH;
        drawText(tx, ty, msg, S, 0.45f, 0.48f, 0.52f, 1.0f);
        return;
    }

    /* 行号分隔线 */
    rectf(textX - pad * 0.6f, L->logY + L->titlePad, S,
          L->logH - L->titlePad - 4.0f * S, 0.86f, 0.87f, 0.89f, 1.0f);

    int maxScroll = gMatchCount - visibleLines;
    if (maxScroll < 0) maxScroll = 0;
    if (gScroll > maxScroll) gScroll = maxScroll;
    if (gScroll < 0) gScroll = 0;

    int start = gMatchCount - visibleLines - gScroll;
    if (start < 0) start = 0;

    float y = L->logY + L->titlePad;
    float bottom = L->logY + L->logH - 4.0f * S;
    for (int i = start; i < gMatchCount && y + L->lineH <= bottom; i++) {
        int li = gMatchIdx[i];
        /* 行号(1-based, 右对齐; 换行续行不重复显示) */
        char numbuf[16];
        snprintf(numbuf, sizeof(numbuf), "%d", li + 1);
        float nw = textWidth(numbuf, S);
        drawText(textX - FONT_ASC_W * S - nw, y, numbuf, S, 0.62f, 0.66f, 0.72f, 1.0f);
        /* 日志文本(自动换行) */
        float r, g, b;
        logLineColor(gText + gLineOff[li], gLineLen[li], &r, &g, &b);
        y = drawWrapped(textX, y, gText + gLineOff[li], gLineLen[li],
                        S, maxW, L->lineH, bottom, r, g, b);
    }

    /* 滚动条(有可滚动内容时显示) */
    if (maxScroll > 0) {
        float trackW = 3.0f * S;
        float trackX = L->logX + L->logW - pad - trackW;
        float trackY = L->logY + L->titlePad;
        float trackH = L->logH - L->titlePad - 4.0f * S;
        float thumbH = trackH * (float)visibleLines / (float)gMatchCount;
        float minThumb = 12.0f * S;
        if (thumbH < minThumb) thumbH = minThumb;
        if (thumbH > trackH) thumbH = trackH;
        float range = trackH - thumbH;
        float frac = 1.0f - (float)gScroll / (float)maxScroll;  /* 0=顶部, 1=底部 */
        float thumbY = trackY + frac * range;
        rectf(trackX, trackY, trackW, trackH, 0.90f, 0.91f, 0.93f, 1.0f);
        rectf(trackX, thumbY, trackW, thumbH, 0.55f, 0.58f, 0.62f, 1.0f);
    }
}

/* ================= 逐帧绘制 ================= */
static void drawFrame(void) {
    pthread_mutex_lock(&gLock);
    if (!gInited) { pthread_mutex_unlock(&gLock); return; }

    Layout L;
    computeLayout(&L);
    float W = (float)gW, H = (float)gH, S = L.S;
    float bd = S;
    float pad = 8.0f * S;

    /* ---- 过滤匹配 ---- */
    if (gFilterDirty) {
        rebuildMatches();
        gFilterDirty = 0;
    }

    /* ---- 运行解析: 逐行流入(大文件按比例快速流入) ---- */
    if (gRunning && gDisplayed < gLineCount) {
        int prev = gDisplayed;
        int chunk = gLineCount / 120 + 1;   /* 约 2 秒流完 */
        gDisplayed += chunk;
        if (gDisplayed > gLineCount) gDisplayed = gLineCount;
        appendMatches(prev, gDisplayed);
        if (gDisplayed >= gLineCount) {
            gRunning = 0;
            gStatus = 3;
        }
    }

    /* ---- 清屏 ---- */
    glViewport(0, 0, gW, gH);
    glClearColor(CLR_BG, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    resetBatches();

    /* 工具栏背景 */
    rectf(0, L.toolbarY, W, L.toolbarH, CLR_TOOLBAR, 1.0f);

    /* 按钮 */
    for (int i = 0; i < BTN_COUNT; i++) {
        int hot = 0;
        if (i == BTN_PARSE  && gRunning) hot = 1;
        if (i == BTN_SEARCH && gSearchFocus) hot = 1;
        if (i == BTN_HELP   && gHelpOpen) hot = 1;
        float cr, cg, cb;
        if (hot)      { cr = 0.30f; cg = 0.55f; cb = 0.90f; }
        else          { cr = 0.24f; cg = 0.28f; cb = 0.35f; }
        rectf(L.btnX[i], L.btnY, L.btnW, L.btnH, cr, cg, cb, 1.0f);

        float tw = textWidth(BTN_LABELS[i], S);
        float tx = L.btnX[i] + (L.btnW - tw) / 2.0f;
        float ty = L.btnY + (L.btnH - FONT_ASC_H * S) / 2.0f;
        drawText(tx, ty, BTN_LABELS[i], S, CLR_TEXT, 1.0f);
    }

    /* 状态栏 */
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s%s", S_STATUS_PREFIX, STATUS_TEXTS[gStatus]);
        drawText(L.margin + 4.0f * S, L.statusY, buf, S, CLR_DIM, 1.0f);
    }

    /* 日志显示子窗口(白底黑字) */
    rectf(L.logX - bd, L.logY - bd, L.logW + 2*bd, L.logH + 2*bd, 0.72f, 0.75f, 0.79f, 1.0f);
    rectf(L.logX, L.logY, L.logW, L.logH, 0.99f, 0.99f, 0.99f, 1.0f);
    drawText(L.logX + pad, L.logY + 5.0f * S, S_TITLE_LOG, S, 0.20f, 0.26f, 0.36f, 1.0f);
    drawLogWindow(&L, S, pad);

    /* 搜索关键字子窗口 */
    {
        float bdr, bdg, bdb;
        if (gSearchFocus) { bdr = 1.00f; bdg = 0.80f; bdb = 0.25f; }
        else              { bdr = 0.30f; bdg = 0.34f; bdb = 0.42f; }
        rectf(L.searchX - bd, L.searchY - bd, L.searchW + 2*bd, L.searchH + 2*bd, bdr, bdg, bdb, 1.0f);
        rectf(L.searchX, L.searchY, L.searchW, L.searchH, 0.12f, 0.14f, 0.18f, 1.0f);
        drawText(L.searchX + pad, L.searchY + 5.0f * S, S_TITLE_SEARCH, S, 0.60f, 0.75f, 0.95f, 1.0f);

        if (gKeyword[0]) {
            drawText(L.searchX + pad, L.searchY + L.titlePad + 2.0f * S, gKeyword, S, CLR_HL, 1.0f);
        } else {
            drawText(L.searchX + pad, L.searchY + L.titlePad + 2.0f * S, S_SEARCH_HINT, S, CLR_DIM, 1.0f);
        }
        char mc[64];
        snprintf(mc, sizeof(mc), S_MATCH_FMT, gMatchCount);
        float tx = L.searchX + L.searchW - pad - textWidth(mc, S);
        drawText(tx, L.searchY + L.titlePad + 2.0f * S, mc, S, CLR_DIM, 1.0f);
    }

    flushBatches();

    /* 帮助面板(覆盖层) */
    if (gHelpOpen) {
        float hw = W - 6.0f * L.margin;
        float hh = 10.0f * L.lineH;
        float hx = (W - hw) / 2.0f;
        float hy = (H - hh) / 2.0f;

        rectf(hx - bd, hy - bd, hw + 2*bd, hh + 2*bd, 0.30f, 0.34f, 0.42f, 1.0f);
        rectf(hx, hy, hw, hh, 0.16f, 0.18f, 0.23f, 0.98f);

        drawText(hx + pad, hy + 6.0f * S, S_HELP_TITLE, S, CLR_HL, 1.0f);
        static const char* const HELP_LINES[] = {
            S_HELP_L1, S_HELP_L2, S_HELP_L3, S_HELP_L4, S_HELP_L5, S_HELP_L6, S_HELP_L7
        };
        for (int i = 0; i < 7; i++)
            drawText(hx + pad, hy + L.titlePad + (float)i * L.lineH + 4.0f * S,
                     HELP_LINES[i], S, CLR_TEXT, 1.0f);
        flushBatches();
    }

    pthread_mutex_unlock(&gLock);
}

/* ================= 触摸处理(UI 线程) ================= */
/* 把触摸 y 映射为滚动位置(与滚动条几何一致) */
static void scrollbarToY(Layout* L, float y) {
    float S = L->S;
    int visibleLines = (int)((L->logH - L->titlePad - 6.0f * S) / L->lineH);
    if (visibleLines < 1) visibleLines = 1;
    int maxScroll = gMatchCount - visibleLines;
    if (maxScroll <= 0) { gScroll = 0; return; }

    float trackY = L->logY + L->titlePad;
    float trackH = L->logH - L->titlePad - 4.0f * S;
    float thumbH = trackH * (float)visibleLines / (float)gMatchCount;
    float minThumb = 12.0f * S;
    if (thumbH < minThumb) thumbH = minThumb;
    if (thumbH > trackH) thumbH = trackH;
    float range = trackH - thumbH;
    float frac = (y - trackY - thumbH * 0.5f) / range;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    gScroll = (int)((1.0f - frac) * (float)maxScroll + 0.5f);
}

static int hitButton(const Layout* L, float x, float y) {
    if (y < L->btnY || y > L->btnY + L->btnH) return -1;
    for (int i = 0; i < BTN_COUNT; i++)
        if (x >= L->btnX[i] && x <= L->btnX[i] + L->btnW) return i;
    return -1;
}

static void touchDown(float x, float y) {
    Layout L; computeLayout(&L);
    gDownX = (int)x; gDownY = (int)y; gLastY = (int)y;
    gDownBtn = hitButton(&L, x, y);
    gDownInLog = (x >= L.logX && x <= L.logX + L.logW &&
                  y >= L.logY && y <= L.logY + L.logH);
    gScrollDrag = 0;
    if (gDownInLog && gDownBtn < 0 && x >= L.logX + L.logW - 20.0f * L.S) {
        /* 右边缘 20S 宽作为滚动条触摸区: 点击轨道即跳到对应位置 */
        gScrollDrag = 1;
        scrollbarToY(&L, y);
    }
}

static void touchMove(float x, float y) {
    (void)x;
    if (gScrollDrag) {
        Layout L; computeLayout(&L);
        scrollbarToY(&L, y);
    } else if (gDownInLog) {
        Layout L; computeLayout(&L);
        int delta = (int)((gLastY - y) / L.lineH);
        if (delta != 0) { gScroll += delta; gLastY = (int)y; }
    }
}

static void touchUp(JNIEnv* env, float x, float y) {
    Layout L; computeLayout(&L);
    int btn = hitButton(&L, x, y);
    if (gDownBtn >= 0 && btn == gDownBtn) {
        switch (btn) {
            case BTN_OPEN:
                gHelpOpen = 0;
                if (env && gCls && gReqOpenFile)
                    (*env)->CallStaticVoidMethod(env, gCls, gReqOpenFile);
                break;
            case BTN_REFRESH:
                if (env && gCls && gReqRefreshFile)
                    (*env)->CallStaticVoidMethod(env, gCls, gReqRefreshFile);
                break;
            case BTN_PARSE:
                startParse();
                break;
            case BTN_STOP:
                gRunning = 0;
                if (gLineCount > 0) gStatus = 3;
                break;
            case BTN_SEARCH:
                gSearchFocus = 1;
                gHelpOpen = 0;
                if (env && gCls && gReqFocus)
                    (*env)->CallStaticVoidMethod(env, gCls, gReqFocus);
                break;
            case BTN_HELP:
                gHelpOpen = !gHelpOpen;
                break;
        }
    }
    gDownBtn = -1;
    gDownInLog = 0;
    gScrollDrag = 0;
}

/* ================= JNI 接口(与 LogViewGL.java 对应) ================= */

JNIEXPORT void JNICALL
Java_com_example_ndkgles_LogViewGL_init(JNIEnv* env, jclass cls) {
    (void)cls;
    pthread_mutex_lock(&gLock);
    gProgSolid = buildProgram(SOLID_VS, SOLID_FS);
    gProgTex   = buildProgram(TEX_VS, TEX_FS);
    if (!gProgSolid || !gProgTex) { LOGE("init: program failed"); pthread_mutex_unlock(&gLock); return; }

    gUSolidHalf = glGetUniformLocation(gProgSolid, "uHalf");
    gASolidP    = glGetAttribLocation(gProgSolid, "aP");
    gASolidC    = glGetAttribLocation(gProgSolid, "aC");
    gUTexHalf   = glGetUniformLocation(gProgTex, "uHalf");
    gUTexSampler= glGetUniformLocation(gProgTex, "uTex");
    gATexP      = glGetAttribLocation(gProgTex, "aP");
    gATexUV     = glGetAttribLocation(gProgTex, "aUV");
    gATexC      = glGetAttribLocation(gProgTex, "aC");

    gTexAsc = makeFontTexture(&FONT_ASC_BMP[0][0], FONT_ASC_COUNT,
                              FONT_ASC_W, FONT_ASC_H, 16, &gAscW, &gAscH);
    gTexCjk = makeFontTexture(&FONT_CJK_BMP[0][0], FONT_CJK_COUNT,
                              FONT_CJK_W, FONT_CJK_H, 16, &gCjkW, &gCjkH);

    jclass c = (*env)->FindClass(env, "com/example/ndkgles/LogViewGL");
    if (c) {
        gCls = (*env)->NewGlobalRef(env, c);
        gReqFocus     = (*env)->GetStaticMethodID(env, c, "requestSearchFocus", "()V");
        gReqOpenFile  = (*env)->GetStaticMethodID(env, c, "requestOpenFile", "()V");
        gReqRefreshFile = (*env)->GetStaticMethodID(env, c, "requestRefreshFile", "()V");
    }

    gInited = 1;
    gStatus = 0;
    LOGI("init ok, renderer=%s", glGetString(GL_RENDERER));
    pthread_mutex_unlock(&gLock);
}

JNIEXPORT void JNICALL
Java_com_example_ndkgles_LogViewGL_setSize(JNIEnv* env, jclass cls, jint w, jint h) {
    (void)env; (void)cls;
    pthread_mutex_lock(&gLock);
    gW = w; gH = h > 0 ? h : 1;
    pthread_mutex_unlock(&gLock);
}

JNIEXPORT void JNICALL
Java_com_example_ndkgles_LogViewGL_drawFrame(JNIEnv* env, jclass cls) {
    (void)env; (void)cls;
    drawFrame();
}

JNIEXPORT void JNICALL
Java_com_example_ndkgles_LogViewGL_touchDown(JNIEnv* env, jclass cls, jfloat x, jfloat y) {
    (void)env; (void)cls;
    pthread_mutex_lock(&gLock);
    touchDown(x, y);
    pthread_mutex_unlock(&gLock);
}

JNIEXPORT void JNICALL
Java_com_example_ndkgles_LogViewGL_touchMove(JNIEnv* env, jclass cls, jfloat x, jfloat y) {
    (void)env; (void)cls;
    pthread_mutex_lock(&gLock);
    touchMove(x, y);
    pthread_mutex_unlock(&gLock);
}

JNIEXPORT void JNICALL
Java_com_example_ndkgles_LogViewGL_touchUp(JNIEnv* env, jclass cls, jfloat x, jfloat y) {
    (void)cls;
    pthread_mutex_lock(&gLock);
    touchUp(env, x, y);
    pthread_mutex_unlock(&gLock);
}

JNIEXPORT void JNICALL
Java_com_example_ndkgles_LogViewGL_setKeyword(JNIEnv* env, jclass cls, jstring kw) {
    (void)cls;
    const char* s = (*env)->GetStringUTFChars(env, kw, NULL);
    pthread_mutex_lock(&gLock);
    if (s) {
        strncpy(gKeyword, s, sizeof(gKeyword) - 1);
        gKeyword[sizeof(gKeyword) - 1] = 0;
        gSearchFocus = 0;
        gFilterDirty = 1;
        (*env)->ReleaseStringUTFChars(env, kw, s);
    }
    pthread_mutex_unlock(&gLock);
}

JNIEXPORT void JNICALL
Java_com_example_ndkgles_LogViewGL_setFileContent(JNIEnv* env, jclass cls, jbyteArray data, jboolean tail) {
    (void)cls;
    jsize len = (*env)->GetArrayLength(env, data);
    if (len > MAX_BYTES) len = MAX_BYTES;
    char* buf = (char*)malloc((size_t)len + 1);
    if (!buf) return;
    if (len > 0) (*env)->GetByteArrayRegion(env, data, 0, len, (jbyte*)buf);
    buf[len] = 0;
    pthread_mutex_lock(&gLock);
    applyFileContent(buf, (size_t)len, tail ? 1 : 0);
    pthread_mutex_unlock(&gLock);
    free(buf);
}
