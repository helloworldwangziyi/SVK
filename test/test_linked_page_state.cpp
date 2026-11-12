#include "linked_page_state.h"

#include <cassert>
#include <cstdio>

namespace {

void testContinuousRequests() {
    svk::LinkedPageState state({10, 20, 30, 40}, 10);
    const auto first = state.select(20);
    assert(first.type == svk::PageTransitionType::Animated);
    assert(first.fromIndex == 0 && first.targetIndex == 1);
    const auto generation = first.generation;

    const auto queued = state.select(30);
    assert(queued.type == svk::PageTransitionType::Queued);
    assert(queued.requestedIndex == 2);
    assert(state.state().activePage == 10);

    assert(state.complete(generation + 100).type == svk::PageTransitionType::Rejected);
    assert(state.state().activePage == 10);

    const auto second = state.complete(generation);
    assert(second.type == svk::PageTransitionType::Animated);
    assert(second.fromIndex == 1 && second.targetIndex == 2);
    const auto done = state.complete(second.generation);
    assert(done.type == svk::PageTransitionType::Completed);
    assert(done.activeIndex == 2 && done.activePage == 30);
    assert(!state.state().animating);
}

void testDirectAndDuplicate() {
    svk::LinkedPageState state({10, 20, 30, 40}, 10);
    const auto direct = state.select(40);
    assert(direct.type == svk::PageTransitionType::Immediate);
    assert(direct.activeIndex == 3);
    assert(state.select(40).type == svk::PageTransitionType::Rejected);
    assert(state.select(99).type == svk::PageTransitionType::Rejected);
}

void testReplacement() {
    svk::LinkedPageState state({10, 20, 30, 40}, 20);
    const auto first = state.select(30);
    assert(first.shouldAnimate());
    assert(state.replacePages({50, 20, 40}));
    assert(state.state().activePage == 20);
    assert(state.state().requestedPage == 20);
    assert(!state.state().animating);

    assert(state.replacePages({50, 60}));
    assert(state.state().activePage == 50);
    assert(!state.replacePages({50, 50}));
    assert(!state.replacePages({}));
    assert(state.state().pageCount == 2);
}

void testStoreLifecycle() {
    svk::LinkedPageStateStore store;
    const uint64_t handle = store.create({1, 2, 3}, 1);
    assert(handle != 0);
    assert(store.select(handle, 2).shouldAnimate());
    assert(store.settle(handle, 2).activePage == 2);
    assert(store.complete(handle, 999).type == svk::PageTransitionType::Rejected);
    assert(store.destroy(handle));
    assert(store.select(handle, 3).type == svk::PageTransitionType::Rejected);
    assert(!store.destroy(handle));
}

} // namespace

int main() {
    testContinuousRequests();
    testDirectAndDuplicate();
    testReplacement();
    testStoreLifecycle();
    std::puts("test_linked_page_state passed");
    return 0;
}
