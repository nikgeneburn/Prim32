// ============================================================================
// Prim32 internal UI state store.
// ============================================================================
#include "prim32_state.h"
#include <stdlib.h>
#include <string.h>

namespace prim32 {

static uint32_t StateHash(uint32_t id) {
    return id * 2654435761u;
}

static void StateRebuildSlots(Prim32StateStore* store) {
    memset(store->slots, 0, (size_t)store->slotCapacity * sizeof(*store->slots));
    uint32_t mask = store->slotCapacity - 1;
    for (uint32_t entryIndex = 0; entryIndex < store->count; entryIndex++) {
        uint32_t slot = StateHash(store->entries[entryIndex].id) & mask;
        while (store->slots[slot]) slot = (slot + 1) & mask;
        store->slots[slot] = entryIndex + 1;
    }
}

static bool StateReserve(Prim32StateStore* store, uint32_t needed) {
    if (needed <= store->capacity) return true;

    uint32_t newCapacity = store->capacity ? store->capacity : 8;
    while (newCapacity < needed) {
        if (newCapacity > UINT32_MAX / 2) return false;
        newCapacity *= 2;
    }
    if (newCapacity > UINT32_MAX / 2 ||
        (size_t)newCapacity > (size_t)-1 / sizeof(*store->entries)) return false;

    uint32_t newSlotCapacity = newCapacity * 2;
    if ((size_t)newSlotCapacity > (size_t)-1 / sizeof(*store->slots)) return false;

    Prim32StateEntry* entries = (Prim32StateEntry*)realloc(
        store->entries, (size_t)newCapacity * sizeof(*store->entries));
    if (!entries) return false;
    store->entries = entries;

    uint32_t* slots = (uint32_t*)realloc(
        store->slots, (size_t)newSlotCapacity * sizeof(*store->slots));
    if (!slots) return false;

    store->slots = slots;
    store->capacity = newCapacity;
    store->slotCapacity = newSlotCapacity;
    StateRebuildSlots(store);
    return true;
}

bool Prim32StateStoreInit(Prim32StateStore* store, uint32_t initialCapacity) {
    if (!store) return false;
    *store = {};
    if (!initialCapacity) return true;

    uint32_t capacity = 8;
    while (capacity < initialCapacity) {
        if (capacity > UINT32_MAX / 2) return false;
        capacity *= 2;
    }
    if (capacity > UINT32_MAX / 2 ||
        (size_t)capacity > (size_t)-1 / sizeof(*store->entries)) return false;

    uint32_t slotCapacity = capacity * 2;
    if ((size_t)slotCapacity > (size_t)-1 / sizeof(*store->slots)) return false;

    store->entries = (Prim32StateEntry*)malloc((size_t)capacity * sizeof(*store->entries));
    if (!store->entries) return false;

    store->slots = (uint32_t*)malloc((size_t)slotCapacity * sizeof(*store->slots));
    if (!store->slots) {
        free(store->entries);
        *store = {};
        return false;
    }

    store->capacity = capacity;
    store->slotCapacity = slotCapacity;
    memset(store->slots, 0, (size_t)slotCapacity * sizeof(*store->slots));
    return true;
}

void Prim32StateStoreFree(Prim32StateStore* store) {
    if (!store) return;
    free(store->entries);
    free(store->slots);
    *store = {};
}

Prim32StateEntry* Prim32StateStoreFind(Prim32StateStore* store, uint32_t id) {
    if (!store || !store->slots || !store->slotCapacity) return nullptr;

    uint32_t mask = store->slotCapacity - 1;
    uint32_t slot = StateHash(id) & mask;
    while (store->slots[slot]) {
        Prim32StateEntry* entry = &store->entries[store->slots[slot] - 1];
        if (entry->id == id) return entry;
        slot = (slot + 1) & mask;
    }
    return nullptr;
}

Prim32StateEntry* Prim32StateStoreFindOrCreate(Prim32StateStore* store, uint32_t id, uint32_t frame) {
    Prim32StateEntry* entry = Prim32StateStoreFind(store, id);
    if (entry) {
        entry->lastFrame = frame;
        return entry;
    }
    if (!store || !StateReserve(store, store->count + 1)) return nullptr;

    uint32_t entryIndex = store->count;
    entry = &store->entries[entryIndex];
    *entry = {};
    entry->id = id;
    entry->lastFrame = frame;

    uint32_t mask = store->slotCapacity - 1;
    uint32_t slot = StateHash(id) & mask;
    while (store->slots[slot]) slot = (slot + 1) & mask;
    store->slots[slot] = entryIndex + 1;
    store->count++;
    return entry;
}

uint32_t Prim32StateStorePrune(Prim32StateStore* store, uint32_t minLastFrame) {
    if (!store || !store->count) return 0;

    uint32_t writeIndex = 0;
    for (uint32_t readIndex = 0; readIndex < store->count; readIndex++) {
        Prim32StateEntry* entry = &store->entries[readIndex];
        if (entry->lastFrame < minLastFrame) continue;
        if (writeIndex != readIndex) store->entries[writeIndex] = *entry;
        writeIndex++;
    }

    uint32_t removed = store->count - writeIndex;
    store->count = writeIndex;
    if (removed) StateRebuildSlots(store);
    return removed;
}

} // namespace prim32
