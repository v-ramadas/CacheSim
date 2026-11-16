#ifndef __COHERENCE_H__
#define __COHERENCE_H__

// Coherence protocols (--coherence). A protocol only decides: given an event at one core and the states of the
// other cores' private copies of the 64B line, it says what each copy does, who supplies the data, and the
// requester's new state. The engine in access_multi_level (cachesim.cc) carries the decision out on the caches,
// charges it and counts it; replacement-history hooks (--serviced-from-c2c, merge-on-drop) are the engine's, not
// the protocol's. Adding a protocol = one class here + an entry in coh::Kind and --coherence.
//
// States are small integers numbered by each protocol, with 0 = invalid in every protocol and larger = more
// ownership (two copies of one core's line merge to the larger state). Each private (sectored) line stores its
// state (SectoredSet::coh_state); packets carry it between levels.
//
// Directory: unbounded and exact - the copies are found by asking the other cores' private caches (one entry per
// private cache that holds any word of the line, in core order, then level order).

#include <cstdint>
#include <memory>
#include <vector>

namespace coh {

enum class Kind { NONE, MSI, MESI };

constexpr uint8_t INVALID = 0;

enum class Event {
    READ_MISS,    // a load missed every private level of the requester
    WRITE_MISS,   // a store missed every private level of the requester
    STORE,        // a store after its hit or fill (upgrade if others still hold the line)
    EVICT,        // the requester's copy leaves its private levels (towards the LLC)
};

struct Copy {                      // another core's private copy of the line
    uint8_t state = INVALID;
    bool has_word = false;         // holds the demanded 8B word
    bool dirty = false;            // holds dirty words (it owes their writeback); with ownership hand-over below,
                                   // only a copy in a state that may be dirty (M, O) does
};

enum class Act : uint8_t { KEEP, DOWNGRADE, INVALIDATE };

struct Order {
    Act act = Act::KEEP;
    uint8_t new_state = INVALID;   // DOWNGRADE: the copy's state after (its dirty words are written back)
};

struct Decision {
    std::vector<Order> orders;     // one per copy, same order as the input
    int supplier = -1;             // index of the copy that sends the line (cache-to-cache), -1 = LLC / memory
    uint8_t requester_state = INVALID;
    bool request = false;          // the requester sent a coherence request: one snoop round trip is charged
    bool drop_victim = false;      // EVICT: other copies stay, so the victim is dropped instead of filling the LLC
    // READ_MISS served by the LLC or memory with dirty words (the LLC is exclusive: dirty words leave it with the
    // line, and the requester's own partial copy may be dirty). Ownership hand-over, so that dirty data is only ever
    // in an owning state (as in an inclusive hierarchy, Sniper): the requester takes `dirty_fill_state` (e.g. M,
    // owing the writeback), or, with clean_dirty_fill, the dirty words are written back as the line fills and the
    // requester keeps requester_state.
    uint8_t dirty_fill_state = INVALID;
    bool clean_dirty_fill = false;
};

class Protocol {
  public:
    virtual ~Protocol() = default;
    virtual const char* name() const = 0;
    virtual const char* state_name(uint8_t state) const = 0;
    virtual bool may_be_dirty(uint8_t state) const = 0;   // a copy in this state may hold dirty words (M, O)
    virtual bool shares_dirty() const = 0;                // dirty data may stay while other cores hold the line (O)
    virtual bool exclusive(uint8_t state) const = 0;      // no other core may hold the line meanwhile
    virtual Decision decide(Event event, const std::vector<Copy>& others, uint8_t own_state) const = 0;
};

std::unique_ptr<Protocol> make(Kind kind);

}  // namespace coh

#endif
