// =============================================================================
//  gpu_timer.cpp - GPU busy time per frame (see gpu_timer.h)
// =============================================================================
#include "perf/gpu_timer.h"

#include <windows.h>
#include <wrl/client.h>

#include <atomic>

#include "core/logger.h"
#include "perf/timer.h"

namespace luma::perf::gputimer {

namespace {

using Microsoft::WRL::ComPtr;

constexpr int kSlots = 4;          // frames in flight we can keep track of
constexpr UINT kMaxCalls = 32;     // submissions per frame we can measure (1.26.63 sends ~3)
constexpr UINT kMaxBatch = 62;     // lists per submission we can wrap (+2 of ours)
constexpr UINT kQueriesPerSlot = kMaxCalls * 2;

// One frame being measured. Its timestamps live in the query heap starting
// at "base" (one "before" and one "after" per submission).
struct Slot {
    ComPtr<ID3D12CommandAllocator> staticAllocator;   // for the timestamp lists (never reset)
    ComPtr<ID3D12CommandAllocator> resolveAllocator;  // for the resolve list (reset every use)
    ComPtr<ID3D12GraphicsCommandList> timestamps[kQueriesPerSlot];
    ComPtr<ID3D12GraphicsCommandList> resolve;
    UINT base = 0;
    UINT used = 0;         // submissions wrapped in this frame
    bool active = false;   // measuring the current frame in this slot
    bool overflow = false; // more submissions than kMaxCalls: the sum would be incomplete
    bool pending = false;  // sent to the GPU, results not read yet
    UINT64 fenceValue = 0;
};

std::atomic<std::uint32_t> g_wanted{0};
std::atomic<bool> g_ready{false};
std::atomic<bool> g_failed{false};
std::atomic<float> g_lastMs{-1.0f};
std::atomic<ID3D12CommandQueue*> g_queueRaw{nullptr};  // for the cheap "is it our queue?" check

// Everything below is protected by g_lock.
SRWLOCK g_lock = SRWLOCK_INIT;
ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12CommandQueue> g_queue;
ComPtr<ID3D12QueryHeap> g_heap;
ComPtr<ID3D12Resource> g_readback;
ComPtr<ID3D12Fence> g_fence;
HANDLE g_fenceEvent = nullptr;
UINT64 g_fenceCounter = 0;
double g_ticksPerSecond = 0.0;
Slot g_slots[kSlots];
int g_current = 0;
bool g_loggedFirst = false;

struct Lock {
    Lock() { AcquireSRWLockExclusive(&g_lock); }
    ~Lock() { ReleaseSRWLockExclusive(&g_lock); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

void ReleaseAll() {
    for (Slot& slot : g_slots) {
        slot = Slot{};
    }
    g_heap.Reset();
    g_readback.Reset();
    g_fence.Reset();
    if (g_fenceEvent != nullptr) {
        CloseHandle(g_fenceEvent);
        g_fenceEvent = nullptr;
    }
}

// Creates the query heap, the readback buffer and ALL the command lists, once.
// With g_lock held. Returns false (and leaves everything released) on failure.
bool Create() {
    const std::int64_t start = Now();
    UINT64 frequency = 0;
    if (FAILED(g_queue->GetTimestampFrequency(&frequency)) || frequency == 0) {
        log::Warn("GPU timer: this GPU queue doesn't support timestamps.");
        return false;
    }
    g_ticksPerSecond = static_cast<double>(frequency);

    D3D12_QUERY_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heapDesc.Count = kSlots * kQueriesPerSlot;
    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = static_cast<UINT64>(heapDesc.Count) * sizeof(UINT64);
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&g_heap))) ||
        FAILED(g_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_readback))) ||
        FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) {
        log::Warn("GPU timer: couldn't create the query heap or the readback buffer.");
        ReleaseAll();
        return false;
    }
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    for (int s = 0; s < kSlots; ++s) {
        Slot& slot = g_slots[s];
        slot.base = static_cast<UINT>(s) * kQueriesPerSlot;
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&slot.staticAllocator))) ||
            FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&slot.resolveAllocator))) ||
            FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.resolveAllocator.Get(),
                                               nullptr, IID_PPV_ARGS(&slot.resolve)))) {
            log::Warn("GPU timer: couldn't create the command lists.");
            ReleaseAll();
            return false;
        }
        slot.resolve->Close();
        // Each list does ONE thing: write the GPU clock into its own query.
        // A list can be executed again and again without recording it again.
        for (UINT q = 0; q < kQueriesPerSlot; ++q) {
            if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.staticAllocator.Get(),
                                                   nullptr, IID_PPV_ARGS(&slot.timestamps[q])))) {
                log::Warn("GPU timer: couldn't create the command lists.");
                ReleaseAll();
                return false;
            }
            slot.timestamps[q]->EndQuery(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot.base + q);
            slot.timestamps[q]->Close();
        }
    }
    g_current = 0;
    log::Info("GPU timer ready ({} command lists, {:.1f} ms, once).", kSlots * (kQueriesPerSlot + 1),
              TicksToMs(Now() - start));
    return true;
}

