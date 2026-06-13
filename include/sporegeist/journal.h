#ifndef SPOREGEIST_JOURNAL_H
#define SPOREGEIST_JOURNAL_H

#include "sporegeist/status.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPG_JOURNAL_MAGIC UINT32_C(0x53474a31)
#define SPG_JOURNAL_VERSION UINT16_C(1)
#define SPG_JOURNAL_HASH_BYTES 32u

enum spg_journal_event_kind {
    SPG_JOURNAL_EVENT_RUN_START = 0,
    SPG_JOURNAL_EVENT_POLICY_DECISION,
    SPG_JOURNAL_EVENT_MODEL_INPUT,
    SPG_JOURNAL_EVENT_MODEL_OUTPUT,
    SPG_JOURNAL_EVENT_ACTION,
    SPG_JOURNAL_EVENT_RESULT,
    SPG_JOURNAL_EVENT_GRAPH,
    SPG_JOURNAL_EVENT_MEMORY,
    SPG_JOURNAL_EVENT_SIM,
    SPG_JOURNAL_EVENT_ERROR,
};

struct spg_journal_record_header {
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint64_t sequence;
    uint64_t timestamp_ns;
    uint64_t parent_sequence;
    uint32_t event_kind;
    uint32_t status;
    uint64_t payload_bytes;
    uint8_t  prev_hash[SPG_JOURNAL_HASH_BYTES];
    uint8_t  record_hash[SPG_JOURNAL_HASH_BYTES];
};

struct spg_journal_writer {
    FILE    *file;
    uint64_t next_sequence;
    uint8_t  last_hash[SPG_JOURNAL_HASH_BYTES];
};

struct spg_journal_reader {
    FILE    *file;
    uint64_t next_sequence;
    uint8_t  last_hash[SPG_JOURNAL_HASH_BYTES];
};

struct spg_journal_record {
    struct spg_journal_record_header header;
    size_t                           payload_used;
};

[[nodiscard]] enum spg_status
spg_journal_writer_open(struct spg_journal_writer *writer, const char *path);
[[nodiscard]] enum spg_status
spg_journal_writer_append(struct spg_journal_writer *writer,
                          uint64_t timestamp_ns, uint64_t parent_sequence,
                          enum spg_journal_event_kind event_kind,
                          enum spg_status event_status, size_t payload_n,
                          const uint8_t payload[],
                          uint64_t *out_sequence);
[[nodiscard]] enum spg_status
spg_journal_writer_close(struct spg_journal_writer *writer);

[[nodiscard]] enum spg_status
spg_journal_reader_open(struct spg_journal_reader *reader, const char *path);
[[nodiscard]] enum spg_status
spg_journal_reader_next(struct spg_journal_reader *reader,
                        size_t payload_capacity,
                        uint8_t payload[],
                        struct spg_journal_record *out);
[[nodiscard]] enum spg_status
spg_journal_reader_close(struct spg_journal_reader *reader);

#ifdef __cplusplus
}
#endif

#endif
