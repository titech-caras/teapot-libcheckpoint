/* Independent C decision model; no production implementation is called. */
#include <stdint.h>

static uint64_t model_hash(uint64_t key, unsigned octave, uint64_t seed) {
    uint64_t x = (key ^ seed) * UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 31;
    x += octave * UINT64_C(0x94d049bb133111eb);
    x ^= x >> 29;
    x *= UINT64_C(0xbf58476d1ce4e5b9);
    return x ^ (x >> 32);
}

static int model_decision(struct teapot_nested_site *s, uint64_t index,
                          unsigned outcome, unsigned depth, uint32_t *generation,
                          uint64_t seed) {
    if (depth == 6)
        return 0;
    if (!depth) {
        unsigned c = s->arch[outcome];
        if (c != 0xffff)
            s->arch[outcome] = ++c;
        if (!c || (c & (c - 1)))
            return 0;
        if (++*generation == 0)
            ++*generation;
        return 1;
    }
    if (s->gen != *generation) {
        s->gen = *generation;
        s->tree[0] = s->tree[1] = 0;
    }
    unsigned t = s->tree[outcome];
    if (t != 0xffff)
        s->tree[outcome] = ++t;
    if (t == 0xffff)
        return 0;
    unsigned e = 0;
    while ((2u << e) <= t)
        ++e;
    unsigned base = 1u << e;
    uint64_t key = ((uint64_t)*generation << 24) ^ (index << 1) ^ outcome;
    return t - base == (model_hash(key, e, seed) & (base - 1));
}
