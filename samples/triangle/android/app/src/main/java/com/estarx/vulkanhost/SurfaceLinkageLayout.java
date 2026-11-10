package com.estarx.vulkanhost;

import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.ValueAnimator;
import android.content.Context;
import android.graphics.Color;
import android.util.Log;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.VelocityTracker;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.animation.DecelerateInterpolator;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.OverScroller;
import android.widget.TextView;

import com.estarx.vulkan.VulkanSurfaceView;

import java.util.ArrayList;

/**
 * Android 专用多 Surface 联动探针，不是公共 UI 抽象。
 *
 * 首屏 Header 与底部内容都占满 TopBar 下的视口，Tab 覆盖 Header 底边，
 * WebView 初始位于屏幕外。纵向只由本类修改外层/Web/Native 偏移；网页内部可横滑的
 * 区域优先消费横向手势，其他区域才切换页面。菜单与页面动画各自只用一个进度。
 */
public final class SurfaceLinkageLayout extends FrameLayout {
    private static final String TAG = "SVK-SurfaceProbe";
    private static final int HEADER_SURFACE_ID = 0;
    private static final int NATIVE_SURFACE_ID = 1;
    private static final int MENU_SURFACE_ID = 2;
    private static final int WEB_PAGE_COUNT = 4;
    private static final int PAGE_NATIVE = 4;
    private static final int PAGE_COUNT = 5;
    private static final int GESTURE_NONE = 0;
    private static final int GESTURE_HORIZONTAL = 1;
    private static final int GESTURE_VERTICAL = 2;
    private static final int GESTURE_WEB_HORIZONTAL = 3;
    private static final int GESTURE_PAGER_ANIMATING = 4;

    private static final String[] WEB_PATHS = {"notice", "relevance", "rank", "f10"};
    private static final String[] TAB_TITLES = {"资讯", "关联", "排行", "F10", "绿色"};

    private final FrameLayout scene;
    private final FrameLayout scrollGroup;
    private final LinearLayout tabBar;
    private final FrameLayout contentViewport;
    private final FrameLayout menuContainer;
    private final VulkanSurfaceView headerSurface;
    private final VulkanSurfaceView nativeSurface;
    private final VulkanSurfaceView menuSurface;
    private final ArrayList<WebView> webViews = new ArrayList<>();
    private final ArrayList<Button> tabButtons = new ArrayList<>();
    private final float[] webOffsets = new float[WEB_PAGE_COUNT];
    private final View drawerBlocker;
    private final OverScroller scroller;
    private final int touchSlop;

    private VelocityTracker velocityTracker;
    private ValueAnimator pagerAnimator;
    private ValueAnimator drawerAnimator;

    private int sceneWidth;
    private int topBarHeight;
    private int headerHeight;
    private int tabHeight;
    private int contentHeight;
    private int drawerWidth;
    private int gesture = GESTURE_NONE;
    private int activePage = 0;
    private boolean touchStartedOnWeb;
    private Boolean webHorizontalInteractive;
    private int webProbeGeneration;
    private float downX;
    private float downY;
    private float lastY;
    private float startPagerPosition;
    private float pagerPosition;
    private float outerOffset;
    private float nativeOffset;
    private float drawerProgress;
    private int lastFlingY;

    private final Runnable flingRunnable =
            new Runnable() {
                @Override
                public void run() {
                    if (!scroller.computeScrollOffset()) {
                        Log.d(TAG, "event=fling_end");
                        return;
                    }
                    final int currentY = scroller.getCurrY();
                    scrollVerticalBy(currentY - lastFlingY);
                    lastFlingY = currentY;
                    postOnAnimation(this);
                }
            };

    public SurfaceLinkageLayout(Context context) {
        super(context);
        setClipChildren(false);
        setBackgroundColor(Color.WHITE);
        touchSlop = ViewConfiguration.get(context).getScaledTouchSlop();
        scroller = new OverScroller(context);
        topBarHeight = dp(56);
        tabHeight = dp(56);

        scene = new FrameLayout(context);
        scene.setClipChildren(true);
        addView(scene, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));

