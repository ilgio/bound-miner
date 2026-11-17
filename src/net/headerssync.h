#pragma once

#include "core/arith_uint256.h"
#include "core/block.h"
#include "core/chain.h"
#include "core/params.h"
#include "core/uint256.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <vector>

namespace bound {

struct HeadersSyncParams {
    int commitment_period = 64;            // one commitment bit per this many headers
    std::size_t redownload_buffer = 1536;  // headers held back in the redownload: 24 commitments
};

// Hardening: headers presync (Bitcoin Core's HeadersSyncState, v24).
//
// A peer offers a chain that leaves ours at `start` and doesn't have the work to be worth storing
// yet: less than `min_work` in total, counting `start` and its ancestors (PeerManager's anti-DoS
// threshold). Storing every header of a long, cheap
// chain is the flood this defends against, so the chain is downloaded twice:
//  1. Presync. Each header is checked as the block tree would check it (proof of work, the
//     difficulty the rules require, its timestamp) and its work is added up. For one header in
//     every commitment_period a single commitment bit is kept: a bit of a hash of its hash keyed
//     with a secret salt. Nothing else is kept. If the chain ends before reaching min_work it's
//     dropped; it never threatened ours.
//  2. Redownload. Once min_work is reached, the same headers are fetched again from `start`, checked
//     again, and matched against the commitments. A header is handed out for storing only after
//     redownload_buffer more have matched behind it, or once the second download has reached
//     min_work too. A peer that sends a different chain the second time is caught by a mismatch,
//     except with probability 2^-(redownload_buffer / commitment_period).
// Memory: a bit per commitment_period headers, plus the buffer.
class HeadersSync {
public:
    enum class Phase { Presync, Redownload, Done, Failed };

    // `start` must stay in the tree (nodes are never removed) and params must outlive this object.
    HeadersSync(const BlockIndex& start, const arith_uint256& min_work, const ConsensusParams& params,
                HeadersSyncParams sync, std::uint64_t salt, std::int64_t now);

    struct Result {
        std::vector<BlockHeader> to_store;  // checked twice and backed by the work: store them in this order
        bool invalid = false;               // a header broke a rule: the peer misbehaved
        std::string note;                   // what happened, when the phase changed or it failed
        std::vector<uint256> locator;       // ask the peer for headers after these; empty: ask nothing
    };
    // The peer's next headers message; an empty one means it has no more. A batch that doesn't
    // follow on from the last header (a new tip announced meanwhile) is ignored.
    Result process(std::span<const BlockHeader> headers, std::int64_t now);

    Phase phase() const { return phase_; }
    int presync_height() const { return presync_.height; }
    const arith_uint256& presync_work() const { return presync_.work; }

private:
    // One pass's position: enough of the chain so far to check the next header.
    struct Tip {
        uint256 hash;
        int height = 0;
        std::uint32_t bits = 0;
        std::int64_t time = 0;
        std::deque<std::int64_t> times;  // the latest headers' times above start, oldest first
        arith_uint256 work;              // above start
    };
    enum class Check { Ok, Invalid, TooNew };

    Tip tip_at_start() const;
    Check advance(Tip& tip, const BlockHeader& header, std::int64_t now, std::string& why) const;
    std::int64_t time_at(const Tip& tip, int height) const;
    bool commitment_bit(const uint256& hash) const;
    bool committed(int height) const { return height % sync_.commitment_period == offset_; }
    std::vector<uint256> locator_after(const Tip& tip) const;
    Result fail(Result r, std::string note);

    const BlockIndex& start_;
    arith_uint256 min_work_;  // above start_
    const ConsensusParams& params_;
    HeadersSyncParams sync_;
    std::uint64_t salt_;
    int offset_;                   // which height in each commitment_period is committed
    std::size_t max_commitments_;  // more headers than this can't be valid by now
    std::size_t times_kept_;
    Phase phase_ = Phase::Presync;
    Tip presync_;
    std::vector<bool> commitments_;
    Tip redownload_;
    std::size_t next_commitment_ = 0;
    std::deque<BlockHeader> buffer_;
    bool release_all_ = false;  // the second download has reached min_work
};

}  // namespace bound
