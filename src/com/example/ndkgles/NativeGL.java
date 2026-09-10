package com.example.ndkgles;

import android.opengl.GLSurfaceView;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/** native GL 桥接: 装载 libnativegl.so 并在 GLSurfaceView.Renderer 回调里驱动 native 渲染。 */
public final class NativeGL {
    static {
        System.loadLibrary("nativegl");
    }

    private static native void initGL();
    private static native void setSize(int w, int h);
    private static native void drawFrame(float t);

    /** 供 GLSurfaceView 使用(内部转发到 native)。 */
    public static final class Renderer implements GLSurfaceView.Renderer {
        @Override
        public void onSurfaceCreated(GL10 gl, EGLConfig config) {
            initGL();
        }
        @Override
        public void onSurfaceChanged(GL10 gl, int width, int height) {
            setSize(width, height);
        }
        @Override
        public void onDrawFrame(GL10 gl) {
            drawFrame((System.currentTimeMillis() & 0xFFFF) / 1000.0f);
        }
    }

    private NativeGL() {}
}