// The GPU finished this slot: add up its "after - before" pairs.
void ReadSlot(Slot& slot) {
    if (slot.used == 0) {
        return;
    }
    const SIZE_T begin = static_cast<SIZE_T>(slot.base) * sizeof(UINT64);
    const D3D12_RANGE range{begin, begin + static_cast<SIZE_T>(slot.used) * 2 * sizeof(UINT64)};
    void* mapped = nullptr;
    if (FAILED(g_readback->Map(0, &range, &mapped))) {
        return;
    }
    const auto* t = reinterpret_cast<const UINT64*>(static_cast<const char*>(mapped) + begin);
    UINT64 busy = 0;
    for (UINT i = 0; i < slot.used; ++i) {
        const UINT64 before = t[i * 2];
        const UINT64 after = t[i * 2 + 1];
        if (after >= before) {
            busy += after - before;
        }
    }
    const D3D12_RANGE nothingWritten{0, 0};
    g_readback->Unmap(0, &nothingWritten);
    const double ms = static_cast<double>(busy) / g_ticksPerSecond * 1000.0;
    if (ms >= 0.0 && ms < 1000.0) {
        g_lastMs.store(static_cast<float>(ms), std::memory_order_relaxed);
        if (!g_loggedFirst) {
            g_loggedFirst = true;
            log::Info("GPU timer: first frame measured, {} submissions, GPU busy {:.2f} ms.", slot.used, ms);
        }
    }
}

}  // namespace

void Want(Source source, bool want) {
    if (want) {
        g_wanted.fetch_or(source);
    } else {
        g_wanted.fetch_and(~static_cast<std::uint32_t>(source));
    }
}

void Attach(ID3D12Device* device, ID3D12CommandQueue* queue) {
    Lock lock;
    if (g_device) {
        return;  // already attached (the overlay is initialized only once)
    }
    g_device = device;
    g_queue = queue;
    g_queueRaw.store(queue);
}

void Execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists, ExecuteFn original) {
    // Fast path: nobody wants it, it isn't ready, or it's another queue.
    if (g_wanted.load(std::memory_order_relaxed) == 0 || !g_ready.load(std::memory_order_acquire) ||
        queue != g_queueRaw.load(std::memory_order_relaxed) || count == 0 || count > kMaxBatch) {
        original(queue, count, lists);
        return;
    }
    Lock lock;
    Slot& slot = g_slots[g_current];
    if (!g_ready.load(std::memory_order_relaxed) || !slot.active) {
        original(queue, count, lists);
        return;
    }
    if (slot.used >= kMaxCalls) {
        slot.overflow = true;
        original(queue, count, lists);
        return;
    }
    // [our "before"] [the game's lists...] [our "after"], in one submission so
    // the GPU runs them back to back. The array lives on the stack: no memory
    // is allocated. The lock stays held so the frame's resolve (OnPresent)
    // can't be sent in between.
    ID3D12CommandList* wrapped[kMaxBatch + 2];
    wrapped[0] = slot.timestamps[slot.used * 2].Get();
    for (UINT i = 0; i < count; ++i) {
        wrapped[i + 1] = lists[i];
    }
    wrapped[count + 1] = slot.timestamps[slot.used * 2 + 1].Get();
    ++slot.used;
    original(queue, count + 2, wrapped);
}

