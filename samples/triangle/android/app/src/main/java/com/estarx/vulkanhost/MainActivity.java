package com.estarx.vulkanhost;

import android.app.Activity;
import android.os.Bundle;

import com.estarx.vulkan.NativeBridge;
import com.estarx.vulkan.SurfaceHost;

/** 只装配通用宿主；页面、URL、Surface 数量和组合由 C++ App 配置提供。 */
public class MainActivity extends Activity {
    private SurfaceHost surfaceHost;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        NativeBridge.nativeSetLogDirectory(getFilesDir().getAbsolutePath() + "/logs");
        surfaceHost = new SurfaceHost(this);
        setContentView(surfaceHost.rootView());
    }

    @Override
    protected void onDestroy() {
        if (surfaceHost != null) surfaceHost.close();
        super.onDestroy();
    }
}
