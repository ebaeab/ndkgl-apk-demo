/*
 * jni_gl.c —— native GLES2 渲染(JNI,由 Java 的 GLSurfaceView.Renderer 驱动)
 *
 * 设计: EGL context / surface / 渲染线程 / swap 全部交给框架的 GLSurfaceView,
 *       native 只负责建 shader program、设置视口、每帧绘制一个旋转的彩色三角形。
 * 加载: System.loadLibrary("nativegl")  →  libnativegl.so
 *
 * 链接须知: 必须在链接时加 -lGLESv2, 使 libnativegl.so 声明 DT_NEEDED=libGLESv2.so,
 *          否则 GL 符号在运行期无法解析, dlopen 失败(UnsatisfiedLinkError)。
 */
#include <jni.h>
#include <android/log.h>
#include <GLES2/gl2.h>

#define TAG "NDKGL"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static GLuint gProg;
static GLint  gUT, gAPos, gAColor;
static int    gW = 1, gH = 1;

/* ---- 着色器: 顶点随时间旋转,片段颜色随时间脉动 ---- */
static const char* VS =
    "attribute vec2 aP; attribute vec3 aC; uniform float uT; varying vec3 vC;\n"
    "void main(){\n"
    "  float c=cos(uT), s=sin(uT);\n"
    "  gl_Position = vec4(c*aP.x - s*aP.y, s*aP.x + c*aP.y, 0.0, 1.0);\n"
    "  vC = aC;\n"
    "}\n";
static const char* FS =
    "precision mediump float; varying vec3 vC; uniform float uT;\n"
    "void main(){ gl_FragColor = vec4(vC * (0.6 + 0.4*sin(uT*2.0)), 1.0); }\n";

/* 三顶点: x, y, r, g, b */
static const float TRI[15] = {
    -0.5f, -0.45f,  1, 0, 0,
     0.5f, -0.45f,  0, 1, 0,
     0.0f,  0.55f,  0, 0, 1,
};

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

/* ================= JNI 接口(与 NativeGL.java 对应) ================= */

JNIEXPORT void JNICALL
Java_com_example_ndkgles_NativeGL_initGL(JNIEnv* env, jclass cls) {
    (void)env; (void)cls;
    gProg = buildProgram(VS, FS);
    if (!gProg) { LOGE("initGL: program creation failed"); return; }
    gUT     = glGetUniformLocation(gProg, "uT");
    gAPos   = glGetAttribLocation(gProg, "aP");
    gAColor = glGetAttribLocation(gProg, "aC");
    glDisable(GL_DEPTH_TEST);
    LOGI("initGL ok, renderer=%s", glGetString(GL_RENDERER));
}

JNIEXPORT void JNICALL
Java_com_example_ndkgles_NativeGL_setSize(JNIEnv* env, jclass cls, jint w, jint h) {
    (void)env; (void)cls;
    gW = w; gH = h > 0 ? h : 1;
    glViewport(0, 0, gW, gH);
}

JNIEXPORT void JNICALL
Java_com_example_ndkgles_NativeGL_drawFrame(JNIEnv* env, jclass cls, jfloat t) {
    (void)env; (void)cls;
    if (!gProg) return;

    glClearColor(0.35f, 0.45f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(gProg);
    glUniform1f(gUT, t);
    glEnableVertexAttribArray(gAPos);
    glEnableVertexAttribArray(gAColor);
    glVertexAttribPointer(gAPos,   2, GL_FLOAT, GL_FALSE, 5*sizeof(float), TRI);
    glVertexAttribPointer(gAColor, 3, GL_FLOAT, GL_FALSE, 5*sizeof(float), TRI+2);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableVertexAttribArray(gAPos);
    glDisableVertexAttribArray(gAColor);
    /* eglSwapBuffers 由 GLSurfaceView 负责 */
}