        scrollGroup = new FrameLayout(context);
        // Tab/内容初始在 scrollGroup 下边界外，这里不能提前裁剪；屏幕边界由
        // 不参与纵向滚动的 scene 统一裁剪，否则子 View 平移后仍无法进入。
        scrollGroup.setClipChildren(false);
        scene.addView(
                scrollGroup,
                new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));

        headerSurface = new VulkanSurfaceView(context, HEADER_SURFACE_ID, false);
        scrollGroup.addView(headerSurface);

        tabBar = new LinearLayout(context);
        tabBar.setOrientation(LinearLayout.HORIZONTAL);
        tabBar.setGravity(Gravity.CENTER);
        tabBar.setBackgroundColor(Color.WHITE);
        for (int page = 0; page < PAGE_COUNT; ++page) {
            Button tab = makeTab(context, TAB_TITLES[page], page);
            tabButtons.add(tab);
            tabBar.addView(
                    tab, new LinearLayout.LayoutParams(0, LayoutParams.MATCH_PARENT, 1.0f));
        }
        contentViewport = new FrameLayout(context);
        contentViewport.setClipChildren(true);
        contentViewport.setBackgroundColor(Color.WHITE);
        scrollGroup.addView(contentViewport);

        for (int page = 0; page < WEB_PAGE_COUNT; ++page) {
            WebView webView = createWebView(context, page);
            webViews.add(webView);
            contentViewport.addView(
                    webView,
                    new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
            webView.loadUrl(webUrl(WEB_PATHS[page]));
        }

        nativeSurface = new VulkanSurfaceView(context, NATIVE_SURFACE_ID, false);
        contentViewport.addView(nativeSurface);

        // Tab 在内容之后加入；绿色 Surface 内部上滑与 Tab 区域重叠时仍由 Tab 盖住内容。
        scrollGroup.addView(
                tabBar,
                new LayoutParams(LayoutParams.MATCH_PARENT, tabHeight));

        LinearLayout topBar = new LinearLayout(context);
        topBar.setOrientation(LinearLayout.HORIZONTAL);
        topBar.setGravity(Gravity.CENTER_VERTICAL);
        topBar.setBackgroundColor(Color.WHITE);
        TextView title = new TextView(context);
        title.setText("Surface Probe");
        title.setTextColor(Color.DKGRAY);
        title.setTextSize(16.0f);
        topBar.addView(title, new LinearLayout.LayoutParams(0, LayoutParams.MATCH_PARENT, 1.0f));
        Button menuButton = new Button(context);
        menuButton.setText("MENU");
        menuButton.setOnClickListener(v -> animateDrawer(1.0f));
        topBar.addView(menuButton, new LinearLayout.LayoutParams(dp(88), LayoutParams.MATCH_PARENT));
        scene.addView(
                topBar,
                new LayoutParams(LayoutParams.MATCH_PARENT, topBarHeight, Gravity.TOP));

        drawerBlocker = new View(context);
        drawerBlocker.setBackgroundColor(Color.TRANSPARENT);
        drawerBlocker.setVisibility(GONE);
        drawerBlocker.setOnClickListener(v -> animateDrawer(0.0f));
        addView(drawerBlocker, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));

        menuContainer = new FrameLayout(context);
        menuSurface = new VulkanSurfaceView(context, MENU_SURFACE_ID, false);
        // 菜单必须位于 WebView 和其他 Surface 之上；关闭入口是点击菜单外的 blocker。
        menuSurface.setZOrderOnTop(true);
        menuContainer.addView(
                menuSurface,
                new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
        addView(menuContainer);

        updateTabState();
    }

    private static String webUrl(String path) {
        // URL 与参数格式取自参考项目 K 线页；合约取 market_catalog.h 的 CFFEX|F|IF。
        return "https://news.epolestar.xyz/" + path + "?isEstarX=1"
                + "&contractNo=CFFEX%7CF%7CIF&theme=0&contractName=IF";
    }

    private WebView createWebView(Context context, int page) {
        WebView webView = new LinkedWebView(context);
        webView.setBackgroundColor(Color.WHITE);
        webView.setOverScrollMode(OVER_SCROLL_NEVER);
        webView.setVerticalScrollBarEnabled(false);
        webView.setHorizontalScrollBarEnabled(false);
        webView.setNestedScrollingEnabled(false);
        webView.getSettings().setJavaScriptEnabled(true);
        webView.getSettings().setDomStorageEnabled(true);
        webView.getSettings().setLoadWithOverviewMode(true);
        webView.getSettings().setUseWideViewPort(true);
        webView.setWebContentsDebuggingEnabled(true);
        webView.setWebViewClient(
                new WebViewClient() {
                    @Override
                    public void onPageFinished(WebView view, String url) {
                        webOffsets[page] = clamp(webOffsets[page], 0.0f, webContentMax(page));
                        applyScrollState();
                        Log.i(TAG, "event=web_loaded page=" + page
                                + " content_max=" + webContentMax(page));
                    }

                    @Override
                    public void onReceivedError(WebView view, int errorCode,
                                                String description, String failingUrl) {
                        Log.w(TAG, "event=web_load_failed page=" + page + " code=" + errorCode);
                    }
                });
        return webView;
    }

    private Button makeTab(Context context, String title, int page) {
        Button tab = new Button(context);
        tab.setText(title);
        tab.setTextSize(12.0f);
        tab.setOnClickListener(v -> animatePagerTo(page));
        return tab;
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        sceneWidth = w;
        // Header 与底部页面都等于 TopBar 下的完整视口；Tab 覆盖 Header 底边，
        // WebView 初始 y == 屏幕高度，外层只需移动 Header 高减去 Tab 高。
        headerHeight = Math.max(1, h - topBarHeight);
        contentHeight = headerHeight;
        drawerWidth = Math.max(1, w / 4);

        LayoutParams headerParams = (LayoutParams) headerSurface.getLayoutParams();
        headerParams.width = w;
        headerParams.height = headerHeight;
        headerParams.topMargin = topBarHeight;
        headerSurface.setLayoutParams(headerParams);

        LayoutParams tabParams = (LayoutParams) tabBar.getLayoutParams();
        tabParams.width = w;
        tabParams.height = tabHeight;
        tabParams.topMargin = topBarHeight + headerHeight - tabHeight;
        tabBar.setLayoutParams(tabParams);

        LayoutParams contentParams = (LayoutParams) contentViewport.getLayoutParams();
        contentParams.width = w;
        contentParams.height = contentHeight;
        contentParams.topMargin = topBarHeight + headerHeight;
        contentViewport.setLayoutParams(contentParams);

        LayoutParams nativeParams = (LayoutParams) nativeSurface.getLayoutParams();
        nativeParams.width = w;
        // 绿色内容只占一个完整视口；置顶后可见高度还要减去 Tab，不能按两倍
        // 内容高继续上移，否则 Surface 底边越过屏幕底部后会露出白色背景。
        nativeParams.height = contentHeight;
        nativeSurface.setLayoutParams(nativeParams);

        LayoutParams menuParams = (LayoutParams) menuContainer.getLayoutParams();
        menuParams.width = drawerWidth;
        menuParams.height = h;
        menuParams.leftMargin = w;
        menuContainer.setLayoutParams(menuParams);

        nativeOffset = clamp(nativeOffset, 0.0f, nativeContentMax());
        for (int page = 0; page < WEB_PAGE_COUNT; ++page) {
            webOffsets[page] = clamp(webOffsets[page], 0.0f, webContentMax(page));
        }
        outerOffset = clamp(outerOffset, 0.0f, outerMaxOffset());
        applyScrollState();
        applyPagerState();
        applyDrawerState();
        Log.i(TAG, "event=scene_resize width=" + w + " height=" + h
                + " topbar_height=" + topBarHeight + " header_height=" + headerHeight
                + " content_height=" + contentHeight + " outer_max=" + outerMaxOffset()
                + " drawer_width=" + drawerWidth);
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent event) {
        if (velocityTracker == null) {
            velocityTracker = VelocityTracker.obtain();
        }
        if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
            velocityTracker.clear();
        }
        velocityTracker.addMovement(event);
        if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
            stopFling();
            downX = event.getX();
            downY = event.getY();
            lastY = downY;
            startPagerPosition = pagerPosition;
            touchStartedOnWeb = isTouchInsideActiveWeb(event);
            webHorizontalInteractive = null;
            final int probeGeneration = ++webProbeGeneration;
            if (touchStartedOnWeb && !isPagerAnimating()) {
                probeWebHorizontalInteraction(event, probeGeneration);
            }
            gesture = isPagerAnimating() ? GESTURE_PAGER_ANIMATING : GESTURE_NONE;
        }
        if (event.getActionMasked() == MotionEvent.ACTION_MOVE
                && drawerProgress == 0.0f && gesture == GESTURE_NONE) {
            final float dx = event.getX() - downX;
            final float dy = event.getY() - downY;
            if (Math.max(Math.abs(dx), Math.abs(dy)) > touchSlop) {
                final boolean horizontal = Math.abs(dx) > Math.abs(dy);
                if (horizontal && touchStartedOnWeb && webHorizontalInteractive == null) {
                    // DOM 命中检测异步返回前继续把事件交给网页，不能先抢成 Tab 手势。
                    return super.dispatchTouchEvent(event);
                }
                stopFling();
                startPagerPosition = pagerPosition;
                if (horizontal && touchStartedOnWeb
                        && Boolean.TRUE.equals(webHorizontalInteractive)) {
                    gesture = GESTURE_WEB_HORIZONTAL;
                } else {
                    gesture = horizontal ? GESTURE_HORIZONTAL : GESTURE_VERTICAL;
                }
                Log.d(TAG, "event=gesture_lock owner=" + gestureName(gesture)
                        + " page=" + activePage);
            }
        }
        if ((event.getActionMasked() == MotionEvent.ACTION_UP
                || event.getActionMasked() == MotionEvent.ACTION_CANCEL)
                && gesture == GESTURE_WEB_HORIZONTAL) {
            gesture = GESTURE_NONE;
        }
        // SurfaceView 不消费 DOWN 时没有 child touch target，ViewGroup 会绕过
        // onInterceptTouchEvent 直接调用自身 onTouchEvent，因此方向锁定必须在这里完成。
        return super.dispatchTouchEvent(event);
    }

    private boolean isTouchInsideActiveWeb(MotionEvent event) {
        if (activePage >= WEB_PAGE_COUNT) {
            return false;
        }
        View webView = webViews.get(activePage);
        if (webView.getWidth() == 0 || webView.getHeight() == 0) {
            return false;
        }
        final int[] location = new int[2];
        webView.getLocationOnScreen(location);
        final float x = event.getRawX();
        final float y = event.getRawY();
        return x >= location[0] && x < location[0] + webView.getWidth()
                && y >= location[1] && y < location[1] + webView.getHeight();
    }

    private void probeWebHorizontalInteraction(MotionEvent event, int probeGeneration) {
        final int page = activePage;
        final WebView webView = webViews.get(page);
        final int[] location = new int[2];
        webView.getLocationOnScreen(location);
        final float scale = webView.getScale();
        if (scale <= 0.0f) {
            webHorizontalInteractive = false;
            return;
        }
        final float x = (event.getRawX() - location[0]) / scale;
        final float y = (event.getRawY() - location[1]) / scale;
        // View.canScrollHorizontally 只反映文档本身；除 overflow-x 列表外，Canvas/Chart
        // 可能用手势直接缩放而没有滚动范围，这些区域也必须整段交给网页。
        final String script = "(function(){var e=document.elementFromPoint(" + x + "," + y + ");"
                + "while(e){var s=getComputedStyle(e);"
                + "var marker=(e.id+' '+String(e.className)).toLowerCase();"
                + "if(e.tagName==='CANVAS'||marker.indexOf('chart')>=0)return true;"
                + "if(e.scrollWidth>e.clientWidth+1&&(s.overflowX==='auto'"
                + "||s.overflowX==='scroll'||e===document.documentElement))return true;"
                + "e=e.parentElement;}return false;})()";
        webView.evaluateJavascript(script, value -> {
            if (probeGeneration != webProbeGeneration) {
                Log.d(TAG, "event=web_horizontal_probe_stale generation=" + probeGeneration);
                return;
            }
            webHorizontalInteractive = Boolean.parseBoolean(value);
            Log.d(TAG, "event=web_horizontal_probe page=" + page
                    + " interactive=" + webHorizontalInteractive
                    + " result=" + (value == null ? "invalid" : "valid"));
        });
    }

    @Override
    public boolean onInterceptTouchEvent(MotionEvent event) {
        // 切页动画期间整段消费但不响应新手势，避免快速输入从半页状态继续切页。
        if (gesture == GESTURE_PAGER_ANIMATING) {
            return true;
        }
        if (drawerProgress > 0.0f || gesture == GESTURE_WEB_HORIZONTAL) {
            return false;
        }
        return gesture == GESTURE_HORIZONTAL || gesture == GESTURE_VERTICAL;
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                return true;
            case MotionEvent.ACTION_MOVE:
                if (gesture == GESTURE_HORIZONTAL && sceneWidth > 0) {
                    pagerPosition = clamp(startPagerPosition - (event.getX() - downX) / sceneWidth,
                            0.0f, PAGE_COUNT - 1.0f);
                    applyPagerState();
                } else if (gesture == GESTURE_VERTICAL) {
                    scrollVerticalBy(lastY - event.getY());
                    lastY = event.getY();
                }
                return true;
            case MotionEvent.ACTION_UP:
                if (gesture == GESTURE_HORIZONTAL) {
                    velocityTracker.computeCurrentVelocity(1000, dp(4000));
                    final float velocityX = velocityTracker.getXVelocity();
                    int target = Math.round(pagerPosition);
                    if (Math.abs(velocityX) > dp(800)) {
                        target = velocityX < 0.0f
                                ? Math.min(PAGE_COUNT - 1, Math.round(startPagerPosition) + 1)
                                : Math.max(0, Math.round(startPagerPosition) - 1);
                    }
                    animatePagerTo(target);
                } else if (gesture == GESTURE_VERTICAL) {
                    startVerticalFling();
                } else if (gesture == GESTURE_NONE) {
                    performClick();
                }
                gesture = GESTURE_NONE;
                return true;
            case MotionEvent.ACTION_CANCEL:
                if (gesture == GESTURE_HORIZONTAL) {
                    animatePagerTo(Math.round(startPagerPosition));
                }
                stopFling();
                gesture = GESTURE_NONE;
                return true;
            default:
                return gesture != GESTURE_NONE;
        }
    }

    @Override
    public boolean performClick() {
        return super.performClick();
    }

    private float outerMaxOffset() {
        return Math.max(0, headerHeight - tabHeight);
    }

    private void scrollVerticalBy(float amount) {
        final boolean wasPinned = outerOffset >= outerMaxOffset();
        if (amount > 0.0f) {
            final float accepted = Math.min(amount, outerMaxOffset() - outerOffset);
            outerOffset += accepted;
            final float remaining = amount - accepted;
            if (remaining > 0.0f) {
                setActiveContentOffset(activeContentOffset() + remaining);
            }
        } else if (amount < 0.0f) {
            final float before = activeContentOffset();
            setActiveContentOffset(before + amount);
            final float consumedByContent = activeContentOffset() - before;
            outerOffset = clamp(outerOffset + amount - consumedByContent, 0.0f, outerMaxOffset());
        }
        applyScrollState();
        final boolean pinned = outerOffset >= outerMaxOffset();
        if (wasPinned != pinned) {
            Log.d(TAG, "event=tab_pin_changed pinned=" + pinned
                    + " outer_offset=" + outerOffset);
        }
    }

    private void applyScrollState() {
        // 容器本身保持不动，避免平移到屏外后连同 Tab/WebView 一起被父布局裁掉。
        headerSurface.setTranslationY(-outerOffset);
        tabBar.setTranslationY(-outerOffset);
        contentViewport.setTranslationY(-outerOffset);
        for (int page = 0; page < WEB_PAGE_COUNT; ++page) {
            WebView webView = webViews.get(page);
            final int targetY = Math.round(webOffsets[page]);
            if (webView.getScrollY() != targetY) {
                // 网页横向手势归 Chromium，因此竖向定位必须保留网页自己的横向偏移。
                webView.scrollTo(webView.getScrollX(), targetY);
            }
        }
        nativeSurface.setTranslationY(-nativeOffset);
    }

    private float activeContentOffset() {
        return activePage < WEB_PAGE_COUNT ? webOffsets[activePage] : nativeOffset;
    }

    private void setActiveContentOffset(float value) {
        if (activePage < WEB_PAGE_COUNT) {
            webOffsets[activePage] = clamp(value, 0.0f, webContentMax(activePage));
        } else {
            nativeOffset = clamp(value, 0.0f, nativeContentMax());
        }
    }

    private float activeContentMax() {
        return activePage < WEB_PAGE_COUNT ? webContentMax(activePage) : nativeContentMax();
    }

    private float webContentMax(int page) {
        if (contentHeight <= 0) {
            return 0.0f;
        }
        WebView webView = webViews.get(page);
        return Math.max(0.0f, webView.getContentHeight() * webView.getScale() - contentHeight);
    }

    private float nativeContentMax() {
        final int visibleHeight = Math.max(0, contentHeight - tabHeight);
        return Math.max(0, contentHeight - visibleHeight);
    }

    private void startVerticalFling() {
        velocityTracker.computeCurrentVelocity(1000, dp(4000));
        final float velocityY = velocityTracker.getYVelocity();
        if (Math.abs(velocityY) < dp(80)) {
            return;
        }
        final int start = Math.round(outerOffset + activeContentOffset());
        final int maximum = Math.round(outerMaxOffset() + activeContentMax());
        if ((velocityY < 0.0f && start >= maximum) || (velocityY > 0.0f && start <= 0)) {
            Log.d(TAG, "event=fling_rejected reason=boundary start=" + start
                    + " max=" + maximum);
            return;
        }
        lastFlingY = start;
        scroller.fling(0, start, 0, Math.round(-velocityY), 0, 0, 0, maximum);
        Log.d(TAG, "event=fling_start start=" + start + " max=" + maximum);
        postOnAnimation(flingRunnable);
    }

    private void stopFling() {
        scroller.abortAnimation();
        removeCallbacks(flingRunnable);
    }

    private boolean isPagerAnimating() {
        return pagerAnimator != null && pagerAnimator.isRunning();
    }

    private void animatePagerTo(int page) {
        if (sceneWidth == 0) {
            return;
        }
        final int targetPage = Math.max(0, Math.min(page, PAGE_COUNT - 1));
        stopFling();
        if (pagerAnimator != null) {
            pagerAnimator.cancel();
        }
        if (Math.abs(targetPage - activePage) > 1) {
            // 跨多个 Tab 直接交换首尾页面，不让中间 WebView/Surface 逐页穿过。
            pagerPosition = targetPage;
            activePage = targetPage;
            applyPagerState();
            updateTabState();
            Log.i(TAG, "event=page_changed reason=non_adjacent page=" + activePage
                    + " outer_offset=" + outerOffset
                    + " content_offset=" + activeContentOffset());
            return;
        }
        final float start = pagerPosition;
        final float target = targetPage;
        pagerAnimator = ValueAnimator.ofFloat(start, target);
        pagerAnimator.setDuration(220L);
        pagerAnimator.setInterpolator(new DecelerateInterpolator());
        pagerAnimator.addUpdateListener(animation -> {
            pagerPosition = (float) animation.getAnimatedValue();
            applyPagerState();
        });
        pagerAnimator.addListener(
                new AnimatorListenerAdapter() {
                    private boolean cancelled;

                    @Override
                    public void onAnimationCancel(Animator animation) {
                        cancelled = true;
                    }

                    @Override
                    public void onAnimationEnd(Animator animation) {
                        if (cancelled) {
                            return;
                        }
                        pagerPosition = target;
                        activePage = targetPage;
                        applyPagerState();
                        updateTabState();
                        Log.i(TAG, "event=page_changed page=" + activePage
                                + " outer_offset=" + outerOffset
                                + " content_offset=" + activeContentOffset());
                    }
                });
        pagerAnimator.start();
    }

    private void applyPagerState() {
        for (int page = 0; page < WEB_PAGE_COUNT; ++page) {
            webViews.get(page).setTranslationX((page - pagerPosition) * sceneWidth);
        }
        nativeSurface.setTranslationX((PAGE_NATIVE - pagerPosition) * sceneWidth);
    }

    private void updateTabState() {
        for (int page = 0; page < PAGE_COUNT; ++page) {
            tabButtons.get(page).setTextColor(page == activePage ? Color.RED : Color.DKGRAY);
        }
    }

    private void animateDrawer(float target) {
        if (drawerWidth == 0) {
            return;
        }
        stopFling();
        if (pagerAnimator != null) {
            pagerAnimator.cancel();
        }
        if (drawerAnimator != null) {
            drawerAnimator.cancel();
        }
        drawerAnimator = ValueAnimator.ofFloat(drawerProgress, target);
        drawerAnimator.setDuration(260L);
        drawerAnimator.setInterpolator(new DecelerateInterpolator());
        drawerAnimator.addUpdateListener(animation -> {
            drawerProgress = (float) animation.getAnimatedValue();
            applyDrawerState();
        });
        drawerAnimator.addListener(
                new AnimatorListenerAdapter() {
                    private boolean cancelled;

                    @Override
                    public void onAnimationCancel(Animator animation) {
                        cancelled = true;
                    }

                    @Override
                    public void onAnimationEnd(Animator animation) {
                        if (!cancelled) {
                            Log.i(TAG, "event=drawer_changed progress=" + target
                                    + " drawer_width=" + drawerWidth);
                        }
                    }
                });
        drawerBlocker.setVisibility(VISIBLE);
        drawerAnimator.start();
        Log.i(TAG, "event=drawer_start target=" + target);
    }

    private void applyDrawerState() {
        final float translation = -drawerWidth * drawerProgress;
        scene.setTranslationX(translation);
        drawerBlocker.setTranslationX(translation);
        menuContainer.setTranslationX(translation);
        drawerBlocker.setVisibility(drawerProgress > 0.0f ? VISIBLE : GONE);
    }

    @Override
    protected void onDetachedFromWindow() {
        stopFling();
        if (pagerAnimator != null) {
            pagerAnimator.cancel();
        }
        if (drawerAnimator != null) {
            drawerAnimator.cancel();
        }
        if (velocityTracker != null) {
            velocityTracker.recycle();
            velocityTracker = null;
        }
        for (WebView webView : webViews) {
            webView.stopLoading();
            webView.destroy();
        }
        super.onDetachedFromWindow();
    }

    /** 单指竖滚由场景协调；网页横向区域和多指图表手势必须继续交给 Chromium。 */
    private static final class LinkedWebView extends WebView {
        private float downX;
        private float downY;

        LinkedWebView(Context context) {
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
                if (event.getPointerCount() == 1 && Math.abs(dy) > Math.abs(dx)) {
                    return true;
                }
            }
            return super.onTouchEvent(event);
        }

        @Override
        public void requestDisallowInterceptTouchEvent(boolean disallowIntercept) {
            if (!disallowIntercept) {
                super.requestDisallowInterceptTouchEvent(false);
            }
            // Chromium 不知道外层 Tab 边界，不能单独撤销父容器的整段手势归属。
        }

        @Override
        public boolean performClick() {
            return super.performClick();
        }
    }

    private int dp(float value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private static float clamp(float value, float minimum, float maximum) {
        return Math.max(minimum, Math.min(value, maximum));
    }

    private static String gestureName(int owner) {
        if (owner == GESTURE_HORIZONTAL) return "horizontal";
        if (owner == GESTURE_VERTICAL) return "vertical";
        if (owner == GESTURE_WEB_HORIZONTAL) return "web_horizontal";
        return "none";
    }
}
