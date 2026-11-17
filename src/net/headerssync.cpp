#include "net/headerssync.h"

#include "core/pow.h"
#include "crypto/sha256.h"
#include "util/serialize.h"

#include <algorithm>
#include <format>
#include <utility>

namespace bound {

HeadersSync::HeadersSync(const BlockIndex& start, const arith_uint256& min_work, const ConsensusParams& params,
                         HeadersSyncParams sync, std::uint64_t salt, std::int64_t now)
    : start_(start),
      min_work_(min_work),
      params_(params),
      sync_(sync),
      salt_(salt),
      offset_(static_cast<int>((salt >> 32) % static_cast<std::uint64_t>(sync.commitment_period))),
      times_kept_(static_cast<std::size_t>(std::max(params.retarget_interval + 1, params.median_time_span))),
      presync_(tip_at_start()),
      redownload_(tip_at_start()) {
    // min_work is a whole chain's work; what's counted here is the work above start.
    min_work_ = min_work > start.chain_work ? min_work - start.chain_work : arith_uint256(0);
    // Each header's time must pass the median of the 11 before it, so a chain can't gain more than
    // about 6 headers per second on average without running into the future (Core's bound too).
    const std::int64_t seconds = now + params.max_future_block_time - start.median_time_past(params.median_time_span);
    max_commitments_ = static_cast<std::size_t>(std::max<std::int64_t>(0, 6 * seconds / sync.commitment_period)) + 1;
}

HeadersSync::Tip HeadersSync::tip_at_start() const {
    Tip tip;
    tip.hash = start_.hash;
    tip.height = start_.height;
    tip.bits = start_.header.bits;
    tip.time = start_.header.time;
    return tip;
}

std::int64_t HeadersSync::time_at(const Tip& tip, int height) const {
    if (height <= start_.height) return start_.ancestor(height)->header.time;
    const int oldest = tip.height - static_cast<int>(tip.times.size()) + 1;  // > start_.height
    return tip.times.at(static_cast<std::size_t>(height - oldest));
}

// The checks Chainstate::check_header makes, against a chain that isn't in the tree.
HeadersSync::Check HeadersSync::advance(Tip& tip, const BlockHeader& header, std::int64_t now, std::string& why) const {
    if (header.prev_hash != tip.hash) {
        why = "headers that don't follow on from each other";
        return Check::Invalid;
    }
    const uint256 hash = header.hash();
    if (!check_proof_of_work(header, params_)) {
        why = "bad proof of work";
        return Check::Invalid;
    }
    const auto time_of = [&](int h) { return time_at(tip, h); };
    if (header.bits != next_work_required(tip.height, tip.bits, tip.time, time_of, params_)) {
        why = "wrong difficulty";
        return Check::Invalid;
    }
    std::vector<std::int64_t> recent;
    for (int h = tip.height; h >= 0 && static_cast<int>(recent.size()) < params_.median_time_span; --h) recent.push_back(time_of(h));
    std::sort(recent.begin(), recent.end());
    if (header.time <= recent[recent.size() / 2]) {
        why = "time too old";
        return Check::Invalid;
    }
    if (static_cast<std::int64_t>(header.time) > now + params_.max_future_block_time) {
        why = "a header from the future";
        return Check::TooNew;  // maybe our clock: not the peer's fault
    }

    tip.hash = hash;
    ++tip.height;
    tip.bits = header.bits;
    tip.time = header.time;
    tip.times.push_back(header.time);
    if (tip.times.size() > times_kept_) tip.times.pop_front();
    tip.work += block_proof(header.bits);
    return Check::Ok;
}

bool HeadersSync::commitment_bit(const uint256& hash) const {
    ByteWriter w;
    w.u64(salt_);
    w.bytes(hash.bytes());
    return (tagged_hash("BOUND/headers-commitment", w.data())[0] & 1) != 0;
}

std::vector<uint256> HeadersSync::locator_after(const Tip& tip) const {
    std::vector<uint256> locator{tip.hash};
    for (const uint256& h : block_locator(start_)) locator.push_back(h);  // in case the peer moved off it
    return locator;
}

HeadersSync::Result HeadersSync::fail(Result r, std::string note) {
    phase_ = Phase::Failed;
    r.note = std::move(note);
    r.locator.clear();
    return r;
}

HeadersSync::Result HeadersSync::process(std::span<const BlockHeader> headers, std::int64_t now) {
    Result r;
    if (phase_ == Phase::Done || phase_ == Phase::Failed) return r;
    const bool presync = phase_ == Phase::Presync;
    Tip& tip = presync ? presync_ : redownload_;

    if (headers.empty()) {
        if (presync) {
            return fail(std::move(r), std::format("dropped a chain of {} headers with too little work to store", presync_.height - start_.height));
        }
        if (!release_all_) return fail(std::move(r), "the second download fell short of the first");
        while (!buffer_.empty()) {
            r.to_store.push_back(buffer_.front());
            buffer_.pop_front();
        }
        phase_ = Phase::Done;
        return r;
    }
    if (headers.front().prev_hash != tip.hash) return r;  // not the reply we asked for

    for (const BlockHeader& header : headers) {
        std::string why;
        const Check check = advance(tip, header, now, why);
        if (check != Check::Ok) {
            r.invalid = check == Check::Invalid;
            return fail(std::move(r), why);
        }
        if (presync) {
            if (committed(tip.height)) {
                if (commitments_.size() >= max_commitments_) return fail(std::move(r), "more headers than the time since the fork allows");
                commitments_.push_back(commitment_bit(tip.hash));
            }
            if (tip.work >= min_work_) {
                phase_ = Phase::Redownload;
                r.note = std::format("{} headers carry enough work; downloading them again to store them", tip.height - start_.height);
                r.locator = block_locator(start_);
                return r;
            }
        } else {
            if (tip.height <= presync_.height && committed(tip.height)) {
                if (next_commitment_ >= commitments_.size() || commitments_[next_commitment_++] != commitment_bit(tip.hash)) {
                    return fail(std::move(r), "the headers changed between the two downloads");
                }
            }
            buffer_.push_back(header);
            if (tip.work >= min_work_) release_all_ = true;
        }
    }

    if (!presync) {
        while (!buffer_.empty() && (release_all_ || buffer_.size() > sync_.redownload_buffer)) {
            r.to_store.push_back(buffer_.front());
            buffer_.pop_front();
        }
        if (release_all_) {
            phase_ = Phase::Done;  // the rest of the chain, if any, syncs as usual: it has the work now
            return r;
        }
    }
    r.locator = locator_after(tip);
    return r;
}

}  // namespace bound
