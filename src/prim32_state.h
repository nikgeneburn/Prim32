// ============================================================================
// Prim32 internal UI state store.
//
// The store has no globals and is intended to be embedded in Context when UI
// state is integrated. Zero-initialize it, then call Init before use.
// ============================================================================
#pragma once
#include <stdint.h>

namespace prim32 {

// Persistent state shared by future scroll, table, tab, and widget helpers.
// FindOrCreate zero-initializes a new entry and updates lastFrame on every hit.
struct Prim32StateEntry {
    uint32_t id;
    float    x;
    float    y;
    float    extentX;
    float    extentY;
    float    viewportX;
    float    viewportY;
    float    viewportW;
    float    viewportH;
    int32_t  selection;
    uint32_t flags;
    uint32_t lastFrame;
    uint32_t owner;
    uint32_t depth;
};

// Context-owned storage. entries stays dense; slots holds entry indices + 1,
// with zero denoting an unused slot. Call Free before reinitializing a store.
struct Prim32StateStore {
    Prim32StateEntry* entries;
    uint32_t*         slots;
    uint32_t          count;
    uint32_t          capacity;
    uint32_t          slotCapacity;
};

// initialCapacity is the expected number of entries; zero enables lazy growth.
bool Prim32StateStoreInit(Prim32StateStore* store, uint32_t initialCapacity);
void Prim32StateStoreFree(Prim32StateStore* store);

// Find never allocates. FindOrCreate returns null only when storage growth fails.
Prim32StateEntry* Prim32StateStoreFind(Prim32StateStore* store, uint32_t id);
Prim32StateEntry* Prim32StateStoreFindOrCreate(Prim32StateStore* store, uint32_t id, uint32_t frame);

// Removes entries last used before minLastFrame and returns the removed count.
// Growth and pruning may move entries, invalidating previously returned pointers.
uint32_t Prim32StateStorePrune(Prim32StateStore* store, uint32_t minLastFrame);

} // namespace prim32
