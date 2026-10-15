package com.estarx.vulkansample;

import android.app.Activity;
import android.os.Bundle;

import com.estarx.vulkan.NativeBridge;

/**
 * 三角形示例 Activity：整屏就是一个 VulkanSurfaceView（薄壳视图）。
 * 绘制内容完全由 native 引擎决定，本类不含任何业务逻辑。
 */
public class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
    }

    @Override
    protected void onDestroy() {
        // Activity.onDestroy 语义 = 真正销毁：全量释放引擎资源。
        // surface 的保活挂起/恢复由 VulkanSurfaceView 的
        // surfaceDestroyed/surfaceCreated 独立驱动，两者互不替代。
        NativeBridge.nativeShutdown();
        super.onDestroy();
    }
}
