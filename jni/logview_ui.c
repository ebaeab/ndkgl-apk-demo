/*
 * logview_ui.c —— 原生 OpenGL ES2 日志查看器 UI
 *
 * 与 jni_gl.c 一样: EGL/线程/swap 交给 GLSurfaceView, native 只负责绘制与交互。
 * 本文件实现一个带「工具栏 + 日志显示子窗口 + 搜索关键字子窗口」的工具界面:
 *
 *   工具栏:  [打开] [运行解析] [停止] [搜索] [帮助]
 *   日志窗口: 滚动显示日志行(按 I/W/E 等级着色)
 *   搜索窗口: 显示当前关键字与匹配行数(关键字由 Java 侧 EditText 输入)
 *   帮助面板: 点击 [帮助] 弹出说明
 *
 * 文字渲染: 内嵌位图字体(jni/font_bitmap.h, 由 tools/genfont.c 生成),
 *           在 GPU 上打包成两张 alpha 纹理(ASCII + 中文), 逐字形画 quad。
 *
 * 加载: System.loadLibrary("logview")  ->  liblogview.so
 */
#include <jni.h>
#include <android/log.h>
#include <GLES2/gl2.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "ui_strings.h"
#include "font_bitmap.h"

#define TAG "LOGVIEW"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* ================= 全局状态 ================= */
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static int gW = 1, gH = 1;
static int gInited = 0;

#define BTN_OPEN   0
#define BTN_PARSE  1
#define BTN_STOP   2
#define BTN_SEARCH 3
#define BTN_HELP   4
#define BTN_COUNT  5
static const char* const BTN_LABELS[BTN_COUNT] = {
    S_BTN_OPEN, S_BTN_PARSE, S_BTN_STOP, S_BTN_SEARCH, S_BTN_HELP
};

static int gRunning = 0;      /* 是否正在“运行解析”(逐行流入) */
static int gHelpOpen = 0;     /* 帮助面板 */
static int gSearchFocus = 0;  /* 搜索窗口高亮 */
static char gKeyword[64] = "";

#define MAX_LINES 512
#define MAX_LEN   160
static char gLines[MAX_LINES][MAX_LEN];      /* 显示缓冲区(当前已显示的行) */
static int  gLineCount = 0;
static int  gScroll = 0;      /* 上滚行数(0=跟随最新) */

/* 已打开文件的内容(解析源) */
static char gFileLines[MAX_LINES][MAX_LEN];
static int  gFileLineCount = 0;
static int  gFileIdx = 0;     /* “运行解析”已流入显示缓冲区的行数 */

static const char* const STATUS_TEXTS[] = {
    S_STATUS_READY, S_STATUS_LOADED, S_STATUS_RUN, S_STATUS_STOP
};
static int gStatus = 0;       /* 0 就绪 1 已载入 2 解析中 3 已停止 */

/* 触摸状态 */
static int gDownX = 0, gDownY = 0, gDownBtn = -1, gDownInLog = 0, gLastY = 0;

/* ================= GL 对象 ================= */
static GLuint gProgSolid = 0, gProgTex = 0;
static GLint  gUSolidHalf = -1;
static GLint  gASolidP = -1, gASolidC = -1;
static GLint  gUTexHalf = -1, gUTexSampler = -1;
static GLint  gATexP = -1, gATexUV = -1, gATexC = -1;
static GLuint gTexAsc = 0, gTexCjk = 0;

/* JNI 回调 */
static jclass    gCls = NULL;
static jmethodID gReqFocus = NULL;    /* 搜索 -> 弹软键盘 */
static jmethodID gReqOpenFile = NULL; /* 打开 -> 系统文件选择器 */

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
    L->btnW = (W - 2.0f * L->margin - 4.0f * gap) / (float)BTN_COUNT;
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

/* 纯色矩形: 顶点含颜色属性 */
static const char* SOLID_VS =
    "attribute vec2 aP; attribute vec4 aC; varying vec4 vC; uniform vec2 uHalf;\n"
    "void main(){ gl_Position = vec4(aP.x/uHalf.x - 1.0, 1.0 - aP.y/uHalf.y, 0.0, 1.0); vC = aC; }\n";
static const char* SOLID_FS =
    "precision mediump float; varying vec4 vC;\n"
    "void main(){ gl_FragColor = vC; }\n";

/* 文字纹理: 顶点含 UV 与颜色属性 */
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

/* 记录两套纹理图集的尺寸 */
static int gAscW = 128, gAscH = 96;
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

static int containsNoCase(const char* hay, const char* needle) {
    if (!needle || !needle[0]) return 1;
    size_t n = strlen(needle);
    for (const char* h = hay; *h; h++) {
        const char* a = h, *b = needle; size_t k = 0;
        while (k < n && *a) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
            if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
            if (ca != cb) break;
            a++; b++; k++;
        }
        if (k == n) return 1;
    }
    return 0;
}

