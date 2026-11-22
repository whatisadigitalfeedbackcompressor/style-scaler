#pragma once

#include "engine/PatternBuffer.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace style_scaler {

/**
 * Wait-free single-slot pattern publisher (message/worker thread -> audio thread).
 * The audio thread performs only atomic loads/fetch_adds: zero locks, zero allocations,
 * zero deallocations.
 */
class LockFreePatternSlot {
public:
    LockFreePatternSlot() = default;

    ~LockFreePatternSlot() {
        delete active_.exchange(nullptr, std::memory_order_acq_rel);
        retired_.clear();
    }

    LockFreePatternSlot(const LockFreePatternSlot&) = delete;
    LockFreePatternSlot& operator=(const LockFreePatternSlot&) = delete;

    // Audio-thread scoped guard
    class ScopedAudioRead {
    public:
        explicit ScopedAudioRead(LockFreePatternSlot& slot) noexcept : slot_(slot) {
            slot_.audio_seq_.fetch_add(1, std::memory_order_acq_rel);
            buf_ = slot_.active_.load(std::memory_order_acquire);
        }

        ~ScopedAudioRead() noexcept {
            slot_.audio_seq_.fetch_add(1, std::memory_order_release);
        }

        ScopedAudioRead(const ScopedAudioRead&) = delete;
        ScopedAudioRead& operator=(const ScopedAudioRead&) = delete;

        const PatternBuffer* get() const noexcept { return buf_; }

    private:
        LockFreePatternSlot& slot_;
        const PatternBuffer* buf_ = nullptr;
    };

    // Message / background worker thread: publishes new immutable PatternBuffer
    uint64_t publish(std::unique_ptr<PatternBuffer> next) {
        std::lock_guard<std::mutex> lock(publish_mutex_);
        const uint64_t gen = next_gen_.fetch_add(1, std::memory_order_relaxed) + 1;
        next->generation = gen;

        PatternBuffer* old = active_.exchange(next.release(), std::memory_order_acq_rel);
        if (old != nullptr) {
            const uint64_t seq = audio_seq_.load(std::memory_order_acquire);
            retired_.push_back(RetiredEntry{std::unique_ptr<PatternBuffer>(old), seq});
        }
        reclaimLocked();
        return gen;
    }

    void reclaimRetired() {
        std::lock_guard<std::mutex> lock(publish_mutex_);
        reclaimLocked();
    }

    uint64_t latestGeneration() const noexcept {
        return next_gen_.load(std::memory_order_acquire);
    }

private:
    struct RetiredEntry {
        std::unique_ptr<PatternBuffer> ptr;
        uint64_t seq_at_retire = 0;
    };

    void reclaimLocked() {
        const uint64_t currentSeq = audio_seq_.load(std::memory_order_acquire);
        std::erase_if(retired_, [currentSeq](const RetiredEntry& entry) {
            // Safe when retired while audio thread was outside block (even seq)
            // or once audio thread has completed that block (currentSeq > seq_at_retire).
            return (entry.seq_at_retire & 1ULL) == 0ULL || currentSeq > entry.seq_at_retire;
        });
    }

    std::atomic<PatternBuffer*> active_{nullptr};
    std::atomic<uint64_t> audio_seq_{0};
    std::atomic<uint64_t> next_gen_{0};

    std::mutex publish_mutex_;
    std::vector<RetiredEntry> retired_;
};

} // namespace style_scaler
