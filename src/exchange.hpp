// exchange.hpp - deterministic price-time-priority matching engine: the
// replicated state machine of the exchange.
//
// Determinism here is the correctness foundation, not a performance trick:
// every replica applies the same committed command sequence and must
// produce the byte-identical book and fill stream. The rules that make
// that provable rather than hopeful:
//   - integer ticks only, ordered containers only, no clocks, no
//     randomness, no pointers in observable state;
//   - parsing is a hand-rolled pure function of the command BYTES --
//     iostreams were rejected because operator>> consults the process
//     global locale, which lets two identical binaries diverge;
//   - the command language is canonical (single spaces, digits only, no
//     signs, no trailing bytes): one byte string, one action;
//   - price and quantity are admission-bounded (<= 1e9) so every
//     downstream arithmetic step is provably in range; the only unbounded
//     counters are uint64, whose wrap is defined and identical everywhere;
//   - every input, malformed ones included, has exactly one defined
//     outcome, and rejections are counted state.
//
// The entire observable state folds into one FNV-1a hash with count-prefix
// framing (sizes before contents, so data words cannot impersonate
// structure): two replicas agree if and only if one number agrees.
//
// Scope notes: order ids are unique among OPEN orders only -- an id may be
// reused after its order fully fills or is cancelled, and fills attribute
// to id occurrences, not economic orders. There is no trader or account
// concept, so trader-level self-trade prevention is out of scope here; a
// real venue adds STP as a deterministic REPLICATED rule, not a gateway
// filter. Client-session deduplication across leader failover is Phase C.
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace exch {

struct Fill {
    std::uint64_t taker = 0, maker = 0;
    std::int64_t px = 0, qty = 0;
};

class Exchange {
public:
    static constexpr std::uint64_t kMaxPx = 1000000000;   // admission bounds:
    static constexpr std::uint64_t kMaxQty = 1000000000;  // all math in range

    // Commands (opaque strings in the Raft log), canonical grammar only:
    //   "N <id> B|S <px> <qty>"   new limit order, fields ASCII digits
    //   "C <id>"                  cancel by open order id
    // Returns the fills this command produced (empty for rests/cancels).
    std::vector<Fill> apply(const std::string& cmd) {
        std::vector<Fill> fills;
        const char* p = cmd.data();
        const char* end = p + cmd.size();
        if (p == end) { note_reject(); return fills; }
        char op = *p++;
        if (op == 'N') {
            std::uint64_t id = 0, px = 0, qty = 0;
            char side = 0;
            bool ok = eat(p, end, ' ') && num(p, end, id, ~0ULL) &&
                      eat(p, end, ' ') && p != end &&
                      (*p == 'B' || *p == 'S') && (side = *p++, true) &&
                      eat(p, end, ' ') && num(p, end, px, kMaxPx) &&
                      eat(p, end, ' ') && num(p, end, qty, kMaxQty) &&
                      p == end && px > 0 && qty > 0 && !index_.count(id);
            if (!ok) { note_reject(); return fills; }
            fills = match(id, static_cast<std::int64_t>(px),
                          static_cast<std::int64_t>(qty), side == 'B');
        } else if (op == 'C') {
            std::uint64_t id = 0;
            bool ok = eat(p, end, ' ') && num(p, end, id, ~0ULL) &&
                      p == end && index_.count(id);
            if (!ok) { note_reject(); return fills; }
            auto [buy, px] = index_[id];
            auto& q = buy ? bids_[px] : asks_[px];
            for (auto it = q.begin(); it != q.end(); ++it)
                if (it->id == id) { q.erase(it); break; }
            if (q.empty()) {
                if (buy) bids_.erase(px); else asks_.erase(px);
            }
            index_.erase(id);
        } else {
            note_reject();
        }
        return fills;
    }

    // FNV-1a over the whole observable state, with count-prefix framing so
    // the folded word stream parses unambiguously: sizes come before
    // contents, and no data word can fake a level boundary.
    std::uint64_t state_hash() const {
        std::uint64_t h = 1469598103934665603ULL;
        auto fold = [&h](std::uint64_t v) {
            for (int i = 0; i < 8; ++i) {
                h ^= (v >> (8 * i)) & 0xff;
                h *= 1099511628211ULL;
            }
        };
        fold(bids_.size());
        fold(asks_.size());
        for (auto& [px, q] : bids_) {
            fold(static_cast<std::uint64_t>(px));
            fold(q.size());
            for (auto& o : q) {
                fold(o.id);
                fold(static_cast<std::uint64_t>(o.qty));
            }
        }
        for (auto& [px, q] : asks_) {
            fold(static_cast<std::uint64_t>(px));
            fold(q.size());
            for (auto& o : q) {
                fold(o.id);
                fold(static_cast<std::uint64_t>(o.qty));
            }
        }
        fold(fills_hash_);
        fold(n_fills_);
        fold(volume_);
        fold(rejected_);
        return h;
    }

    // ---- observers -------------------------------------------------------
    std::int64_t best_bid() const {
        return bids_.empty() ? 0 : bids_.begin()->first;
    }
    std::int64_t best_ask() const {
        return asks_.empty() ? 0 : asks_.begin()->first;
    }
    std::uint64_t fill_count() const { return n_fills_; }
    std::uint64_t volume() const { return volume_; }
    std::size_t open_orders() const { return index_.size(); }
    std::uint64_t rejected() const { return rejected_; }
    void reset() { *this = Exchange{}; }

private:
    struct Order { std::uint64_t id; std::int64_t qty; };

    // Digits-only uint64 parse with an admission bound; rejects signs,
    // leading whitespace, hex, grouping -- a pure function of the bytes.
    static bool num(const char*& p, const char* end, std::uint64_t& v,
                    std::uint64_t max) {
        if (p == end || *p < '0' || *p > '9') return false;
        std::uint64_t x = 0;
        while (p != end && *p >= '0' && *p <= '9') {
            std::uint64_t d = static_cast<std::uint64_t>(*p - '0');
            if (x > max / 10 || x * 10 > max - d) return false;
            x = x * 10 + d;
            ++p;
        }
        v = x;
        return true;
    }
    static bool eat(const char*& p, const char* end, char c) {
        if (p == end || *p != c) return false;
        ++p;
        return true;
    }

    std::vector<Fill> match(std::uint64_t id, std::int64_t px,
                            std::int64_t qty, bool buy) {
        std::vector<Fill> fills;
        if (buy) {
            while (qty > 0 && !asks_.empty() && asks_.begin()->first <= px)
                take(fills, id, qty, asks_);
        } else {
            while (qty > 0 && !bids_.empty() && bids_.begin()->first >= px)
                take(fills, id, qty, bids_);
        }
        if (qty > 0) {                       // remainder rests
            if (buy) bids_[px].push_back({id, qty});
            else asks_[px].push_back({id, qty});
            index_[id] = {buy, px};
        }
        return fills;
    }

    template <class Book>
    void take(std::vector<Fill>& fills, std::uint64_t taker,
              std::int64_t& qty, Book& book) {
        auto lvl = book.begin();             // best price, FIFO within it
        auto& q = lvl->second;
        Order& maker = q.front();
        std::int64_t traded = qty < maker.qty ? qty : maker.qty;
        fills.push_back({taker, maker.id, lvl->first, traded});
        note_fill(fills.back());
        qty -= traded;
        maker.qty -= traded;
        if (maker.qty == 0) {
            index_.erase(maker.id);
            q.pop_front();
            if (q.empty()) book.erase(lvl);
        }
    }

    void note_fill(const Fill& f) {
        auto fold = [this](std::uint64_t v) {
            for (int i = 0; i < 8; ++i) {
                fills_hash_ ^= (v >> (8 * i)) & 0xff;
                fills_hash_ *= 1099511628211ULL;
            }
        };
        fold(f.taker); fold(f.maker);
        fold(static_cast<std::uint64_t>(f.px));
        fold(static_cast<std::uint64_t>(f.qty));
        ++n_fills_;
        volume_ += static_cast<std::uint64_t>(f.qty);
    }

    void note_reject() { ++rejected_; }

    std::map<std::int64_t, std::deque<Order>, std::greater<std::int64_t>>
        bids_;
    std::map<std::int64_t, std::deque<Order>> asks_;
    std::map<std::uint64_t, std::pair<bool, std::int64_t>> index_;
    std::uint64_t fills_hash_ = 1469598103934665603ULL;
    std::uint64_t n_fills_ = 0;
    std::uint64_t volume_ = 0;
    std::uint64_t rejected_ = 0;
};

}  // namespace exch