void OnPresent(ExecuteFn original) {
    const bool wanted = g_wanted.load(std::memory_order_relaxed) != 0;
    if (!g_ready.load(std::memory_order_acquire)) {
        if (!wanted || g_failed.load(std::memory_order_relaxed)) {
            return;
        }
        Lock lock;
        if (!g_device || !g_queue) {
            return;  // the overlay hasn't attached yet
        }
        if (Create()) {
            g_ready.store(true, std::memory_order_release);
        } else {
            g_failed.store(true);
        }
        return;
    }

    Lock lock;
    if (!g_ready.load(std::memory_order_relaxed)) {
        return;
    }
    // 1. Close the frame that just ended: copy its timestamps to the readback
    //    buffer and mark when the GPU is done with it.
    Slot& current = g_slots[g_current];
    if (current.active) {
        current.active = false;
        if (current.used > 0 && !current.overflow) {
            current.resolveAllocator->Reset();
            current.resolve->Reset(current.resolveAllocator.Get(), nullptr);
            current.resolve->ResolveQueryData(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, current.base,
                                              current.used * 2, g_readback.Get(),
                                              static_cast<UINT64>(current.base) * sizeof(UINT64));
            current.resolve->Close();
            ID3D12CommandList* resolveList[] = {current.resolve.Get()};
            original(g_queue.Get(), 1, resolveList);
            current.fenceValue = ++g_fenceCounter;
            g_queue->Signal(g_fence.Get(), current.fenceValue);
            current.pending = true;
        } else if (current.overflow) {
            g_lastMs.store(-1.0f, std::memory_order_relaxed);  // can't give a complete number
        }
    }
    // 2. Move to the next slot. If the GPU already finished it, read it; if
    //    not, this frame isn't measured (we never wait for the GPU).
    g_current = (g_current + 1) % kSlots;
    Slot& next = g_slots[g_current];
    if (next.pending && g_fence->GetCompletedValue() >= next.fenceValue) {
        ReadSlot(next);
        next.pending = false;
    }
    if (!next.pending && wanted) {
        next.active = true;
        next.used = 0;
        next.overflow = false;
    }
    if (!wanted) {
        g_lastMs.store(-1.0f, std::memory_order_relaxed);
    }
}

float LastFrameMs() {
    return g_lastMs.load(std::memory_order_relaxed);
}

void Shutdown() {
    Lock lock;
    if (g_ready.exchange(false) && g_fence && g_queue) {
        // The queue runs in order: once this value is reached, the GPU is done
        // with every list of ours that was sent before it.
        const UINT64 value = ++g_fenceCounter;
        if (SUCCEEDED(g_queue->Signal(g_fence.Get(), value)) && g_fence->GetCompletedValue() < value &&
            g_fenceEvent != nullptr) {
            g_fence->SetEventOnCompletion(value, g_fenceEvent);
            WaitForSingleObject(g_fenceEvent, 2000);  // 2 s at most, for safety
        }
    }
    ReleaseAll();
    g_queueRaw.store(nullptr);
    g_queue.Reset();
    g_device.Reset();
    g_failed.store(false);
    g_loggedFirst = false;
    g_lastMs.store(-1.0f);
}

}  // namespace luma::perf::gputimer
