// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include <stdio.h>
#include <stdlib.h>
#include "nvn_vk_cold_ledger.h"

static unsigned failures;

#define CHECK(expect, label) do { \
    if (!(expect)) { fprintf(stderr, "cold-ledger FAIL: %s\n", (label)); failures++; } \
} while (0)

static unsigned writers_at(const struct cold_ledger *ledger, unsigned at) {
    for (unsigned i = 0; i < ledger->n; i++)
        if (ledger->span[i].lo <= at && at < ledger->span[i].hi) return ledger->span[i].writers;
    return 0;
}

static void compact_published_bytes(void) {
    struct cold_ledger ledger = {0};
    enum { FIRST = 128, WRITES = COLD_LEDGER_SPANS + 8 };
    cold_ledger_write(&ledger, 0, 8, 0, COLD_READ_TRANSFER, 1);
    for (unsigned i = 0; i < WRITES; i++)
        cold_ledger_write(&ledger, FIRST + i * 4, FIRST + (i + 1) * 4, i + 1, COLD_READ_TRANSFER, 1);
    CHECK(writers_at(&ledger, 0) == COLD_READ_TRANSFER, "fragmentation erased a disjoint proven domain");
    unsigned missing = 0;
    for (unsigned at = FIRST; at < FIRST + WRITES * 4; at++)
        missing += writers_at(&ledger, at) != COLD_READ_TRANSFER;
    CHECK(!missing, "ascending exact writes lost bytes from the scan envelope");
    CHECK(ledger.n == 3, "older writes compact while the newest epoch stays distinct");
}

static void preserve_proof_boundaries(void) {
    const unsigned transfer = COLD_READ_TRANSFER, compute = COLD_READ_COMPUTE;
    struct cold_ledger ledger = {0};
    cold_ledger_write(&ledger, 0, 4, 1, transfer, 1);
    cold_ledger_write(&ledger, 4, 8, 2, transfer, 1);
    cold_ledger_write(&ledger, 0, 8, 2, compute, 0);
    CHECK(writers_at(&ledger, 0) == (transfer | compute), "ordered bytes lost their earlier producer");
    CHECK(writers_at(&ledger, 4) == 0, "same-epoch GPU overlap must stay unknown");

    ledger = (struct cold_ledger){0};
    cold_ledger_write(&ledger, 0, 4, 1, transfer, 1);
    cold_ledger_write(&ledger, 8, 12, 2, transfer, 1);
    cold_ledger_write(&ledger, 12, 16, 3, COLD_READ_HOST, 1);
    cold_ledger_write(&ledger, 20, 24, 4, transfer, 1);
    CHECK(writers_at(&ledger, 4) == 0, "compaction crossed a real hole");
    CHECK(writers_at(&ledger, 8) == transfer && writers_at(&ledger, 12) == COLD_READ_HOST,
          "compaction changed per-byte producer masks");

    ledger = (struct cold_ledger){0};
    for (unsigned i = 0; i < COLD_LEDGER_SPANS; i++)
        cold_ledger_write(&ledger, 2 * i, 2 * i + 1, i + 1, transfer, 1);
    CHECK(ledger.n == COLD_LEDGER_SPANS, "independent proofs fit at the bound");
    cold_ledger_write(&ledger, 2 * COLD_LEDGER_SPANS, 2 * COLD_LEDGER_SPANS + 1,
                      COLD_LEDGER_SPANS + 1, transfer, 1);
    CHECK(ledger.n == 0, "irreducible overflow invalidates the publication table");

    ledger = (struct cold_ledger){0};
    for (unsigned i = 0; i < COLD_LEDGER_SPANS; i++)
        cold_ledger_write(&ledger, 4 * i, 4 * i + 3, i + 1, transfer, 1);
    CHECK(ledger.n == COLD_LEDGER_SPANS && writers_at(&ledger, 1) == transfer,
          "overlap fixture starts at capacity with a proven write");
    cold_ledger_write(&ledger, 1, 2, COLD_LEDGER_SPANS + 1, 0, 0);
    CHECK(!writers_at(&ledger, 1), "overflow must not keep proof invalidated by an unknown write");
}

static uint32_t random_word(uint32_t *seed) {
    *seed ^= *seed << 13;
    *seed ^= *seed >> 17;
    *seed ^= *seed << 5;
    return *seed;
}

static void compare_byte_model(void) {
    // Fewer bytes than span slots so the byte model can compare every proof
    // without modeling the implementation's capacity fallback.
    enum { BYTES = 64, STREAMS = 32, WRITES = 512 };
    for (uint32_t stream = 1; stream <= STREAMS; stream++) {
        struct cold_ledger ledger = {0};
        unsigned writers[BYTES] = {0};
        uint64_t ages[BYTES] = {0}, epoch = 0;
        uint32_t seed = stream;
        for (unsigned i = 0; i < WRITES; i++) {
            const unsigned start = random_word(&seed) % BYTES;
            const unsigned length = 1 + random_word(&seed) % (BYTES - start);
            const unsigned op = random_word(&seed);
            const unsigned kind = op % 5;
            const unsigned writer = kind ? 1u << (kind - 1) : 0;
            const int exact = (op >> 5) & 1;
            epoch += (op >> 11) & 1;
            cold_ledger_write(&ledger, start, start + length, epoch, writer, exact);
            for (unsigned b = start; b < start + length; b++) {
                if (!writers[b]) {
                    writers[b] = exact ? writer : 0;
                } else if (!writer || (ages[b] == epoch &&
                           (writers[b] & ~COLD_READ_HOST) && writer != COLD_READ_HOST)) {
                    writers[b] = 0;
                } else {
                    writers[b] = exact ? writer : writers[b] | writer;
                }
                ages[b] = epoch;
            }
            unsigned span = 0;
            for (unsigned b = 0; b < BYTES; b++) {
                while (span < ledger.n && ledger.span[span].hi <= b) span++;
                const struct cold_interval *s = span < ledger.n && ledger.span[span].lo <= b ?
                    &ledger.span[span] : NULL;
                if ((s ? s->writers : 0) != writers[b] ||
                    (s && ((s->epoch == epoch) != (ages[b] == epoch)))) {
                    fprintf(stderr, "cold-ledger FAIL: byte model stream=%u write=%u byte=%u\n", stream, i, b);
                    failures++;
                    return;
                }
            }
        }
    }
    puts("cold-ledger byte model: 16384 writes, 1048576 byte checks PASS");
}

int main(void) {
    compact_published_bytes();
    preserve_proof_boundaries();
    compare_byte_model();
    printf("cold-ledger failures=%u\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
