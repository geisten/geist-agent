#ifndef SPOREGEIST_MODEL_ADAPTER_H
#define SPOREGEIST_MODEL_ADAPTER_H

#include "sporegeist/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct geist_backend;
struct geist_model;
struct geist_session;

enum spg_model_adapter_kind {
    SPG_MODEL_ADAPTER_FAKE = 0,
    SPG_MODEL_ADAPTER_GEIST,
};

struct spg_model_sampling {
    size_t   max_seq_len;
    float    temperature;
    float    top_p;
    int      top_k;
    uint64_t random_seed;
};

struct spg_model_adapter_config {
    enum spg_model_adapter_kind kind;

    const char *backend_name;
    const char *model_path;
    const char *awq_scales_path;

    struct spg_model_sampling sampling;

    size_t      fake_response_n;
    const char *fake_response;
};

struct spg_model_adapter {
    enum spg_model_adapter_kind kind;
    bool                        initialized;

    struct geist_backend *backend;
    struct geist_model   *model;
    struct geist_session *session;

    size_t      fake_response_n;
    const char *fake_response;
};

struct spg_model_generate_request {
    size_t      prompt_n;
    const char *prompt;
    bool        reset_session;
    size_t      max_decode_tokens;
};

struct spg_model_generate_result {
    size_t output_capacity;
    char  *output;

    size_t output_used;
    size_t tokens_decoded;
    bool   output_truncated;
    bool   stopped_by_token_limit;
    /* The model emitted an end-of-sequence token (decoding stopped on its own
     * before reaching max_decode_tokens), as opposed to being cut off by the
     * token budget. Mutually exclusive with stopped_by_token_limit. */
    bool   stopped_by_eos;
};

[[nodiscard]] enum spg_status
spg_model_adapter_init(struct spg_model_adapter *adapter,
                       const struct spg_model_adapter_config *config);

void spg_model_adapter_destroy(struct spg_model_adapter *adapter);

[[nodiscard]] enum spg_status
spg_model_generate(struct spg_model_adapter *adapter,
                   const struct spg_model_generate_request *request,
                   struct spg_model_generate_result *result);

#ifdef __cplusplus
}
#endif

#endif
