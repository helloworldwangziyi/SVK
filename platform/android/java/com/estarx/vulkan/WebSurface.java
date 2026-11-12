package com.estarx.vulkan;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.view.MotionEvent;
import android.webkit.WebView;
import android.webkit.WebViewClient;

/** 独立 WebView 对象；文档 session 与系统 Surface generation 分离。 */
public final class WebSurface extends PlatformSurface {
    public interface Listener {
        void onPageStarted(WebSurface surface);
        void onPageFinished(WebSurface surface);
        void onError(WebSurface surface, int errorCode);
    }

    private final WebView webView;
    private int documentSession;
    private Listener listener;

    WebSurface(SurfaceRef ref, Context context, String initialUrl) {
        super(ref, new PlatformWebView(context));
        webView = (WebView) view();
        webView.setBackgroundColor(Color.WHITE);
        webView.setOverScrollMode(WebView.OVER_SCROLL_NEVER);
        webView.setVerticalScrollBarEnabled(false);
        webView.setHorizontalScrollBarEnabled(false);
        webView.setNestedScrollingEnabled(false);
        webView.getSettings().setJavaScriptEnabled(true);
        webView.getSettings().setDomStorageEnabled(true);
        webView.getSettings().setLoadWithOverviewMode(true);
        webView.getSettings().setUseWideViewPort(true);
        webView.setWebContentsDebuggingEnabled(true);
        webView.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageStarted(WebView view, String url, Bitmap favicon) {
                ++documentSession;
                if (listener != null) listener.onPageStarted(WebSurface.this);
            }

            @Override
            public void onPageFinished(WebView view, String url) {
                if (listener != null) listener.onPageFinished(WebSurface.this);
            }

            @Override
            public void onReceivedError(
                    WebView view, int errorCode, String description, String failingUrl) {
                if (listener != null) listener.onError(WebSurface.this, errorCode);
            }
        });
        if (initialUrl != null && !initialUrl.isEmpty()) webView.loadUrl(initialUrl);
    }

    public WebView webView() { return webView; }
    public int documentSession() { return documentSession; }
    public void setListener(Listener value) { listener = value; }

    public SurfaceResult navigate(String url) {
        if (state() == State.CLOSED) return new SurfaceResult(SurfaceResult.Error.STALE_REFERENCE);
        if (url == null || url.isEmpty()) {
            return new SurfaceResult(SurfaceResult.Error.INVALID_ARGUMENT);
        }
        webView.loadUrl(url);
        return SurfaceResult.OK;
    }

    @Override
    public SurfaceResult suspend() {
        webView.onPause();
        return super.suspend();
    }

    @Override
    public SurfaceResult resume() {
        webView.onResume();
        return super.resume();
    }

    @Override
    protected void onClose() {
        webView.stopLoading();
        webView.destroy();
    }

    /** 单指竖滚由联动布局协调；横向区域和多指图表事件继续交给 Chromium。 */
    private static final class PlatformWebView extends WebView {
        private float downX;
        private float downY;

        PlatformWebView(Context context) {
            super(context);
        }

        @Override
        public boolean onTouchEvent(MotionEvent event) {
            if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
                downX = event.getX();
                downY = event.getY();
            } else if (event.getActionMasked() == MotionEvent.ACTION_MOVE) {
                final float dx = event.getX() - downX;
                final float dy = event.getY() - downY;
                if (event.getPointerCount() == 1 && Math.abs(dy) > Math.abs(dx)) return true;
            }
            return super.onTouchEvent(event);
        }

        @Override
        public void requestDisallowInterceptTouchEvent(boolean disallowIntercept) {
            if (!disallowIntercept) super.requestDisallowInterceptTouchEvent(false);
        }

        @Override
        public boolean performClick() {
            return super.performClick();
        }
    }
}
