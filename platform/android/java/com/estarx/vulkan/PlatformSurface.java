package com.estarx.vulkan;

import android.view.View;
import android.view.ViewGroup;

/** 单个系统视图的 Java 侧控制对象；成员生命周期由宿主而非联动布局拥有。 */
public abstract class PlatformSurface {
    public enum State { CREATED, MOUNTED, SUSPENDED, CLOSED }

    protected final SurfaceRef ref;
    private final View view;
    private State state = State.CREATED;
    private long layoutId;

    protected PlatformSurface(SurfaceRef ref, View view) {
        this.ref = ref;
        this.view = view;
    }

    public final SurfaceRef ref() { return ref; }
    public final View view() { return view; }
    public final State state() { return state; }
    public final long layoutId() { return layoutId; }

    public final SurfaceResult bindLayout(long id) {
        if (state == State.CLOSED) return new SurfaceResult(SurfaceResult.Error.STALE_REFERENCE);
        if (layoutId != 0 && layoutId != id) {
            return new SurfaceResult(SurfaceResult.Error.OWNERSHIP_CONFLICT);
        }
        layoutId = id;
        return SurfaceResult.OK;
    }

    public final void unbindLayout(long id) {
        if (layoutId == id) layoutId = 0;
    }

    public SurfaceResult mount() {
        if (state == State.CLOSED) return new SurfaceResult(SurfaceResult.Error.STALE_REFERENCE);
        state = State.MOUNTED;
        return SurfaceResult.OK;
    }

    public SurfaceResult setPlacement(SurfacePlacement placement) {
        if (state == State.CLOSED) return new SurfaceResult(SurfaceResult.Error.STALE_REFERENCE);
        if (layoutId != 0) return new SurfaceResult(SurfaceResult.Error.CONTROLLED_BY_LAYOUT);
        return applyPlacement(placement);
    }

    public final SurfaceResult applyPlacement(SurfacePlacement placement) {
        ViewGroup.LayoutParams params = view.getLayoutParams();
        if (params == null) return new SurfaceResult(SurfaceResult.Error.PLATFORM_FAILURE);
        params.width = Math.round(placement.width);
        params.height = Math.round(placement.height);
        if (params instanceof ViewGroup.MarginLayoutParams) {
            ((ViewGroup.MarginLayoutParams) params).leftMargin = Math.round(placement.x);
            ((ViewGroup.MarginLayoutParams) params).topMargin = Math.round(placement.y);
        }
        view.setLayoutParams(params);
        view.setVisibility(placement.visible ? View.VISIBLE : View.GONE);
        view.setEnabled(placement.interactive);
        return SurfaceResult.OK;
    }

    public SurfaceResult suspend() {
        if (state == State.CLOSED) return new SurfaceResult(SurfaceResult.Error.STALE_REFERENCE);
        state = State.SUSPENDED;
        return SurfaceResult.OK;
    }

    public SurfaceResult resume() {
        if (state == State.CLOSED) return new SurfaceResult(SurfaceResult.Error.STALE_REFERENCE);
        state = State.MOUNTED;
        return SurfaceResult.OK;
    }

    public void close() {
        if (state == State.CLOSED) return;
        state = State.CLOSED;
        layoutId = 0;
        if (view.getParent() instanceof ViewGroup) {
            ((ViewGroup) view.getParent()).removeView(view);
        }
        onClose();
    }

    protected abstract void onClose();
}
