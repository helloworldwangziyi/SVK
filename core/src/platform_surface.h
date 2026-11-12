#pragma once

#include <cstdint>
#include <memory>

namespace svk {

using SurfaceId = uint64_t;
using PageId = uint64_t;
using NativeContentId = uint32_t;

struct SurfaceRef {
    SurfaceId id = 0;
    uint64_t instanceGeneration = 0;

    bool valid() const { return id != 0; }
};

inline bool operator==(const SurfaceRef& lhs, const SurfaceRef& rhs) {
    return lhs.id == rhs.id && lhs.instanceGeneration == rhs.instanceGeneration;
}

struct PixelRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct SurfacePlacement {
    PixelRect bounds;
    PixelRect clip;
    bool visible = true;
    bool interactive = true;
};

enum class SurfaceState { Created, Mounted, Suspended, Closed };
enum class SurfaceError {
    None,
    InvalidArgument,
    InvalidState,
    StaleReference,
    PageNotFound,
    InvalidLayout,
    OwnershipConflict,
    ControlledByLayout,
    Unsupported,
    PlatformFailure,
};

struct SurfaceResult {
    SurfaceError error = SurfaceError::None;
    bool ok() const { return error == SurfaceError::None; }
};

struct NativeSurfaceConfig {
    SurfaceRef surface;
    NativeContentId contentId = 0;
};

// 单个系统视图的控制协议。所有调用在宿主 UI 线程顺序执行；实现不得在
// 调用返回后继续持有裸的调用方指针，close() 后不得再发出事件。
class IPlatformSurface {
public:
    virtual ~IPlatformSurface() = default;
    virtual SurfaceRef ref() const = 0;
    virtual SurfaceResult mount() = 0;
    virtual SurfaceResult setPlacement(const SurfacePlacement& placement) = 0;
    virtual SurfaceResult suspend() = 0;
    virtual SurfaceResult resume() = 0;
    virtual SurfaceState state() const = 0;
    virtual void close() = 0;
};

} // namespace svk
