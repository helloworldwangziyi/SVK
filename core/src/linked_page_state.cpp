#include "linked_page_state.h"

#include <algorithm>

namespace svk {
namespace {

bool validPages(const std::vector<PageId>& pages) {
    if (pages.empty()) return false;
    for (size_t index = 0; index < pages.size(); ++index) {
        if (pages[index] == 0) return false;
        for (size_t other = index + 1; other < pages.size(); ++other) {
            if (pages[index] == pages[other]) return false;
        }
    }
    return true;
}

} // namespace

LinkedPageState::LinkedPageState(std::vector<PageId> pages, PageId initialPage)
    : pages_(std::move(pages)) {
    activePage_ = indexOf(initialPage) >= 0 ? initialPage : pages_.front();
    animationTarget_ = activePage_;
    requestedPage_ = activePage_;
}

int LinkedPageState::indexOf(PageId pageId) const {
    const auto found = std::find(pages_.begin(), pages_.end(), pageId);
    return found == pages_.end() ? -1 : static_cast<int>(found - pages_.begin());
}

PageTransition LinkedPageState::transition(PageTransitionType type) const {
    PageTransition result;
    result.type = type;
    result.fromPage = activePage_;
    result.targetPage = animationTarget_;
    result.activePage = activePage_;
    result.requestedPage = requestedPage_;
    result.fromIndex = indexOf(activePage_);
    result.targetIndex = indexOf(animationTarget_);
    result.activeIndex = indexOf(activePage_);
    result.requestedIndex = indexOf(requestedPage_);
    result.generation = generation_;
    result.pageCount = pages_.size();
    return result;
}

PageTransition LinkedPageState::startNext() {
    if (requestedPage_ == activePage_) return transition(PageTransitionType::Completed);
    const int fromIndex = indexOf(activePage_);
    const int targetIndex = indexOf(requestedPage_);
    if (fromIndex < 0 || targetIndex < 0) return transition(PageTransitionType::Rejected);
    animationTarget_ = requestedPage_;
    ++generation_;
    if (std::abs(targetIndex - fromIndex) == 1) {
        animating_ = true;
        return transition(PageTransitionType::Animated);
    }
    activePage_ = animationTarget_;
    animating_ = false;
    return transition(PageTransitionType::Immediate);
}

PageTransition LinkedPageState::select(PageId pageId) {
    if (indexOf(pageId) < 0 || pageId == requestedPage_) {
        return transition(PageTransitionType::Rejected);
    }
    requestedPage_ = pageId;
    if (animating_) return transition(PageTransitionType::Queued);
    return startNext();
}

PageTransition LinkedPageState::complete(uint64_t generation) {
    if (!animating_ || generation != generation_) {
        return transition(PageTransitionType::Rejected);
    }
    activePage_ = animationTarget_;
    animating_ = false;
    if (requestedPage_ == activePage_) return transition(PageTransitionType::Completed);
    return startNext();
}

void LinkedPageState::settle(PageId pageId) {
    if (indexOf(pageId) < 0) return;
    ++generation_;
    activePage_ = pageId;
    animationTarget_ = pageId;
    requestedPage_ = pageId;
    animating_ = false;
}

bool LinkedPageState::replacePages(const std::vector<PageId>& pages) {
    if (!validPages(pages)) return false;
    PageId retained = 0;
    if (std::find(pages.begin(), pages.end(), requestedPage_) != pages.end()) {
        retained = requestedPage_;
    } else if (std::find(pages.begin(), pages.end(), activePage_) != pages.end()) {
        retained = activePage_;
    } else {
        const int oldIndex = indexOf(requestedPage_);
        for (int index = oldIndex + 1; index < static_cast<int>(pages_.size()); ++index) {
            if (std::find(pages.begin(), pages.end(), pages_[index]) != pages.end()) {
                retained = pages_[index];
                break;
            }
        }
        if (retained == 0) {
            for (int index = oldIndex - 1; index >= 0; --index) {
                if (std::find(pages.begin(), pages.end(), pages_[index]) != pages.end()) {
                    retained = pages_[index];
                    break;
                }
            }
        }
    }
    if (retained == 0) retained = pages.front();
    pages_ = std::move(pages);
    settle(retained);
    return true;
}

LinkedLayoutState LinkedPageState::state() const {
    LinkedLayoutState result;
    result.activePage = activePage_;
    result.animationTarget = animationTarget_;
    result.requestedPage = requestedPage_;
    result.generation = generation_;
    result.animating = animating_;
    result.pageCount = pages_.size();
    return result;
}

uint64_t LinkedPageStateStore::create(const std::vector<PageId>& pages, PageId initialPage) {
    if (!validPages(pages)) return 0;
    const uint64_t handle = nextHandle_++;
    states_.emplace(handle, LinkedPageState(pages, initialPage));
    return handle;
}

bool LinkedPageStateStore::destroy(uint64_t handle) {
    return states_.erase(handle) != 0;
}

LinkedPageState* LinkedPageStateStore::find(uint64_t handle) {
    const auto found = states_.find(handle);
    return found == states_.end() ? nullptr : &found->second;
}

const LinkedPageState* LinkedPageStateStore::find(uint64_t handle) const {
    const auto found = states_.find(handle);
    return found == states_.end() ? nullptr : &found->second;
}

PageTransition LinkedPageStateStore::select(uint64_t handle, PageId pageId) {
    LinkedPageState* state = find(handle);
    return state ? state->select(pageId) : PageTransition{};
}

PageTransition LinkedPageStateStore::complete(uint64_t handle, uint64_t generation) {
    LinkedPageState* state = find(handle);
    return state ? state->complete(generation) : PageTransition{};
}

LinkedLayoutState LinkedPageStateStore::settle(uint64_t handle, PageId pageId) {
    LinkedPageState* state = find(handle);
    if (!state) return LinkedLayoutState{};
    state->settle(pageId);
    return state->state();
}

LinkedLayoutState LinkedPageStateStore::replacePages(
    uint64_t handle, const std::vector<PageId>& pages) {
    LinkedPageState* state = find(handle);
    if (!state || !state->replacePages(pages)) return LinkedLayoutState{};
    return state->state();
}

LinkedLayoutState LinkedPageStateStore::state(uint64_t handle) const {
    const LinkedPageState* state = find(handle);
    return state ? state->state() : LinkedLayoutState{};
}

} // namespace svk
