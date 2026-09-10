package com.example.ndkgles;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.net.Uri;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.View;
import android.widget.EditText;
import android.widget.Toast;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * 日志查看器入口: GLSurfaceView 全屏, 由 native 绘制工具栏/日志窗口/搜索窗口,
 * 触摸事件转发给 native 做按钮命中、滚动与滚动条拖动。
 * 搜索关键字通过点击 [搜索] 弹出的对话框输入。
 */
public class LogViewActivity extends Activity {
    private GLSurfaceView glView;
    private Uri mUri = null;       /* 当前打开的文件, 供 [刷新] 重读 */
    private String mKeyword = "";  /* 当前搜索关键字, 供弹窗预填 */

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);

        glView = new GLSurfaceView(this);
        glView.setEGLContextClientVersion(2);            // OpenGL ES 2.0
        glView.setRenderer(new LogViewGL.Renderer());
        setContentView(glView);

        LogViewGL.attach(this);

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

    /** 供 native [搜索] 按钮回调: 弹出搜索关键字输入框 */
    public void showSearchDialog() {
        final EditText input = new EditText(this);
        input.setSingleLine(true);
        input.setHint("搜索关键字");
        input.setText(mKeyword);
        input.setSelection(input.length());

        new AlertDialog.Builder(this)
                .setTitle("搜索关键字")
                .setView(input)
                .setPositiveButton("确定", new DialogInterface.OnClickListener() {
                    @Override public void onClick(DialogInterface d, int w) {
                        mKeyword = input.getText().toString();
                        LogViewGL.setKeyword(mKeyword);
                    }
                })
                .setNeutralButton("清除", new DialogInterface.OnClickListener() {
                    @Override public void onClick(DialogInterface d, int w) {
                        mKeyword = "";
                        LogViewGL.setKeyword("");
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    /** 供 native [刷新] 按钮回调: 重读同一文件并跳到末尾(tail) */
    public void refreshFile() {
        if (mUri == null) {
            Toast.makeText(this, "请先打开文件", Toast.LENGTH_SHORT).show();
            return;
        }
        loadFromUri(mUri, true);   // 刷新: 跳到末尾
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQ_OPEN_FILE || resultCode != RESULT_OK || data == null) return;
        Uri uri = data.getData();
        if (uri == null) return;
        mUri = uri;
        loadFromUri(uri, false);   // 打开: 显示开头
    }

    private void loadFromUri(Uri uri, boolean tail) {
        try {
            byte[] data = readBytes(uri);
            LogViewGL.setFileContent(data, tail);
        } catch (IOException e) {
            Toast.makeText(this, "读取文件失败", Toast.LENGTH_SHORT).show();
        }
    }

    private static final int MAX_BYTES = 50 * 1024 * 1024;

    /** 通过 ContentResolver 读取所选文件字节(最多 50MB) */
    private byte[] readBytes(Uri uri) throws IOException {
        InputStream in = getContentResolver().openInputStream(uri);
        if (in == null) throw new IOException("openInputStream null");
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        byte[] buf = new byte[65536];
        int n, total = 0;
        while ((n = in.read(buf)) > 0) {
            if (total + n > MAX_BYTES) {
                bos.write(buf, 0, MAX_BYTES - total);
                break;
            }
            bos.write(buf, 0, n);
            total += n;
        }
        in.close();
        return bos.toByteArray();
    }

    @Override protected void onResume() { super.onResume(); glView.onResume(); }
    @Override protected void onPause() { super.onPause(); glView.onPause(); }
}