/* ================= 批次写入 ================= */
static void rectf(float x, float y, float w, float h,
                  float r, float g, float b, float a) {
    if (gRectN >= MAX_RECTS) return;
    float* v = gRectV + gRectN * 36;
    float x1 = x + w, y1 = y + h;
    /* 两个三角形, 每个顶点 x,y,r,g,b,a */
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

/* 画一行文字(自动按 ASCII/中文 分到两张纹理批次, v 方向已翻转适配 GL) */
static void drawText(float x, float y, const char* s, float scale,
                     float r, float g, float b, float a) {
    const char* p = s;
    while (*p) {
        unsigned int cp = utf8Next(&p);
        if (cp < 0x80) {
            int idx = (int)cp - 0x20;
            if (idx < 0 || idx >= FONT_ASC_COUNT) { x += FONT_ASC_W * scale; continue; }
            int col = idx % 16, row = idx / 16;
            float u0 = (col * FONT_ASC_W) / (float)gAscW;
            float u1 = ((col + 1) * FONT_ASC_W) / (float)gAscW;
            /* GL 把纹理首行当底部, 图集又是自顶向下填充, 故字形顶部 v=row, 底部 v=row+1 */
            float v0 = (row * FONT_ASC_H) / (float)gAscH;          /* 顶部 */
            float v1 = ((row + 1) * FONT_ASC_H) / (float)gAscH;    /* 底部 */
            glyph(gAscV, &gAscN, x, y, FONT_ASC_W * scale, FONT_ASC_H * scale,
                  u0, v0, u1, v1, r, g, b, a);
            x += FONT_ASC_W * scale;
        } else {
            int idx = cjkIndex(cp);
            if (idx < 0) { x += FONT_CJK_W * scale; continue; }
            int col = idx % 16, row = idx / 16;
            float u0 = (col * FONT_CJK_W) / (float)gCjkW;
            float u1 = ((col + 1) * FONT_CJK_W) / (float)gCjkW;
            float v0 = (row * FONT_CJK_H) / (float)gCjkH;          /* 顶部 */
            float v1 = ((row + 1) * FONT_CJK_H) / (float)gCjkH;    /* 底部 */
            glyph(gCjkV, &gCjkN, x, y, FONT_CJK_W * scale, FONT_CJK_H * scale,
                  u0, v0, u1, v1, r, g, b, a);
            x += FONT_CJK_W * scale;
        }
    }
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
#define CLR_BTN       0.24f, 0.28f, 0.35f
#define CLR_BTN_HOT   0.30f, 0.55f, 0.90f
#define CLR_WIN_BG    0.12f, 0.14f, 0.18f
#define CLR_BORDER    0.30f, 0.34f, 0.42f
#define CLR_TEXT      0.88f, 0.90f, 0.93f
#define CLR_TITLE     0.60f, 0.75f, 0.95f
#define CLR_DIM       0.55f, 0.58f, 0.62f
#define CLR_HL        1.00f, 0.80f, 0.25f

static void logLineColor(const char* line, float* r, float* g, float* b) {
    if (strstr(line, " E ")) { *r = 0.72f; *g = 0.06f; *b = 0.06f; return; }  /* 深红 */
    if (strstr(line, " W ")) { *r = 0.72f; *g = 0.45f; *b = 0.00f; return; }  /* 深琥珀 */
    if (strstr(line, " I ")) { *r = 0.05f; *g = 0.30f; *b = 0.60f; return; }  /* 深蓝 */
    *r = 0.05f; *g = 0.06f; *b = 0.08f;                                       /* 近黑 */
}

/* 匹配索引(过滤后的可见行) */
static int gMatchIdx[MAX_LINES];
static int gMatchCount = 0;

static void rebuildMatches(void) {
    gMatchCount = 0;
    int filter = gKeyword[0] != 0;
    for (int i = 0; i < gLineCount; i++) {
        if (filter && !containsNoCase(gLines[i], gKeyword)) continue;
        if (gMatchCount < MAX_LINES) gMatchIdx[gMatchCount++] = i;
    }
}

/* 视觉行(自动换行后的每一行): 源行号 + 段范围 [s, e) */
typedef struct { int line; int s; int e; } VRow;
#define MAX_VROWS 4096
static VRow gVRows[MAX_VROWS];
static int  gVRowCount = 0;

/* 把匹配到的源行按最大宽度拆成视觉行(逐字符, 至少放一个字符) */
static void buildVRows(float scale, float maxW) {
    gVRowCount = 0;
    for (int m = 0; m < gMatchCount && gVRowCount < MAX_VROWS; m++) {
        const char* s = gLines[gMatchIdx[m]];
        const char* p = s;
        while (*p && gVRowCount < MAX_VROWS) {
            const char* q = p, *end = p;
            float w = 0;
            while (*q) {
                unsigned int cp = utf8Next(&q);
                float cw = (cp < 0x80 ? FONT_ASC_W : FONT_CJK_W) * scale;
                if (w + cw > maxW && end != p) break;  /* 已放不下且本段已有字符 */
                w += cw;
                end = q;
                if (w >= maxW) break;                  /* 已填满 */
            }
            gVRows[gVRowCount].line = gMatchIdx[m];
            gVRows[gVRowCount].s = (int)(p - s);
            gVRows[gVRowCount].e = (int)(end - s);
            gVRowCount++;
            p = end;
        }
    }
}

/* ================= 文件装载 / 解析 ================= */
/* 把打开的文件内容填入 gFileLines 并立即显示 */
static void applyFileContent(const char* text) {
    gFileLineCount = 0;
    gLineCount = 0;
    gScroll = 0;
    gFileIdx = 0;
    gRunning = 0;

    const char* p = text;
    while (*p && gFileLineCount < MAX_LINES) {
        const char* nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= MAX_LEN) len = MAX_LEN - 1;
        if (len > 0 && p[len-1] == '\r') len--;      /* 去掉 CRLF 的 \r */
        if (len > 0) {
            memcpy(gFileLines[gFileLineCount], p, len);
            gFileLines[gFileLineCount][len] = 0;
            memcpy(gLines[gFileLineCount], gFileLines[gFileLineCount], len + 1);
            gFileLineCount++;
            gLineCount++;
        }
        if (!nl) break;
        p = nl + 1;
    }
    gStatus = 1; /* 已载入 */
}

/* 开始“运行解析”: 清空显示后逐行流入(模拟逐行解析) */
static void startParse(void) {
    if (gFileLineCount == 0) return;
    gLineCount = 0;
    gFileIdx = 0;
    gScroll = 0;
    gRunning = 1;
    gStatus = 2; /* 解析中 */
}

/* 每帧最多流入的行数 */
#define STREAM_PER_FRAME 2

/* ================= 逐帧绘制 ================= */
static void drawFrame(void) {
    pthread_mutex_lock(&gLock);
    if (!gInited) { pthread_mutex_unlock(&gLock); return; }

    Layout L;
    computeLayout(&L);
    float W = (float)gW, H = (float)gH, S = L.S;
    float bd = S;                       /* 边框宽度(随缩放) */
    float pad = 8.0f * S;

    /* ---- 运行解析: 逐行流入显示 ---- */
    if (gRunning && gFileIdx < gFileLineCount) {
        for (int i = 0; i < STREAM_PER_FRAME && gFileIdx < gFileLineCount; i++) {
            if (gLineCount < MAX_LINES) {
                memcpy(gLines[gLineCount], gFileLines[gFileIdx], MAX_LEN);
                gLines[gLineCount][MAX_LEN - 1] = 0;
                gLineCount++;
            }
            gFileIdx++;
        }
        if (gFileIdx >= gFileLineCount) {
            gRunning = 0;
            gStatus = 3; /* 已停止 */
        }
    }
    rebuildMatches();
    buildVRows(S, L.logW - 2.0f * pad);

    int visible = (int)((L.logH - L.titlePad - 6.0f * S) / L.lineH);
    if (visible < 1) visible = 1;
    int maxScroll = gVRowCount - visible;
    if (maxScroll < 0) maxScroll = 0;
    if (gScroll > maxScroll) gScroll = maxScroll;
    if (gScroll < 0) gScroll = 0;

    /* ---- 清屏 ---- */
    glViewport(0, 0, gW, gH);
    glClearColor(CLR_BG, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* ============ 主界面批次 ============ */
    resetBatches();

    /* 工具栏背景 */
    rectf(0, L.toolbarY, W, L.toolbarH, CLR_TOOLBAR, 1.0f);

    /* 五个按钮 */
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

    if (gMatchCount == 0) {
        const char* msg = gLineCount ? S_NO_MATCH : S_EMPTY_LOG;
        float tx = L.logX + (L.logW - textWidth(msg, S)) / 2.0f;
        float ty = L.logY + L.titlePad + L.lineH;
        drawText(tx, ty, msg, S, 0.45f, 0.48f, 0.52f, 1.0f);
    } else {
        int start = gVRowCount - visible - gScroll;
        if (start < 0) start = 0;
        int end = gVRowCount - gScroll;
        if (end < 0) end = 0;
        if (end > gVRowCount) end = gVRowCount;
        for (int k = start; k < end; k++) {
            float y = L.logY + L.titlePad + (float)(k - start) * L.lineH;
            if (y + L.lineH > L.logY + L.logH - 4.0f * S) break;
            VRow* vr = &gVRows[k];
            int len = vr->e - vr->s;
            char seg[MAX_LEN];
            if (len >= MAX_LEN) len = MAX_LEN - 1;
            memcpy(seg, gLines[vr->line] + vr->s, len);
            seg[len] = 0;
            float r, g, b;
            logLineColor(gLines[vr->line], &r, &g, &b);
            drawText(L.logX + pad, y, seg, S, r, g, b, 1.0f);
        }
    }

    /* 搜索关键字子窗口 */
    {
        float bdr, bdg, bdb;
        if (gSearchFocus) { bdr = 1.00f; bdg = 0.80f; bdb = 0.25f; }
        else              { bdr = 0.30f; bdg = 0.34f; bdb = 0.42f; }
        rectf(L.searchX - bd, L.searchY - bd, L.searchW + 2*bd, L.searchH + 2*bd, bdr, bdg, bdb, 1.0f);
        rectf(L.searchX, L.searchY, L.searchW, L.searchH, CLR_WIN_BG, 1.0f);
        drawText(L.searchX + pad, L.searchY + 5.0f * S, S_TITLE_SEARCH, S, CLR_TITLE, 1.0f);

        /* 关键字或提示 */
        if (gKeyword[0]) {
            drawText(L.searchX + pad, L.searchY + L.titlePad + 2.0f * S, gKeyword, S, CLR_HL, 1.0f);
        } else {
            drawText(L.searchX + pad, L.searchY + L.titlePad + 2.0f * S, S_SEARCH_HINT, S, CLR_DIM, 1.0f);
        }
        /* 匹配计数(靠右) */
        char mc[64];
        snprintf(mc, sizeof(mc), S_MATCH_FMT, gMatchCount);
        float tx = L.searchX + L.searchW - pad - textWidth(mc, S);
        drawText(tx, L.searchY + L.titlePad + 2.0f * S, mc, S, CLR_DIM, 1.0f);
    }

    /* 主界面先提交, 保证帮助面板覆盖其上 */
    flushBatches();

    /* ============ 帮助面板(覆盖层) ============ */
    if (gHelpOpen) {
        float hw = W - 2.0f * L.margin * 3.0f;
        float hh = 9.0f * L.lineH;
        float hx = (W - hw) / 2.0f;
        float hy = (H - hh) / 2.0f;

        rectf(hx - bd, hy - bd, hw + 2*bd, hh + 2*bd, CLR_BORDER, 1.0f);
        rectf(hx, hy, hw, hh, 0.16f, 0.18f, 0.23f, 0.98f);

        drawText(hx + pad, hy + 6.0f * S, S_HELP_TITLE, S, CLR_HL, 1.0f);
        static const char* const HELP_LINES[] = {
            S_HELP_L1, S_HELP_L2, S_HELP_L3, S_HELP_L4, S_HELP_L5, S_HELP_L6
        };
        for (int i = 0; i < 6; i++)
            drawText(hx + pad, hy + L.titlePad + (float)i * L.lineH + 4.0f * S,
                     HELP_LINES[i], S, CLR_TEXT, 1.0f);
        flushBatches();
    }

    pthread_mutex_unlock(&gLock);
}

/* ================= 触摸处理(UI 线程) ================= */
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
}

static void touchMove(float x, float y) {
    (void)x;
    if (gDownInLog) {
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
                /* 调系统文件选择器, 结果经 setFileContent 回来 */
                gHelpOpen = 0;
                if (env && gCls && gReqOpenFile)
                    (*env)->CallStaticVoidMethod(env, gCls, gReqOpenFile);
                break;
            case BTN_PARSE:
                startParse();
                break;
            case BTN_STOP:
                gRunning = 0;
                if (gFileIdx < gFileLineCount || gLineCount > 0) gStatus = 3;
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

    /* 缓存 Java 回调 */
    jclass c = (*env)->FindClass(env, "com/example/ndkgles/LogViewGL");
    if (c) {
        gCls = (*env)->NewGlobalRef(env, c);
        gReqFocus    = (*env)->GetStaticMethodID(env, c, "requestSearchFocus", "()V");
        gReqOpenFile = (*env)->GetStaticMethodID(env, c, "requestOpenFile", "()V");
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
        (*env)->ReleaseStringUTFChars(env, kw, s);
    }
    pthread_mutex_unlock(&gLock);
}

JNIEXPORT void JNICALL
Java_com_example_ndkgles_LogViewGL_setFileContent(JNIEnv* env, jclass cls, jstring text) {
    (void)cls;
    const char* s = (*env)->GetStringUTFChars(env, text, NULL);
    pthread_mutex_lock(&gLock);
    if (s) {
        applyFileContent(s);
        (*env)->ReleaseStringUTFChars(env, text, s);
    }
    pthread_mutex_unlock(&gLock);
}
