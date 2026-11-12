#pragma once

#include "linked_layout.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace svk {

enum class PageTransitionType { Rejected, Queued, Immediate, Animated, Completed };

struct PageTransition {
    PageTransitionType type = PageTransitionType::Rejected;
    PageId fromPage = 0;
    PageId targetPage = 0;
    PageId activePage = 0;
    PageId requestedPage = 0;
    int fromIndex = -1;
    int targetIndex = -1;
    int activeIndex = -1;
    int requestedIndex = -1;
    uint64_t generation = 0;
    size_t pageCount = 0;

    bool accepted() const { return type != PageTransitionType::Rejected; }
    bool shouldAnimate() const { return type == PageTransitionType::Animated; }
};

// 只保存分页决策，不驱动平台动画。调用方负责执行 Animated，并用 generation
// 完成一次且仅一次落定；Queued 只更新最新意图，不能启动第二个动画。
class LinkedPageState {
public:
    LinkedPageState(std::vector<PageId> pages, PageId initialPage);

    PageTransition select(PageId pageId);
    PageTransition complete(uint64_t generation);
    void settle(PageId pageId);
    bool replacePages(const std::vector<PageId>& pages);
    LinkedLayoutState state() const;
    const std::vector<PageId>& pages() const { return pages_; }
    int indexOf(PageId pageId) const;

private:
    PageTransition startNext();
    PageTransition transition(PageTransitionType type) const;

    std::vector<PageId> pages_;
    PageId activePage_ = 0;
    PageId animationTarget_ = 0;
    PageId requestedPage_ = 0;
    uint64_t generation_ = 0;
    bool animating_ = false;
};

// 每个平台宿主持有一个 Store；不在 core 中设置进程全局实例。所有调用限定在
// 宿主 UI 线程，布局销毁必须同时销毁对应状态句柄。
class LinkedPageStateStore {
public:
    uint64_t create(const std::vector<PageId>& pages, PageId initialPage);
    bool destroy(uint64_t handle);
    PageTransition select(uint64_t handle, PageId pageId);
    PageTransition complete(uint64_t handle, uint64_t generation);
    LinkedLayoutState settle(uint64_t handle, PageId pageId);
    LinkedLayoutState replacePages(uint64_t handle, const std::vector<PageId>& pages);
    LinkedLayoutState state(uint64_t handle) const;

private:
    LinkedPageState* find(uint64_t handle);
    const LinkedPageState* find(uint64_t handle) const;

    uint64_t nextHandle_ = 1;
    std::unordered_map<uint64_t, LinkedPageState> states_;
};

} // namespace svk
