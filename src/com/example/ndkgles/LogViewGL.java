package com.example.ndkgles;

import android.content.Context;
import android.opengl.GLSurfaceView;
import android.os.Handler;
import android.os.Looper;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/** 日志查看器的 native 桥接: 装载 liblogview.so, 转发渲染/触摸/关键字。 */
public final class LogViewGL {
    static {
        System.loadLibrary("logview");
    }

    private static EditText sEdit = null;
    private static final Handler sMain = new Handler(Looper.getMainLooper());

    private static native void init();
    private static native void setSize(int w, int h);
    private static native void drawFrame();
    static native void touchDown(float x, float y);
    static native void touchMove(float x, float y);
    static native void touchUp(float x, float y);
    public  static native void setKeyword(String kw);

    /** 由 LogViewActivity 注入底部输入框, 供 [搜索] 按钮聚焦。 */
    public static void attach(EditText edit) {
        sEdit = edit;
    }

    /** 由 native 调用(在 GL 线程), 切回主线程聚焦输入框并弹出软键盘。 */
    public static void requestSearchFocus() {
        sMain.post(new Runnable() {
            @Override public void run() {
                if (sEdit != null) {
                    sEdit.requestFocus();
                    sEdit.setSelection(sEdit.length());
                    InputMethodManager imm = (InputMethodManager)
                        sEdit.getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
                    if (imm != null) imm.showSoftInput(sEdit, InputMethodManager.SHOW_IMPLICIT);
                }
            }
        });
    }

    /** 供 GLSurfaceView 使用(内部转发到 native)。 */
    public static final class Renderer implements GLSurfaceView.Renderer {
        @Override public void onSurfaceCreated(GL10 gl, EGLConfig config) { init(); }
        @Override public void onSurfaceChanged(GL10 gl, int width, int height) { setSize(width, height); }
        @Override public void onDrawFrame(GL10 gl) { drawFrame(); }
    }

    private LogViewGL() {}
}
