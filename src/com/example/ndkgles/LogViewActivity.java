package com.example.ndkgles;

import android.app.Activity;
import android.graphics.Color;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.MotionEvent;
import android.view.View;
import android.widget.EditText;
import android.widget.LinearLayout;

/**
 * 日志查看器入口: 上半部分是 GLSurfaceView(由 native 绘制工具栏/日志窗口/搜索窗口),
 * 底部一个 EditText 用于输入搜索关键字(软键盘), 触摸事件转发给 native 做按钮命中与滚动。
 */
public class LogViewActivity extends Activity {
    private GLSurfaceView glView;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(18, 20, 24));

        glView = new GLSurfaceView(this);
        glView.setEGLContextClientVersion(2);            // OpenGL ES 2.0
        glView.setRenderer(new LogViewGL.Renderer());
        LinearLayout.LayoutParams glp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1.0f);
        root.addView(glView, glp);

        final EditText edit = new EditText(this);
        edit.setHint("搜索关键字");
        edit.setSingleLine(true);
        edit.setTextColor(Color.WHITE);
        edit.setHintTextColor(Color.rgb(120, 126, 136));
        edit.setBackgroundColor(Color.rgb(24, 28, 36));
        root.addView(edit, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        LogViewGL.attach(edit);
        edit.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int c, int d) {}
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void afterTextChanged(Editable s) {
                LogViewGL.setKeyword(s.toString());
            }
        });

        /* 触摸坐标相对 GLSurfaceView, 与 native 布局坐标系一致(y 向下) */
        glView.setOnTouchListener(new View.OnTouchListener() {
            @Override public boolean onTouch(View v, MotionEvent ev) {
                float x = ev.getX(), y = ev.getY();
                switch (ev.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        LogViewGL.touchDown(x, y);
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        LogViewGL.touchMove(x, y);
                        return true;
                    case MotionEvent.ACTION_UP:
                        LogViewGL.touchUp(x, y);
                        return true;
                    default:
                        return false;
                }
            }
        });

        setContentView(root);
    }

    @Override protected void onResume() { super.onResume(); glView.onResume(); }
    @Override protected void onPause() { super.onPause(); glView.onPause(); }
}
