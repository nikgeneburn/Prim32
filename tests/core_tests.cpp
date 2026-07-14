#include <prim32/prim32.h>
#include "prim32_state.h"

#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void Check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "core test failed at line %d: %s\n", line, expression);
        ++failures;
    }
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

void CheckUtf8(uint32_t codepoint, const unsigned char* expected, int expectedLength) {
    char encoded[5];
    const int length = prim32::EncodeUtf8(codepoint, encoded);
    CHECK(length == expectedLength);
    CHECK(std::memcmp(encoded, expected, static_cast<size_t>(expectedLength)) == 0);
    CHECK(encoded[length] == '\0');
}

void TestStateStore() {
    prim32::Prim32StateStore store = {};
    CHECK(prim32::Prim32StateStoreInit(&store, 0));

    prim32::Prim32StateEntry* first = prim32::Prim32StateStoreFindOrCreate(&store, 100u, 3u);
    CHECK(first != nullptr);
    if (first) {
        first->x = 12.5f;
        first->selection = 7;
    }
    CHECK(store.count == 1u);

    prim32::Prim32StateEntry* same = prim32::Prim32StateStoreFindOrCreate(&store, 100u, 8u);
    CHECK(same != nullptr);
    if (same) {
        CHECK(same->x == 12.5f);
        CHECK(same->selection == 7);
        CHECK(same->lastFrame == 8u);
    }

    for (uint32_t id = 1; id <= 40; id++) {
        prim32::Prim32StateEntry* entry = prim32::Prim32StateStoreFindOrCreate(&store, id, id);
        CHECK(entry != nullptr);
        if (entry) entry->flags = id;
    }
    CHECK(store.count == 41u);
    prim32::Prim32StateEntry* entry40 = prim32::Prim32StateStoreFind(&store, 40u);
    CHECK(entry40 != nullptr);
    if (entry40) CHECK(entry40->flags == 40u);

    CHECK(prim32::Prim32StateStorePrune(&store, 20u) == 20u);
    CHECK(prim32::Prim32StateStoreFind(&store, 1u) == nullptr);
    CHECK(prim32::Prim32StateStoreFind(&store, 19u) == nullptr);
    CHECK(prim32::Prim32StateStoreFind(&store, 20u) != nullptr);
    CHECK(prim32::Prim32StateStoreFind(&store, 40u) != nullptr);
    CHECK(prim32::Prim32StateStoreFind(&store, 100u) == nullptr);

    prim32::Prim32StateStoreFree(&store);
    CHECK(store.entries == nullptr);
    CHECK(store.slots == nullptr);
    CHECK(store.count == 0u);
}

} // namespace

int main() {
    CHECK(prim32::COL32(0x12, 0x34, 0x56, 0x78) == 0x78563412u);
    CHECK(prim32::COL32(0x12, 0x34, 0x56) == 0xFF563412u);

    CHECK(prim32::F16(0.0f) == 0x0000u);
    CHECK(prim32::F16(1.0f) == 0x3C00u);
    CHECK(prim32::F16(-2.0f) == 0xC000u);
    CHECK(prim32::F16(65504.0f) == 0x7BFFu);
    CHECK(prim32::PackF16x2(1.0f, -2.0f) == 0xC0003C00u);

    CHECK(prim32::PackUV(0.0f, 1.0f) == 0xFFFF0000u);
    CHECK(prim32::PackUV(0.5f, 0.25f) == 0x40008000u);

    const unsigned char ascii[] = { 'A' };
    const unsigned char twoByte[] = { 0xC3u, 0xA9u };
    const unsigned char threeByte[] = { 0xEEu, 0x9Cu, 0x80u };
    const unsigned char fourByte[] = { 0xF0u, 0x9Fu, 0x99u, 0x82u };
    CheckUtf8(0x41u, ascii, 1);
    CheckUtf8(0x00E9u, twoByte, 2);
    CheckUtf8(0xE700u, threeByte, 3);
    CheckUtf8(0x1F642u, fourByte, 4);
    TestStateStore();

    return failures == 0 ? 0 : 1;
}
