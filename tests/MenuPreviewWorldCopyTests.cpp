#include "MenuPreview/DescriptorMap.hpp"
#include "MenuPreview/UiDescriptorCopyGate.hpp"
#include <atomic>
#include <barrier>
#include <cassert>
#include <iostream>
#include <mutex>
#include <thread>

using namespace cvr::menu_preview;
struct Handle { uintptr_t ptr{}; };

int main() {
    DescriptorMap<int,8> recent, persistent;
    Handle destination{0x1000}; unsigned one = 1, two = 2;
    const auto world = [] { return 0u; };
    const auto preview = [] { return 7u; };
    assert(UiCanSkipInactiveSingleCopy(1, &destination, &one, recent, persistent, world));
    assert(!UiCanSkipInactiveSingleCopy(1, &destination, &one, recent, persistent, preview));
    assert(!UiCanSkipInactiveSingleCopy(2, &destination, &one, recent, persistent, world));
    assert(!UiCanSkipInactiveSingleCopy(1, &destination, &two, recent, persistent, world));
    assert(!UiCanSkipInactiveSingleCopy(1, static_cast<Handle*>(nullptr), &one, recent, persistent, world));
    recent.emplace(destination.ptr, 0); // Pending/unknown provenance is still a tracked destination.
    assert(!UiCanSkipInactiveSingleCopy(1, &destination, &one, recent, persistent, world));
    recent.clear(); persistent.emplace(destination.ptr, 1);
    assert(!UiCanSkipInactiveSingleCopy(1, &destination, nullptr, recent, persistent, world));
    auto node = persistent.extract(destination.ptr); node.key() = 0x9000;
    assert(persistent.insert(std::move(node)).inserted && persistent.MayContain(0x9000));
    assert(persistent.find(destination.ptr) == persistent.end());
    persistent.clear(); assert(UiCanSkipInactiveSingleCopy(1, &destination, nullptr, recent, persistent, world));

    // Only non-owning hints are read concurrently. Map access retains its writer lock.
    DescriptorMap<int,8> concurrent;
    std::mutex writer;
    std::atomic<uintptr_t> published{}; std::atomic<bool> done{};
    std::barrier start(3);
    const auto read = [&] {
        start.arrive_and_wait();
        do {
            const auto key = published.load(std::memory_order_acquire);
            if (key) assert(concurrent.MayContain(key));
        } while (!done.load(std::memory_order_acquire));
    };
    std::thread a(read), b(read);
    start.arrive_and_wait();
    for (uintptr_t key = 1; key <= 8192; ++key) {
        std::lock_guard lock(writer); concurrent.emplace(key, int(key));
        published.store(key, std::memory_order_release);
    }
    done.store(true, std::memory_order_release); a.join(); b.join();
    for (uintptr_t key = 1; key <= 8192; ++key) assert(concurrent.MayContain(key));
    concurrent.clear(); assert(!concurrent.MayContain(1));

    // Saturation must remain conservative through erase, never wrap to a false negative.
    DescriptorMap<int,1> saturated;
    for (uintptr_t key = 1; key <= 200000; ++key) saturated.emplace(key, 1);
    for (auto it = saturated.begin(); it != saturated.end();) it = saturated.erase(it);
    assert(saturated.size() == 0 && saturated.MayContain(1) && saturated.MayContain(2));
    saturated.clear(); assert(!saturated.MayContain(1) && !saturated.MayContain(2));
    std::cout << "PASS world unknown-copy gate; active/bulk/pending/persistent rejection; concurrent non-owning hints; extraction and saturation\n";
}
