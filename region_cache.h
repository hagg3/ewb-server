// region_cache.h — the encoded-region cache: a `REGION` reply kept as the `SNAPZ`
// lines it was sent as, so the next request for the same box costs no scan, no
// deflate and no base64 (ROADMAP-SERVER stage 12.1b, PERF-005).
//
// Pure and header-only, tested offline by `region_cache_test.cpp`. The server
// (`server_posix.cpp`) supplies the locks: `ColumnGens` is guarded by `g_worldMtx`
// (it is bumped where the world is written), `RegionCache` by its own mutex, and
// the lock order is world → cache.
//
// ## Why it pays
//
// Requests repeat. Every player who joins at the same spawn asks for the same box,
// players in one area ask for overlapping-but-equal boxes (the reply is chunk-aligned,
// so nearby request points snap to the same box), and back-and-forth travel re-asks
// for boxes it has already had. Each of those used to cost a full scan + encode —
// 178 ms of deflate at level 6 for an alpinecraft-sized spawn, paid again by every
// joiner, for byte-identical replies.
//
// ## Why a hit is exactly a fresh reply
//
// Since stage 12.1a the scan emits records in wire order (no sort, no hash order),
// so a fresh reply is a deterministic function of the box, the world inside it and
// the deflate level. The cache stores that function's output and serves it only
// while the world inside the box is unchanged — so a hit is byte-identical to what
// a fresh scan would produce at that moment.
//
// ## Invalidation: column generations
//
// Every write to the world bumps a generation counter and stamps it on the write's
// chunk column (`ColumnGens::bump`, O(1), under the world lock). A cache entry
// carries the counter's value at the moment its box was scanned. It is still valid
// iff no column the box covers has a newer stamp — checked under the world lock on
// every lookup, so an edit that lands between the scan and the cache insert (the
// writer inserts much later, after the last frame is sent) can never be served:
// the entry is inserted, found stale on its first lookup, and dropped.
//
// The column table is a fixed 256 x 256 tile of chunk columns (4096 x 4096 blocks)
// that the world folds onto. Two columns exactly a multiple of 4096 blocks apart
// share a slot, so an edit there also invalidates the other's boxes: a spurious
// miss, never a stale hit. The table is 512 KB whatever the world's size, needs no
// allocation per edit, and a box (29 x 29 columns at the default radius) never
// aliases itself.
//
// Signs are not in `SNAPZ` (they ride `SIGNQ`), so they do not touch any of this.

#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "region_query.h"   // RegionBox

namespace ewb {

// --- invalidation ------------------------------------------------------------

class ColumnGens {
public:
    static constexpr int TILE = 256;   ///< chunk columns per tile edge

    ColumnGens() : gen_((size_t)TILE * TILE, 0) {}

    /// The generation a scan taken now would be stamped with.
    uint64_t now() const { return now_; }

    /// A cell at block (x, z) changed.
    void bump(int x, int z) { gen_[slot(x >> 4, z >> 4)] = ++now_; }

    /// Everything changed (a whole-world reload). Every entry stamped before this
    /// is stale.
    void bump_all() { floor_ = ++now_; }

    /// The newest generation of any column `b` covers.
    uint64_t newest_in(const RegionBox& b) const {
        uint64_t m = floor_;
        const int cx0 = b.x0 >> 4, cz0 = b.z0 >> 4;
        // Past one tile edge every slot repeats, so cap the sweep there.
        const int nx = clamp_span((b.x1 >> 4) - cx0 + 1);
        const int nz = clamp_span((b.z1 >> 4) - cz0 + 1);
        for (int i = 0; i < nx; ++i)
            for (int j = 0; j < nz; ++j) {
                const uint64_t g = gen_[slot(cx0 + i, cz0 + j)];
                if (g > m) m = g;
            }
        return m;
    }

    /// Nothing in `b` has changed since a scan stamped `stamp`.
    bool unchanged_since(const RegionBox& b, uint64_t stamp) const { return newest_in(b) <= stamp; }

private:
    static size_t slot(int cx, int cz) {
        return ((size_t)((unsigned)cx & (TILE - 1)) << 8) | (size_t)((unsigned)cz & (TILE - 1));
    }
    static int clamp_span(int n) { return n < 0 ? 0 : (n > TILE ? TILE : n); }

    std::vector<uint64_t> gen_;
    uint64_t now_   = 0;
    uint64_t floor_ = 0;
};

// --- entries -----------------------------------------------------------------

/// What a reply is a function of, besides the world: the box (the radius is in its
/// corners) and whether an empty box is answered with `SNAPZ:0:`. The deflate level
/// is fixed for a process, so it is not part of the key.
struct RegionCacheKey {
    int  x0 = 0, x1 = 0, z0 = 0, z1 = 0;
    bool empty_frame = false;

