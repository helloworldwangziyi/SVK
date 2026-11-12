package com.estarx.vulkan;

import android.webkit.WebView;

/** 页面稳定 ID 与成员引用；位置只决定显示顺序，不决定成员类型。 */
public final class PageBinding {
    public final long pageId;
    public final String title;
    public final PlatformSurface surface;
    public float contentOffset;

    public PageBinding(long pageId, String title, PlatformSurface surface) {
        this.pageId = pageId;
        this.title = title;
        this.surface = surface;
    }

    public boolean isWeb() {
        return surface instanceof WebSurface;
    }

    public WebSurface web() {
        return isWeb() ? (WebSurface) surface : null;
    }

    public WebView webView() {
        return isWeb() ? ((WebSurface) surface).webView() : null;
    }
}
