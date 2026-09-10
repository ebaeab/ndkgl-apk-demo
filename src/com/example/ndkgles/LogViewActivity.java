package com.example.ndkgles;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.MotionEvent;
import android.view.View;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.Toast;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

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

        LogViewGL.attach(LogViewActivity.this, edit);
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

    private static final int REQ_OPEN_FILE = 1001;

    /** 供 native [打开] 按钮回调: 弹出系统文件选择器 */
    @SuppressWarnings("deprecation")
    public void openFilePicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");   // 日志文件可能是 .log/.txt/.out, 放开类型
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivityForResult(intent, REQ_OPEN_FILE);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQ_OPEN_FILE || resultCode != RESULT_OK || data == null) return;
        Uri uri = data.getData();
        if (uri == null) return;
        try {
            String text = readText(uri);
            LogViewGL.setFileContent(text);
        } catch (IOException e) {
            Toast.makeText(this, "读取文件失败", Toast.LENGTH_SHORT).show();
        }
    }

    /** 通过 ContentResolver 读取所选文件文本(最多 1MB) */
    private String readText(Uri uri) throws IOException {
        InputStream in = getContentResolver().openInputStream(uri);
        if (in == null) throw new IOException("openInputStream null");
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n, total = 0;
        while ((n = in.read(buf)) > 0) {
            total += n;
            if (total > 1024 * 1024) break;   // 最多读 1MB, 足够查看日志
            bos.write(buf, 0, n);
        }
        in.close();
        return new String(bos.toByteArray(), StandardCharsets.UTF_8);
    }

    @Override protected void onResume() { super.onResume(); glView.onResume(); }
    @Override protected void onPause() { super.onPause(); glView.onPause(); }
}
