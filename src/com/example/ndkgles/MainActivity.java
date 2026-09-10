package com.example.ndkgles;

import android.app.Activity;
import android.opengl.GLSurfaceView;
import android.os.Bundle;

public class MainActivity extends Activity {
    private GLSurfaceView glView;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        glView = new GLSurfaceView(this);
        glView.setEGLContextClientVersion(2);        // OpenGL ES 2.0
        glView.setRenderer(new NativeGL.Renderer());
        setContentView(glView);
    }

    @Override
    protected void onResume() {
        super.onResume();
        glView.onResume();
    }

    @Override
    protected void onPause() {
        super.onPause();
        glView.onPause();
    }
}
