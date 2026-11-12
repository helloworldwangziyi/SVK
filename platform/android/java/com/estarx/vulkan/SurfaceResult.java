package com.estarx.vulkan;

public final class SurfaceResult {
    public enum Error {
        NONE, INVALID_ARGUMENT, INVALID_STATE, STALE_REFERENCE,
        INVALID_LAYOUT, OWNERSHIP_CONFLICT, CONTROLLED_BY_LAYOUT, PLATFORM_FAILURE
    }

    public static final SurfaceResult OK = new SurfaceResult(Error.NONE);
    public final Error error;

    public SurfaceResult(Error error) {
        this.error = error;
    }

    public boolean ok() {
        return error == Error.NONE;
    }
}
