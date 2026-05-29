#ifdef __FISSION_H__
#define __FISSION_H__

#include <list>
#include <vector>
#include <stdint.h>

struct Node {
    uint64_t start_address;
    uint64_t end_address;
    uint64_t size;
    uint64_t idx;

    Node(uint64_t _addr, uint64_t _size) {
        start_address = _addr;
        size = _size;
        end_address = start_address + size;
    }
};
   
struct SaturatingCounter {
    uint64_t max_value;
    uint64_t left_value;
    uint64_t right_value;

    SaturatingCounter(uint64_t val): max_value(val) {
        left_value=0;
        right_value=0;
    }
    void incr_left() {
        if (left_value < max_value)
            left_value++;
    }
    void incr_right() {
        if (right_value < max_value)
            right_value++;
    }
    void decr_left() {
        if (left_value > 0)
            left_value--;
    }
    void decr_right() {
        if (right_value > 0)
            right_value--;
    }    
};


class FissionTable {
    std::list<Node*> nodes;
    SaturatingCounter counter;
    uint64_t idx;
    uint64_t last_access;

    public:
    FissionMetadata(uint64_t _start_address, uint64_t _size) {
        start_address = _start_address;
        size = _size;
        end_address = start_address + size;
        counter.max_value = 256;
        counter_skew.max_value = 16;
        counter_skew.value = counter_skew.max_value/2;
    }

    uint64_t reuses = 0;
    void incr() { counter.incr();}
    void decr() { counter.decr();}

    void do_fission() {
        if (left_value + right_value < max_value)
            return;
        if (right_value == 0) {
            nodes[idx].size /= 2;
            nodes[idx].end_value -= size;
            nodes[idx].left_value/=2;
            nodes[idx].right_value = nodes[idx].left_value;
            return;
        } else if (left_value == 0) {
            nodes[idx].size /= 2;
            nodes[idx].start_value += size;
            nodes[idx].left_value/=2;
            nodes[idx].right_value = nodes[idx].left_value;
            return;
        }

        SaturatingCounter* new_node = SaturatingCounter(max_val);
        if (left_value < right_value) {
        }
    }

};


#endif
