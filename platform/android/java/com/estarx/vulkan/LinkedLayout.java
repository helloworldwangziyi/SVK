package com.estarx.vulkan;

import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.ValueAnimator;
import android.content.Context;
import android.graphics.Color;
import android.os.SystemClock;
import android.util.Log;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.VelocityTracker;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.animation.DecelerateInterpolator;
import android.webkit.WebView;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.OverScroller;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * Android 联动父容器。成员由宿主创建并持有，本类只绑定稳定 PageId、处理手势、
 * 滚动和组内几何；页面决策由 core 状态机负责，动画由平台唯一执行源负责。
 */
public final class LinkedLayout extends FrameLayout {
    private static final String TAG = "SVK-SurfaceProbe";
    private static final int GESTURE_NONE = 0;
    private static final int GESTURE_HORIZONTAL = 1;
    private static final int GESTURE_VERTICAL = 2;
    private static final int GESTURE_WEB_HORIZONTAL = 3;
    private static final int GESTURE_CANCELLED = 4;

    private final long layoutId;
    private final long headerId;
    private final long drawerId;
    private final long pageStateHandle;
    private final NativeSurface headerMember;
    private final NativeSurface drawerMember;
    private final FrameLayout scene;
    private final FrameLayout scrollGroup;
    private final LinearLayout tabBar;
    private final FrameLayout contentViewport;
    private final FrameLayout menuContainer;
    private final VulkanSurfaceView headerSurface;
    private final VulkanSurfaceView menuSurface;
    private final ArrayList<PageBinding> pages = new ArrayList<>();
    private final ArrayList<Button> tabButtons = new ArrayList<>();
    private float[] webOffsets = new float[0];
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
    private boolean closed;
    private int activePage = 0;
    private int requestedPage = 0;
    private int pagerTarget = 0;
    private long pagerGeneration;
    private int touchContentPage = -1;
    private boolean touchStartedOnWeb;
    private boolean touchInProgress;
    private boolean waitingForWebProbe;
    private Boolean webHorizontalInteractive;
    private int webProbeGeneration;
    private float downX;
    private float downY;
    private float lastY;
    private float pendingSwipeOffset;
    private float pendingSwipeVelocity;
    private float pagerPosition;
    private float outerOffset;
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

    public LinkedLayout(
            Context context, long layoutId, NativeSurface header,
            List<PageBinding> initialPages, NativeSurface drawer, long initialPageId) {
        super(context);
        if (initialPages.isEmpty()) throw new IllegalArgumentException("pages must not be empty");
        this.layoutId = layoutId;
        this.headerMember = header;
        this.drawerMember = drawer;
        this.headerId = header.ref.id;
        this.drawerId = drawer == null ? 0 : drawer.ref.id;
        pages.addAll(initialPages);
        webOffsets = new float[pages.size()];
        activePage = Math.max(0, indexOfPage(initialPageId));
        requestedPage = activePage;
        pagerTarget = activePage;
        pagerPosition = activePage;
        pageStateHandle = NativeBridge.nativeCreatePageState(pageIds(), initialPageId);
        if (pageStateHandle == 0) throw new IllegalArgumentException("invalid page state");

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
        scrollGroup.setClipChildren(false);
        scene.addView(scrollGroup,
                new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));

        bindMember(header);
        headerSurface = header.nativeView();
        scrollGroup.addView(headerSurface);

        tabBar = new LinearLayout(context);
        tabBar.setOrientation(LinearLayout.HORIZONTAL);
        tabBar.setGravity(Gravity.CENTER);
        tabBar.setBackgroundColor(Color.WHITE);
        contentViewport = new FrameLayout(context);
        contentViewport.setClipChildren(true);
        contentViewport.setBackgroundColor(Color.WHITE);
        scrollGroup.addView(contentViewport);
        for (int page = 0; page < pages.size(); ++page) {
            addPageViews(page);
        }
        scrollGroup.addView(tabBar, new LayoutParams(LayoutParams.MATCH_PARENT, tabHeight));

