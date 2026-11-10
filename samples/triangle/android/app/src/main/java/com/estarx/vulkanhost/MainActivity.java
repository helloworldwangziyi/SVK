package com.estarx.vulkanhost;

import android.app.Activity;
import android.os.Bundle;

import com.estarx.vulkan.NativeBridge;

/** 只装配多 Surface 场景探针；联动实现位于 SurfaceLinkageLayout。 */
public class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        NativeBridge.nativeSetLogDirectory(getFilesDir().getAbsolutePath() + "/logs");
        setContentView(new SurfaceLinkageLayout(this));
    }

    @Override
    protected void onDestroy() {
        NativeBridge.nativeShutdown();
        super.onDestroy();
    }
}
