package com.estarx.vulkan;

import android.app.Activity;
import android.view.View;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

/** 通用 Android 宿主：读取 App C++ 配置、拥有成员并转发最终关闭。 */
public final class SurfaceHost {
    private final long hostId;
    private final SurfaceFactory factory;
    private final ArrayList<PlatformSurface> members = new ArrayList<>();
    private LinkedLayout layout;
    private boolean closed;

    public SurfaceHost(Activity activity) {
        hostId = NativeBridge.nativeCreateHost();
        factory = new SurfaceFactory(activity, hostId);
        try {
            final JSONObject spec = new JSONObject(NativeBridge.nativeGetSceneSpec());
            final JSONObject headerJson = spec.getJSONObject("header");
            final NativeSurface header = createNative(
                    headerJson.getLong("id"), headerJson.getInt("contentId"));
            NativeSurface drawer = null;
            if (spec.has("drawer")) {
                final JSONObject drawerJson = spec.getJSONObject("drawer");
                drawer = createNative(drawerJson.getLong("id"), drawerJson.getInt("contentId"));
            }
            final JSONArray pageJson = spec.getJSONArray("pages");
            final ArrayList<PageBinding> pages = new ArrayList<>();
            for (int index = 0; index < pageJson.length(); ++index) {
                final JSONObject page = pageJson.getJSONObject(index);
                final long surfaceId = page.getLong("surfaceId");
                final PlatformSurface surface = page.has("url")
                        ? createWeb(surfaceId, page.getString("url"))
                        : createNative(surfaceId, page.getInt("contentId"));
                pages.add(new PageBinding(page.getLong("pageId"), page.getString("title"), surface));
            }
            layout = new LinkedLayout(
                    activity, hostId, header, pages, drawer, spec.getLong("initialPage"));
        } catch (JSONException | RuntimeException error) {
            close();
            throw new IllegalStateException("Invalid linked scene", error);
        }
    }

    public View rootView() {
        if (layout == null) throw new IllegalStateException("host is not ready");
        return layout;
    }

    public WebSurface createWeb(long surfaceId, String initialUrl) {
        final WebSurface surface = factory.createWeb(surfaceId, initialUrl);
        members.add(surface);
        return surface;
    }

    public NativeSurface createNative(long surfaceId, int contentId) {
        final NativeSurface surface = factory.createNative(surfaceId, contentId);
        members.add(surface);
        return surface;
    }

    public PageBinding createWebPage(
            long pageId, String title, long surfaceId, String initialUrl) {
        return new PageBinding(pageId, title, createWeb(surfaceId, initialUrl));
    }

    public PageBinding createNativePage(long pageId, String title, long surfaceId, int contentId) {
        return new PageBinding(pageId, title, createNative(surfaceId, contentId));
    }

    public SurfaceResult replacePages(List<PageBinding> pages) {
        if (layout == null || closed) return new SurfaceResult(SurfaceResult.Error.STALE_REFERENCE);
        return layout.replacePages(pages);
    }

    public void closeSurface(PlatformSurface surface) {
        if (surface == null) return;
        surface.close();
        members.remove(surface);
    }

    public void close() {
        if (closed) return;
        closed = true;
        if (layout != null) layout.close();
        for (PlatformSurface member : members) member.close();
        members.clear();
        NativeBridge.nativeDestroyHost(hostId);
    }
}