    static RegionCacheKey of(const RegionBox& b, bool empty_frame) {
        return {b.x0, b.x1, b.z0, b.z1, empty_frame};
    }
    RegionBox box() const { return {x0, x1, z0, z1}; }
    bool operator==(const RegionCacheKey& o) const {
        return x0 == o.x0 && x1 == o.x1 && z0 == o.z0 && z1 == o.z1 && empty_frame == o.empty_frame;
    }
};

struct RegionCacheKeyHash {
    size_t operator()(const RegionCacheKey& k) const {
        uint64_t h = 1469598103934665603ull;
        for (uint64_t v : {(uint64_t)(uint32_t)k.x0, (uint64_t)(uint32_t)k.x1,
                           (uint64_t)(uint32_t)k.z0, (uint64_t)(uint32_t)k.z1,
                           (uint64_t)k.empty_frame}) {
            h ^= v;
            h *= 1099511628211ull;
        }
        return (size_t)h;
    }
};

/// One encoded `SNAPZ` line and how many records it carries.
struct CachedFrame {
    std::string line;       ///< the whole wire line, `\n` included
    size_t      records = 0;
};

/// A whole reply as it went out. Immutable once built; queues share it.
struct CachedRegion {
    std::vector<CachedFrame> frames;
    size_t   bytes   = 0;   ///< sum of every line's size
    size_t   records = 0;   ///< sum of every frame's records
    uint64_t stamp   = 0;   ///< `ColumnGens::now()` when the box was scanned

    void add(std::string line, size_t recs) {
        bytes += line.size();
        records += recs;
        frames.push_back({std::move(line), recs});
    }
};

/// What a fill carries from the scan to the writer that encodes it: where to
/// file the reply, and when its box was scanned.
struct RegionCacheTag {
    RegionCacheKey key;
    uint64_t       stamp = 0;
};

// --- the cache ---------------------------------------------------------------

/// LRU, bounded by the encoded bytes it holds. An entry larger than a quarter of the
/// bound is never stored: one huge box would otherwise evict everything else to
/// make room for a reply that is rarely asked for twice.
class RegionCache {
public:
    struct Stats {
        uint64_t hits = 0, misses = 0, stale = 0, inserts = 0, evictions = 0, oversize = 0;
    };

    explicit RegionCache(size_t max_bytes = 0) : max_bytes_(max_bytes) {}

    bool   enabled()         const { return max_bytes_ > 0; }
    size_t max_bytes()       const { return max_bytes_; }
    size_t max_entry_bytes() const { return max_bytes_ / 4; }
    size_t bytes()           const { return bytes_; }
    size_t entries()         const { return map_.size(); }
    const Stats& stats()     const { return st_; }

    void set_max_bytes(size_t n) { max_bytes_ = n; evict_to(max_bytes_); }

    /// The entry for `k` if there is one and nothing in its box has changed since
    /// it was scanned (caller holds whatever guards `gens`). A stale entry is
    /// dropped here. Counts a hit, a miss or a stale miss.
    std::shared_ptr<const CachedRegion> find(const RegionCacheKey& k, const ColumnGens& gens) {
        if (!enabled()) return nullptr;
        auto it = map_.find(k);
        if (it == map_.end()) { ++st_.misses; return nullptr; }
        if (!gens.unchanged_since(k.box(), it->second.entry->stamp)) {
            ++st_.stale;
            erase(it);
            return nullptr;
        }
        order_.splice(order_.begin(), order_, it->second.pos);   // most recently used
        ++st_.hits;
        return it->second.entry;
    }

    /// File a finished reply. False (nothing stored) when the cache is off, the
    /// reply is over `max_entry_bytes()`, or the cache already holds a reply for
    /// `k` scanned at least as recently.
    bool insert(const RegionCacheKey& k, std::shared_ptr<const CachedRegion> e) {
        if (!enabled() || !e) return false;
        if (e->bytes > max_entry_bytes()) { ++st_.oversize; return false; }
        auto it = map_.find(k);
        if (it != map_.end()) {
            if (it->second.entry->stamp >= e->stamp) return false;
            erase(it);
        }
        evict_to(max_bytes_ - e->bytes);
        order_.push_front(k);
        bytes_ += e->bytes;
        map_.emplace(k, Node{std::move(e), order_.begin()});
        ++st_.inserts;
        return true;
    }

    void clear() { map_.clear(); order_.clear(); bytes_ = 0; }

private:
    struct Node {
        std::shared_ptr<const CachedRegion> entry;
        std::list<RegionCacheKey>::iterator pos;
    };
    using Map = std::unordered_map<RegionCacheKey, Node, RegionCacheKeyHash>;

    void erase(Map::iterator it) {
        bytes_ -= it->second.entry->bytes;
        order_.erase(it->second.pos);
        map_.erase(it);
    }
    void evict_to(size_t limit) {
        while (bytes_ > limit && !order_.empty()) {
            erase(map_.find(order_.back()));
            ++st_.evictions;
        }
    }

    size_t max_bytes_ = 0;
    size_t bytes_     = 0;
    Map    map_;
    std::list<RegionCacheKey> order_;   ///< front = most recently used
    Stats  st_;
};

}  // namespace ewb