        LinearLayout topBar = new LinearLayout(context);
        topBar.setOrientation(LinearLayout.HORIZONTAL);
        topBar.setGravity(Gravity.CENTER_VERTICAL);
        topBar.setBackgroundColor(Color.WHITE);
        TextView title = new TextView(context);
        title.setVisibility(GONE);
        topBar.addView(title, new LinearLayout.LayoutParams(0, LayoutParams.MATCH_PARENT, 1.0f));
        Button menuButton = new Button(context);
        menuButton.setText("MENU");
        menuButton.setOnClickListener(v -> animateDrawer(1.0f));
        topBar.addView(menuButton, new LinearLayout.LayoutParams(dp(88), LayoutParams.MATCH_PARENT));
        scene.addView(topBar,
                new LayoutParams(LayoutParams.MATCH_PARENT, topBarHeight, Gravity.TOP));

        drawerBlocker = new View(context);
        drawerBlocker.setBackgroundColor(Color.TRANSPARENT);
        drawerBlocker.setVisibility(GONE);
        drawerBlocker.setOnClickListener(v -> animateDrawer(0.0f));
        addView(drawerBlocker, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));

        menuContainer = new FrameLayout(context);
        menuSurface = drawer == null ? null : drawer.nativeView();
        menuButton.setVisibility(menuSurface == null ? GONE : VISIBLE);
        if (drawer != null) {
            bindMember(drawer);
            menuSurface.setZOrderOnTop(true);
            menuContainer.addView(menuSurface,
                    new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
        }
        addView(menuContainer);
        updateTabState();
    }

    private void bindMember(PlatformSurface surface) {
        final SurfaceResult result = surface.bindLayout(layoutId);
        if (!result.ok()) throw new IllegalArgumentException("surface already bound");
        surface.mount();
    }

    private void addPageViews(int page) {
        final PageBinding binding = pages.get(page);
        bindMember(binding.surface);
        contentViewport.addView(binding.surface.view(),
                new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
        Button tab = makeTab(getContext(), binding.title, page);
        tabButtons.add(tab);
        tabBar.addView(tab, new LinearLayout.LayoutParams(0, LayoutParams.MATCH_PARENT, 1.0f));
        if (!binding.isWeb()) return;
        binding.web().setListener(new WebSurface.Listener() {
            @Override
            public void onPageStarted(WebSurface surface) {
                final int index = indexOfSurface(surface);
                if (touchContentPage == index) {
                    ++webProbeGeneration;
                    waitingForWebProbe = false;
                    webHorizontalInteractive = true;
                }
                Log.i(TAG, "event=web_load_started page=" + index
                        + " session_id=" + surface.documentSession());
            }

            @Override
            public void onPageFinished(WebSurface surface) {
                final int index = indexOfSurface(surface);
                if (index < 0 || index >= pages.size()) return;
                webOffsets[index] = clamp(webOffsets[index], 0.0f, webContentMax(index));
                applyScrollState();
                Log.i(TAG, "event=web_loaded page=" + index
                        + " content_max=" + webContentMax(index));
            }

            @Override
            public void onError(WebSurface surface, int errorCode) {
                Log.w(TAG, "event=web_load_failed page=" + indexOfSurface(surface)
                        + " code=" + errorCode);
            }
        });
    }

    private Button makeTab(Context context, String title, int page) {
        Button tab = new Button(context);
        tab.setText(title);
        tab.setTextSize(12.0f);
        tab.setOnClickListener(v -> animatePagerTo(page));
        return tab;
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        final int webViewHeight = Math.max(0,
                MeasureSpec.getSize(heightMeasureSpec) - topBarHeight - tabHeight);
        // 必须在测量前固定 WebView 的实际可见高度；初始 MATCH_PARENT 会让 Chromium
        // 按整个窗口高度限制 scrollTo，导致最后一段内容无法滚入屏幕。
        for (PageBinding page : pages) {
            if (!page.isWeb()) continue;
            WebView webView = page.webView();
            LayoutParams params = (LayoutParams) webView.getLayoutParams();
            if (params.height != webViewHeight) {
                params.height = webViewHeight;
                webView.setLayoutParams(params);
            }
        }
        super.onMeasure(widthMeasureSpec, heightMeasureSpec);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        sceneWidth = w;
        // Header 与绿色内容等于 TopBar 下的完整视口；WebView 高度在 onMeasure 中设置，
        // 初始 y == 屏幕高度，外层只需移动 Header 高减去 Tab 高。
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

        for (int page = 0; page < pages.size(); ++page) {
            if (!pages.get(page).isWeb()) {
                View nativeView = pages.get(page).surface.view();
                LayoutParams nativeParams = (LayoutParams) nativeView.getLayoutParams();
                nativeParams.width = w;
                nativeParams.height = contentHeight;
                nativeView.setLayoutParams(nativeParams);
            }
            webOffsets[page] = clamp(webOffsets[page], 0.0f, webContentMax(page));
        }

        LayoutParams menuParams = (LayoutParams) menuContainer.getLayoutParams();
        menuParams.width = drawerWidth;
        menuParams.height = h;
        menuParams.leftMargin = w;
        menuContainer.setLayoutParams(menuParams);
        outerOffset = clamp(outerOffset, 0.0f, outerMaxOffset());
        applyScrollState();
        applyPagerState();
        applyDrawerState();
        if (oldw > 0 && oldh > 0) {
            cancelTouchProbe("resize");
            settlePager(requestedPage, "resize");
        }
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
            touchInProgress = true;
            waitingForWebProbe = false;
            touchContentPage = findTouchedContentPage(event);
            touchStartedOnWeb = touchContentPage >= 0 && pages.get(touchContentPage).isWeb();
            webHorizontalInteractive = null;
            final int probeGeneration = ++webProbeGeneration;
            if (touchStartedOnWeb) {
                probeWebHorizontalInteraction(event, touchContentPage, probeGeneration);
            }
            gesture = GESTURE_NONE;
        }
        if (event.getActionMasked() == MotionEvent.ACTION_POINTER_DOWN) {
            // Chromium 已持有 DOWN 时，多指继续完整交给网页；父容器已接管的手势
            // 只能取消并消费余下序列，不能把没有 DOWN 的后半段突然交回网页。
            ++webProbeGeneration;
            waitingForWebProbe = false;
            gesture = gesture == GESTURE_HORIZONTAL || gesture == GESTURE_VERTICAL
                    || gesture == GESTURE_CANCELLED
                    ? GESTURE_CANCELLED : GESTURE_WEB_HORIZONTAL;
            stopFling();
            Log.d(TAG, "event=gesture_cancel reason=multi_touch owner=" + gestureName(gesture));
        }
        if ((event.getActionMasked() == MotionEvent.ACTION_MOVE
                || event.getActionMasked() == MotionEvent.ACTION_UP)
                && drawerProgress == 0.0f && gesture == GESTURE_NONE) {
            final float dx = event.getX() - downX;
            final float dy = event.getY() - downY;
            if (Math.max(Math.abs(dx), Math.abs(dy)) > touchSlop) {
                final boolean horizontal = Math.abs(dx) > Math.abs(dy);
                if (horizontal && touchStartedOnWeb && webHorizontalInteractive == null) {
                    if (event.getActionMasked() == MotionEvent.ACTION_UP) {
                        // 短快滑可能先 UP 后收到 DOM 结果；保留本次距离和末段速度，
                        // 当前回调仍可完成同一次请求，下一次 DOWN/Cancel 会使其失效。
                        velocityTracker.computeCurrentVelocity(1000, dp(4000));
                        pendingSwipeOffset = dx;
                        pendingSwipeVelocity = velocityTracker.getXVelocity();
                        waitingForWebProbe = true;
                        touchInProgress = false;
                    }
                    return super.dispatchTouchEvent(event);
                }
                stopFling();
                if (horizontal && touchStartedOnWeb
                        && Boolean.TRUE.equals(webHorizontalInteractive)) {
                    gesture = GESTURE_WEB_HORIZONTAL;
                } else {
                    gesture = horizontal ? GESTURE_HORIZONTAL : GESTURE_VERTICAL;
                    if (!horizontal && isPagerAnimating()) {
                        // 纵向操作中断分页时落定手指实际命中的页，再修改该页偏移；
                        // 不把旧 activePage 的竖向滚动写到仍在平移的目标页上。
                        settlePager(touchContentPage >= 0 ? touchContentPage : requestedPage,
                                "vertical_gesture");
                    }
                }
                Log.d(TAG, "event=gesture_lock owner=" + gestureName(gesture)
                        + " page=" + activePage);
            }
        }
        // SurfaceView 不消费 DOWN 时没有 child touch target，ViewGroup 会绕过
        // onInterceptTouchEvent 直接调用自身 onTouchEvent，因此方向锁定必须在这里完成。
        final boolean handled = super.dispatchTouchEvent(event);
        if (event.getActionMasked() == MotionEvent.ACTION_UP
                || event.getActionMasked() == MotionEvent.ACTION_CANCEL) {
            touchInProgress = false;
            waitingForWebProbe = false;
            ++webProbeGeneration;
            gesture = GESTURE_NONE;
        }
        return handled;
    }

    private int findTouchedContentPage(MotionEvent event) {
        // 动画期间可能同时露出两页，不能仅按已落定页检测 Web。先检查固定视口
        // 的裁剪范围，再按当前平移后的系统位置找到真正持有 DOWN 的内容页。
        if (!containsTouch(contentViewport, event)) {
            return -1;
        }
        for (int page = 0; page < pages.size(); ++page) {
            if (containsTouch(pages.get(page).surface.view(), event)) return page;
        }
        return -1;
    }

    private boolean containsTouch(View view, MotionEvent event) {
        if (view.getWidth() == 0 || view.getHeight() == 0) {
            return false;
        }
        final int[] location = new int[2];
        view.getLocationOnScreen(location);
        final float x = event.getRawX();
        final float y = event.getRawY();
        return x >= location[0] && x < location[0] + view.getWidth()
                && y >= location[1] && y < location[1] + view.getHeight();
    }

    private void probeWebHorizontalInteraction(MotionEvent event, int page, int probeGeneration) {
        final WebSurface web = pages.get(page).web();
        final int session = web.documentSession();
        final WebView webView = web.webView();
        final int[] location = new int[2];
        webView.getLocationOnScreen(location);
        final float scale = webView.getScale();
        if (scale <= 0.0f) {
            webHorizontalInteractive = true;
            Log.w(TAG, "event=web_horizontal_probe_failed page=" + page
                    + " reason=invalid_scale action=keep_web_gesture");
            return;
        }
        final float x = (event.getRawX() - location[0]) / scale;
        final float y = (event.getRawY() - location[1]) / scale;
        // View.canScrollHorizontally 只反映文档本身；除 overflow-x 列表外，Canvas/Chart
        // 可能用手势直接缩放而没有滚动范围，这些区域也必须整段交给网页。
        final String script = "(function(){var e=document.elementFromPoint(" + x + "," + y + ");"
                + "if(!e)return true;while(e){var s=getComputedStyle(e);"
                + "var marker=(e.id+' '+String(e.className)).toLowerCase();"
                + "if(e.tagName==='CANVAS'||e.tagName==='IFRAME'"
                + "||marker.indexOf('chart')>=0)return true;"
                + "if(e.scrollWidth>e.clientWidth+1&&(s.overflowX==='auto'"
                + "||s.overflowX==='scroll'||e===document.documentElement))return true;"
                + "e=e.parentElement;}return false;})()";
        webView.evaluateJavascript(script, value -> {
            if (probeGeneration != webProbeGeneration || session != web.documentSession()) {
                Log.d(TAG, "event=web_horizontal_probe_stale generation=" + probeGeneration);
                return;
            }
            webHorizontalInteractive = !"false".equals(value);
            Log.d(TAG, "event=web_horizontal_probe page=" + page
                    + " session_id=" + session
                    + " interactive=" + webHorizontalInteractive
                    + " result=" + (value == null ? "invalid" : "valid"));
            if (waitingForWebProbe && !touchInProgress) {
                waitingForWebProbe = false;
                ++webProbeGeneration;
                if (!webHorizontalInteractive) {
                    requestSwipe(pendingSwipeOffset, pendingSwipeVelocity);
                }
            }
        });
    }

    @Override
    public boolean onInterceptTouchEvent(MotionEvent event) {
        if (gesture == GESTURE_CANCELLED) {
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
                if (gesture == GESTURE_VERTICAL) {
                    scrollVerticalBy(lastY - event.getY());
                    lastY = event.getY();
                }
                return true;
            case MotionEvent.ACTION_UP:
                if (gesture == GESTURE_HORIZONTAL) {
                    velocityTracker.computeCurrentVelocity(1000, dp(4000));
                    requestSwipe(event.getX() - downX, velocityTracker.getXVelocity());
                } else if (gesture == GESTURE_VERTICAL) {
                    startVerticalFling();
                } else if (gesture == GESTURE_NONE) {
                    performClick();
                }
                gesture = GESTURE_NONE;
                return true;
            case MotionEvent.ACTION_CANCEL:
                Log.d(TAG, "event=gesture_cancel reason=system_cancel owner="
                        + gestureName(gesture));
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

    private void requestSwipe(float offset, float velocity) {
        // 与鸿蒙保持相同的抬手判决：末段快甩或足够位移只推进一页。动画是分页
        // 进度的唯一写入者，因此连续 MOVE 不会与上一段切页动画争写各 Surface。
        final boolean fling = Math.abs(offset) > dp(12) && Math.abs(velocity) >= dp(800);
        if (Math.abs(offset) < dp(40) && !fling) {
            return;
        }
        final float direction = fling ? velocity : offset;
        animatePagerTo(requestedPage + (direction < 0.0f ? 1 : -1));
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
        for (int page = 0; page < pages.size(); ++page) {
            final PageBinding binding = pages.get(page);
            final int targetY = Math.round(webOffsets[page]);
            if (binding.isWeb()) {
                WebView webView = binding.webView();
                if (webView.getScrollY() != targetY) {
                    webView.scrollTo(webView.getScrollX(), targetY);
                }
            } else {
                binding.surface.view().setTranslationY(-webOffsets[page]);
            }
        }
    }

    private float activeContentOffset() {
        return webOffsets[activePage];
    }

    private void setActiveContentOffset(float value) {
        webOffsets[activePage] = clamp(value, 0.0f, activeContentMax());
    }

    private float activeContentMax() {
        return webContentMax(activePage);
    }

    private float webContentMax(int page) {
        if (contentHeight <= 0) return 0.0f;
        if (!pages.get(page).isWeb()) return nativeContentMax();
        WebView webView = pages.get(page).webView();
        // Chromium 按实际视口高度钳制 scrollTo，外层必须使用同一个高度。
        return Math.max(0.0f, webView.getContentHeight() * webView.getScale()
                - webView.getHeight());
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
        if (sceneWidth == 0 || drawerProgress > 0.0f
                || (drawerAnimator != null && drawerAnimator.isRunning())) return;
        final int targetPage = Math.max(0, Math.min(page, pages.size() - 1));
        stopFling();
        handlePageTransition(NativeBridge.nativePageSelect(
                pageStateHandle, pages.get(targetPage).pageId));
    }

    private void handlePageTransition(long[] transition) {
        if (transition == null || transition.length < 11 || transition[0] == 0) return;
        pagerTarget = (int) transition[2];
        activePage = (int) transition[3];
        requestedPage = (int) transition[4];
        switch ((int) transition[0]) {
            case 1:
                Log.d(TAG, "event=page_change_queued page=" + requestedPage
                        + " animation_target=" + pagerTarget);
                return;
            case 2:
                pagerPosition = activePage;
                applyPagerState();
                updateTabState();
                Log.i(TAG, "event=page_changed reason=non_adjacent page=" + activePage);
                return;
            case 3:
                startPagerAnimation(transition);
                return;
            case 4:
                pagerPosition = activePage;
                applyPagerState();
                updateTabState();
                Log.i(TAG, "event=page_changed page=" + activePage
                        + " outer_offset=" + outerOffset
                        + " content_offset=" + activeContentOffset());
                return;
            default:
                return;
        }
    }

    private void startPagerAnimation(long[] transition) {
        final int targetPage = (int) transition[2];
        final long generation = transition[5];
        pagerGeneration = generation;
        pagerTarget = targetPage;
        final float start = pagerPosition;
        final float target = targetPage;
        final long startedAt = SystemClock.uptimeMillis();
        pagerAnimator = ValueAnimator.ofFloat(start, target);
        pagerAnimator.setDuration(160L);
        pagerAnimator.setInterpolator(new DecelerateInterpolator());
        pagerAnimator.addUpdateListener(animation -> {
            pagerPosition = (float) animation.getAnimatedValue();
            applyPagerState();
        });
        pagerAnimator.addListener(new AnimatorListenerAdapter() {
            private boolean cancelled;

            @Override
            public void onAnimationCancel(Animator animation) {
                cancelled = true;
            }

            @Override
            public void onAnimationEnd(Animator animation) {
                if (cancelled || generation != pagerGeneration || animation != pagerAnimator) return;
                pagerAnimator = null;
                pagerPosition = target;
                applyPagerState();
                handlePageTransition(NativeBridge.nativePageComplete(pageStateHandle, generation));
                Log.d(TAG, "event=page_animation_completed page=" + activePage
                        + " elapsed_ms=" + (SystemClock.uptimeMillis() - startedAt));
            }
        });
        pagerAnimator.start();
        Log.i(TAG, "event=page_change_requested page=" + targetPage + " animate=true");
    }

    private void settlePager(int page, String reason) {
        if (page < 0 || page >= pages.size()) return;
        if (pagerAnimator != null) {
            pagerAnimator.cancel();
            pagerAnimator = null;
        }
        final long[] state = NativeBridge.nativePageSettle(
                pageStateHandle, pages.get(page).pageId);
        requestedPage = indexOfPage(state[2]);
        pagerTarget = indexOfPage(state[1]);
        activePage = indexOfPage(state[0]);
        pagerGeneration = state[3];
        pagerPosition = activePage;
        applyPagerState();
        updateTabState();
        Log.d(TAG, "event=page_animation_settled page=" + activePage + " reason=" + reason);
    }

    private void applyPagerState() {
        for (int page = 0; page < pages.size(); ++page) {
            pages.get(page).surface.view().setTranslationX((page - pagerPosition) * sceneWidth);
        }
    }

    private long[] pageIds() {
        return pageIds(pages);
    }

    private long[] pageIds(List<PageBinding> bindings) {
        final long[] ids = new long[bindings.size()];
        for (int index = 0; index < bindings.size(); ++index) ids[index] = bindings.get(index).pageId;
        return ids;
    }

    private int indexOfPage(long pageId) {
        for (int index = 0; index < pages.size(); ++index) {
            if (pages.get(index).pageId == pageId) return index;
        }
        return -1;
    }

    private int indexOfSurface(WebSurface surface) {
        for (int index = 0; index < pages.size(); ++index) {
            if (pages.get(index).web() == surface) return index;
        }
        return -1;
    }

    public SurfaceResult replacePages(List<PageBinding> replacements) {
        if (replacements == null || replacements.isEmpty()) {
            return new SurfaceResult(SurfaceResult.Error.INVALID_LAYOUT);
        }
        final Set<Long> pageIds = new HashSet<>();
        final Set<Long> surfaceIds = new HashSet<>();
        for (PageBinding page : replacements) {
            if (page.pageId == 0 || page.surface == null || page.surface.ref.id == 0
                    || page.surface.state() == PlatformSurface.State.CLOSED) {
                return new SurfaceResult(SurfaceResult.Error.INVALID_ARGUMENT);
            }
            if (page.surface.ref.id == headerId || page.surface.ref.id == drawerId
                    || !pageIds.add(page.pageId) || !surfaceIds.add(page.surface.ref.id)) {
                return new SurfaceResult(SurfaceResult.Error.OWNERSHIP_CONFLICT);
            }
            if (page.surface.layoutId() != 0 && page.surface.layoutId() != layoutId) {
                return new SurfaceResult(SurfaceResult.Error.OWNERSHIP_CONFLICT);
            }
        }
        final long[] state = NativeBridge.nativePageReplace(pageStateHandle, pageIds(replacements));
        if (state == null || state.length < 6 || state[5] == 0) {
            return new SurfaceResult(SurfaceResult.Error.INVALID_LAYOUT);
        }
        stopFling();
        cancelTouchProbe("replace_pages");
        if (pagerAnimator != null) {
            pagerAnimator.cancel();
            pagerAnimator = null;
        }
        final Map<Long, Float> oldOffsets = new HashMap<>();
        for (int index = 0; index < pages.size(); ++index) {
            oldOffsets.put(pages.get(index).pageId, webOffsets[index]);
        }
        for (PageBinding old : pages) {
            old.surface.unbindLayout(layoutId);
            if (!surfaceIds.contains(old.surface.ref.id)) old.surface.suspend();
        }
        contentViewport.removeAllViews();
        tabBar.removeAllViews();
        tabButtons.clear();
        pages.clear();
        pages.addAll(replacements);
        webOffsets = new float[pages.size()];
        for (int index = 0; index < pages.size(); ++index) {
            final Float offset = oldOffsets.get(pages.get(index).pageId);
            webOffsets[index] = offset == null ? 0.0f : offset;
            addPageViews(index);
        }
        activePage = indexOfPage(state[0]);
        pagerTarget = indexOfPage(state[1]);
        requestedPage = indexOfPage(state[2]);
        pagerGeneration = state[3];
        pagerPosition = activePage;
        applyScrollState();
        applyPagerState();
        updateTabState();
        requestLayout();
        return SurfaceResult.OK;
    }

    private void updateTabState() {
        for (int page = 0; page < pages.size(); ++page) {
            tabButtons.get(page).setTextColor(page == activePage ? Color.RED : Color.DKGRAY);
        }
    }

    private void animateDrawer(float target) {
        if (menuSurface == null || drawerWidth == 0) {
            return;
        }
        stopFling();
        cancelTouchProbe("drawer");
        settlePager(activePage, "drawer");
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

    private void cancelTouchProbe(String reason) {
        ++webProbeGeneration;
        touchInProgress = false;
        waitingForWebProbe = false;
        webHorizontalInteractive = null;
        gesture = GESTURE_NONE;
        Log.d(TAG, "event=gesture_cancel reason=" + reason);
    }

    @Override
    protected void onWindowVisibilityChanged(int visibility) {
        super.onWindowVisibilityChanged(visibility);
        if (visibility != VISIBLE && contentViewport != null) {
            // 后台不继续消费排队请求，直接落定最新已确认目标；恢复时不等待旧
            // 动画回调。单指尚未 UP 的操作只取消，不生成新的切页或滚动惯性。
            stopFling();
            cancelTouchProbe("window_hidden");
            settlePager(requestedPage, "window_hidden");
        }
    }

    @Override
    protected void onDetachedFromWindow() {
        close();
        super.onDetachedFromWindow();
    }

    /** 关闭只解绑组内关系；成员仍由各自宿主所有，不能在这里销毁 WebView 或 Surface。 */
    public void close() {
        if (closed) return;
        closed = true;
        stopFling();
        cancelTouchProbe("close");
        ++pagerGeneration;
        if (pagerAnimator != null) pagerAnimator.cancel();
        if (drawerAnimator != null) drawerAnimator.cancel();
        if (velocityTracker != null) {
            velocityTracker.recycle();
            velocityTracker = null;
        }
        for (PageBinding page : pages) page.surface.unbindLayout(layoutId);
        headerMember.unbindLayout(layoutId);
        if (drawerMember != null) drawerMember.unbindLayout(layoutId);
        NativeBridge.nativeDestroyPageState(pageStateHandle);
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
        if (owner == GESTURE_CANCELLED) return "cancelled";
        return "none";
    }
}
