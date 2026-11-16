#include "coherence.h"

namespace coh {
namespace {

// MSI. I = 0, S = 1 (clean, any number of cores), M = 2 (dirty or written, one core).
//   read miss:  M copies downgrade to S (dirty words written back: MSI cannot share dirty data); the first copy
//               holding the demanded word supplies the line (clean); requester S (M if it already owned the line,
//               partly, and nobody else holds it). Dirty words coming from the LLC / the requester's own partial copy:
//               the requester becomes M if nobody else holds the line, else they are written back as the line fills
//   write miss: every copy invalidated; the first one holding the word hands its line (dirty words included) over;
//               requester M
//   store:      a store to an S line sends an upgrade request to the directory (one snoop round trip) even when
//               no other core holds the line; copies still elsewhere are invalidated; requester M
//   evict:      dropped while other copies remain (a line is in private caches or the LLC, never both)
class MSI : public Protocol {
  public:
    static constexpr uint8_t S = 1, M = 2;
    const char* name() const override { return "MSI"; }
    const char* state_name(uint8_t s) const override { return s == M ? "M" : s == S ? "S" : "I"; }
    bool may_be_dirty(uint8_t s) const override { return s == M; }
    bool shares_dirty() const override { return false; }
    bool exclusive(uint8_t s) const override { return s == M; }

    Decision decide(Event e, const std::vector<Copy>& others, uint8_t own) const override {
        Decision d;
        d.orders.resize(others.size());
        d.request = !others.empty();
        auto first_with_word = [&] {
            for (size_t i = 0; i < others.size(); ++i)
                if (others[i].has_word) return (int)i;
            return -1;
        };
        switch (e) {
            case Event::READ_MISS:
                for (size_t i = 0; i < others.size(); ++i)
                    if (others[i].state == M) d.orders[i] = {Act::DOWNGRADE, S};
                d.supplier = first_with_word();
                d.requester_state = (own == M && others.empty()) ? M : S;
                d.dirty_fill_state = M;
                d.clean_dirty_fill = !others.empty();
                break;
            case Event::WRITE_MISS:
            case Event::STORE:
                for (auto& o : d.orders) o = {Act::INVALIDATE, INVALID};
                d.supplier = e == Event::WRITE_MISS ? first_with_word() : -1;
                d.requester_state = M;
                if (e == Event::STORE && own != M)
                    d.request = true;   // S -> M asks the directory even when nobody else holds the line
                break;
            case Event::EVICT:
                d.request = false;
                d.drop_victim = !others.empty();
                break;
        }
        return d;
    }

};

// MESI. I = 0, S = 1, E = 2 (clean, only this core), M = 3. As MSI, plus:
//   read miss:  a requester nobody else holds the line for gets E (M if it already owned it, or if dirty words
//               come with the line: it owes their writeback); E copies elsewhere downgrade to S (clean: nothing
//               written back)
//   store:      E -> M silently (no request: E is already exclusive); S -> M asks the directory as in MSI
class MESI : public Protocol {
  public:
    static constexpr uint8_t S = 1, E = 2, M = 3;
    const char* name() const override { return "MESI"; }
    const char* state_name(uint8_t s) const override { return s == M ? "M" : s == E ? "E" : s == S ? "S" : "I"; }
    bool may_be_dirty(uint8_t s) const override { return s == M; }
    bool shares_dirty() const override { return false; }
    bool exclusive(uint8_t s) const override { return s == M || s == E; }

    Decision decide(Event e, const std::vector<Copy>& others, uint8_t own) const override {
        Decision d;
        d.orders.resize(others.size());
        d.request = !others.empty();
        auto first_with_word = [&] {
            for (size_t i = 0; i < others.size(); ++i)
                if (others[i].has_word) return (int)i;
            return -1;
        };
        switch (e) {
            case Event::READ_MISS:
                for (size_t i = 0; i < others.size(); ++i)
                    if (others[i].state == M || others[i].state == E)
                        d.orders[i] = {Act::DOWNGRADE, S};
                d.supplier = first_with_word();
                d.requester_state = others.empty() ? (own == M ? M : E) : S;
                d.dirty_fill_state = M;
                d.clean_dirty_fill = !others.empty();
                break;
            case Event::WRITE_MISS:
            case Event::STORE:
                for (auto& o : d.orders) o = {Act::INVALIDATE, INVALID};
                d.supplier = e == Event::WRITE_MISS ? first_with_word() : -1;
                d.requester_state = M;
                if (e == Event::STORE && own != M && own != E)
                    d.request = true;   // S -> M asks the directory; E -> M is silent
                break;
            case Event::EVICT:
                d.request = false;
                d.drop_victim = !others.empty();
                break;
        }
        return d;
    }

};

}  // namespace

std::unique_ptr<Protocol> make(Kind kind) {
    switch (kind) {
        case Kind::MSI: return std::make_unique<MSI>();
        case Kind::MESI: return std::make_unique<MESI>();
        case Kind::NONE: break;
    }
    return nullptr;
}

}  // namespace coh
