// SPDX-FileCopyrightText: 2026 andrew
// SPDX-License-Identifier: BSD-3-Clause
#include "store.hpp"
#include <readerwriterqueue.h>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace memory {
struct Writer::Impl {
    // Keys live in stable map nodes until after the worker is joined. The worker
    // only reads the immutable key through the message, never the registry/map.
    struct Slot {
        Geometry latest;
        const Key *key = nullptr;
        Slot *next = nullptr;
        bool pending = false;
    };
    struct Message {
        enum class Kind { Update, Barrier } kind = Kind::Update;
        const Key *key = nullptr;
        Geometry size;
        uint64_t ticket = 0;
    };
    static_assert(std::is_trivially_copyable_v<Message>);
    static_assert(std::atomic<uint64_t>::is_always_lock_free);

    const std::thread::id owner = std::this_thread::get_id();
    const std::filesystem::path path;
    const WriterOptions options;
    std::map<Key, Slot> registry;
    Slot *head = nullptr;
    Slot *tail = nullptr;
    uint64_t nextTicket = 0, pendingTicket = 0;
    uint64_t updates = 0, coalesced = 0, overflows = 0;
    moodycamel::ReaderWriterQueue<Message> queue;
    std::binary_semaphore wake{0};
    std::atomic<bool> wakePending{false};
    std::atomic<uint64_t> progress{0}, completed{0}, writes{0}, wakeups{0};
    std::atomic<bool> errorFlag{false};
    std::mutex errorMutex;
    std::string failure;
    Geometries state;    // worker-owned after construction
    std::jthread thread; // joined before registry and queue are destroyed

    static std::size_t capacity(std::size_t value) {
        if (value == 0 || value > 65536)
            throw std::invalid_argument("queue capacity must be in 1..65536");
        return value;
    }
    Impl(std::filesystem::path p, Geometries initial, WriterOptions opts)
        : path(std::move(p)), options(std::move(opts)), queue(capacity(options.queueCapacity)),
          state(std::move(initial)) {
        if (options.debounce.count() < 0)
            throw std::invalid_argument("negative debounce interval");
        thread = std::jthread([this](std::stop_token stop) { run(stop); });
    }
    void checkOwner() const {
        if (owner != std::this_thread::get_id())
            throw std::logic_error("Writer requires its creating thread");
    }
    void notify() {
        // At most one outstanding token; clear only after consuming it. A producer
        // racing that clear either relies on this wake or publishes the next one.
        if (!wakePending.exchange(true, std::memory_order_acq_rel))
            wake.release();
    }
    bool needsPump() const { return head || pendingTicket; }
    void pump() {
        bool sent = false;
        while (head) {
            if (!queue.try_enqueue(Message{Message::Kind::Update, head->key, head->latest, 0}))
                break;
            auto *slot = head;
            head = slot->next;
            slot->next = nullptr;
            slot->pending = false;
            if (!head)
                tail = nullptr;
            sent = true;
        }
        // A barrier cannot pass any accepted updates still in the overflow list.
        if (!head && pendingTicket && queue.try_enqueue(Message{Message::Kind::Barrier, nullptr, {}, pendingTicket})) {
            pendingTicket = 0;
            sent = true;
        }
        if (sent)
            notify();
    }
    void submit(const Key &key, Geometry size) {
        checkOwner();
        pump();
        auto [it, inserted] = registry.try_emplace(key);
        auto &slot = it->second;
        if (inserted)
            slot.key = &it->first;
        ++updates;
        if (slot.pending) {
            slot.latest = size;
            ++coalesced;
            return;
        }
        if (queue.try_enqueue(Message{Message::Kind::Update, slot.key, size, 0})) {
            notify();
            return;
        }
        ++overflows;
        slot.latest = size;
        slot.pending = true;
        if (tail)
            tail->next = &slot;
        else
            head = &slot;
        tail = &slot;
    }
    uint64_t requestFlush() {
        checkOwner();
        const auto ticket = ++nextTicket;
        pendingTicket = ticket;
        pump();
        return ticket;
    }
    Result<void> flush() {
        const auto ticket = requestFlush();
        while (needsPump()) {
            const auto observed = progress.load(std::memory_order_acquire);
            pump();
            if (needsPump())
                progress.wait(observed, std::memory_order_acquire);
        }
        auto observed = completed.load(std::memory_order_acquire);
        while (observed < ticket) {
            completed.wait(observed, std::memory_order_acquire);
            observed = completed.load(std::memory_order_acquire);
        }
        std::lock_guard lock(errorMutex);
        if (!failure.empty())
            return std::unexpected(failure);
        return {};
    }
    void run(std::stop_token stop) {
        using Clock = std::chrono::steady_clock;
        std::optional<Clock::time_point> due;
        bool dirty = false;
        auto persist = [&] {
            due.reset();
            if (!dirty)
                return;
            Result<void> result;
            try {
                result = options.sink ? options.sink(path, state) : write(path, state);
            } catch (const std::exception &e) {
                result = std::unexpected(e.what());
            }
            writes.fetch_add(1, std::memory_order_relaxed);
            dirty = !result;
            {
                std::lock_guard lock(errorMutex);
                failure = result ? std::string{} : std::move(result.error());
            }
            errorFlag.store(dirty, std::memory_order_release);
        };
        for (;;) {
            const bool signalled = due ? wake.try_acquire_until(*due) : (wake.acquire(), true);
            if (signalled) {
                wakePending.store(false, std::memory_order_release);
                wakeups.fetch_add(1, std::memory_order_relaxed);
            }
            Message message;
            while (queue.try_dequeue(message)) {
                progress.fetch_add(1, std::memory_order_release);
                progress.notify_all();
                if (message.kind == Message::Kind::Barrier) {
                    persist();
                    completed.store(message.ticket, std::memory_order_release);
                    completed.notify_all();
                } else {
                    auto [it, inserted] = state.try_emplace(*message.key, message.size);
                    dirty |= inserted || it->second != message.size;
                    it->second = message.size;
                    if (dirty)
                        due = Clock::now() + options.debounce;
                }
            }
            if (stop.stop_requested())
                return; // the producer flushes before stop
            if (due && Clock::now() >= *due)
                persist();
        }
    }
};
Writer::Writer(std::filesystem::path path, Geometries initial, WriterOptions options)
    : impl_(std::make_unique<Impl>(std::move(path), std::move(initial), std::move(options))) {}
Writer::~Writer() {
    (void)impl_->flush();
    impl_->thread.request_stop();
    impl_->notify();
    impl_->thread.join();
}
void Writer::submit(const Key &key, Geometry size) { impl_->submit(key, size); }
void Writer::pump() {
    impl_->checkOwner();
    impl_->pump();
}
bool Writer::needsPump() const {
    impl_->checkOwner();
    return impl_->needsPump();
}
void Writer::flushAsync() { (void)impl_->requestFlush(); }
Result<void> Writer::flush() { return impl_->flush(); }
bool Writer::hasError() const noexcept { return impl_->errorFlag.load(std::memory_order_acquire); }
std::string Writer::error() {
    std::lock_guard lock(impl_->errorMutex);
    return impl_->failure;
}
WriterStats Writer::stats() {
    impl_->checkOwner();
    return {impl_->updates, impl_->coalesced, impl_->wakeups.load(std::memory_order_relaxed),
            impl_->writes.load(std::memory_order_relaxed), impl_->overflows};
}
} // namespace memory
