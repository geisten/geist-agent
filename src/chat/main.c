#include "sporegeist/sporegeist.h"

#include "sporegeist/chat_template.h"
#include "sporegeist/model_adapter.h"
#include "sporegeist/model_resolve.h"
#include "sporegeist/status.h"

#include <geist.h>

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHAT_LINE_BYTES    4096u
#define CHAT_OUTPUT_BYTES  16384u
#define CHAT_HISTORY_BYTES 6144u /* ~ the CHAT_MAX_SEQ_LEN token budget */
#define CHAT_DEF_TOKENS    256u
#define CHAT_MAX_SEQ_LEN   2048u

#ifndef PATH_MAX
#    define PATH_MAX 4096
#endif

static void print_usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--model <path>] [--max-tokens <n>] [--no-download] "
            "[--fake] [--transcript <path>]\n"
            "\n"
            "Interactive sporegeist chat REPL.\n"
            "Connects to a Gemma 4 GGUF via the geist engine. The model path is\n"
            "taken from --model, then $SPOREGEIST_MODEL, then "
            "./gguf_artifacts/gemma4-e2b-Q4_K_M.gguf;\n"
            "a missing default is auto-downloaded with curl unless --no-download.\n"
            "Use --fake to run offline without a model. Type /quit to exit, "
            "/reset to clear context.\n",
            argv0);
}

static void write_json_string(FILE *file, const char *text) {
    fputc('"', file);
    if (text != nullptr) {
        for (size_t i = 0u; text[i] != '\0'; i += 1u) {
            const unsigned char ch = (unsigned char)text[i];
            if (ch == '"' || ch == '\\') {
                fputc('\\', file);
                fputc((int)ch, file);
            } else if (ch == '\n') {
                fputs("\\n", file);
            } else if (ch == '\r') {
                fputs("\\r", file);
            } else if (ch == '\t') {
                fputs("\\t", file);
            } else if (ch >= 32u && ch <= 126u) {
                fputc((int)ch, file);
            } else {
                fprintf(file, "\\u%04x", (unsigned int)ch);
            }
        }
    }
    fputc('"', file);
}

static int transcript_write(FILE *file, const char *role, const char *text) {
    if (file == nullptr) {
        return 0;
    }
    fputs("{\"role\":", file);
    write_json_string(file, role);
    fputs(",\"content\":", file);
    write_json_string(file, text);
    fputs("}\n", file);
    return fflush(file) == 0 ? 0 : 1;
}

static void trim_newline(char *line) {
    if (line == nullptr) {
        return;
    }
    const size_t n = strlen(line);
    if (n > 0u && line[n - 1u] == '\n') {
        line[n - 1u] = '\0';
    }
}

/* geist renders Gemma's turn/EOS tokens as literal surface text, so cut the
 * reply at the first stop marker the model emits. */
static void trim_at_stop_markers(char *text) {
    static const char *const markers[] = {
        "<end_of_turn>", "<start_of_turn>", "<eos>", "<turn|>",
    };
    char *cut = nullptr;
    for (size_t i = 0u; i < sizeof markers / sizeof markers[0]; i += 1u) {
        char *hit = strstr(text, markers[i]);
        if (hit != nullptr && (cut == nullptr || hit < cut)) {
            cut = hit;
        }
    }
    if (cut != nullptr) {
        *cut = '\0';
    }
}

struct chat_args {
    const char *model_path;
    const char *transcript_path;
    size_t      max_tokens;
    bool        allow_download;
    bool        fake;
};

/* Parse argv into args. Returns 0 on success, 1 to exit 0 (--help/--version),
 * 2 on a usage error. */
static int parse_args(int argc, char **argv, struct chat_args *out) {
    *out = (struct chat_args){
        .max_tokens     = CHAT_DEF_TOKENS,
        .allow_download = true,
    };
    for (int i = 1; i < argc; i += 1) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 1;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("sporegeist-chat %s\n", SPG_VERSION_STRING);
            printf("libgeist %s\n", geist_version_string());
            return 1;
        }
        if (strcmp(argv[i], "--no-download") == 0) {
            out->allow_download = false;
            continue;
        }
        if (strcmp(argv[i], "--fake") == 0) {
            out->fake = true;
            continue;
        }
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            out->model_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--transcript") == 0 && i + 1 < argc) {
            out->transcript_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--max-tokens") == 0 && i + 1 < argc) {
            char           *end = nullptr;
            const unsigned long v = strtoul(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || v == 0u) {
                fprintf(stderr, "sporegeist-chat: invalid --max-tokens\n");
                return 2;
            }
            out->max_tokens = (size_t)v;
            continue;
        }
        fprintf(stderr, "sporegeist-chat: unknown or incomplete argument: %s\n",
                argv[i]);
        return 2;
    }
    return 0;
}

/* Initialize the model adapter (real or fake). Returns SPG_OK on success. */
static enum spg_status init_adapter(const struct chat_args   *args,
                                    struct spg_model_adapter *adapter) {
    if (args->fake) {
        static const char fake_reply[] =
            "(offline fake model — pass a real --model to generate)";
        const struct spg_model_adapter_config cfg = {
            .kind            = SPG_MODEL_ADAPTER_FAKE,
            .sampling        = {.top_p = 1.0f},
            .fake_response   = fake_reply,
            .fake_response_n = sizeof fake_reply - 1u,
        };
        return spg_model_adapter_init(adapter, &cfg);
    }

    char path[PATH_MAX];
    bool downloaded = false;
    const struct spg_model_resolve_opts ropts = {
        .explicit_path  = args->model_path,
        .allow_download = args->allow_download,
    };
    const enum spg_status rstatus =
        spg_model_resolve(&ropts, sizeof path, path, &downloaded);
    if (rstatus != SPG_OK) {
        fprintf(stderr,
                "sporegeist-chat: no model (%s). Pass --model <path>, set "
                "$SPOREGEIST_MODEL, run `make fetch-model`, or use --fake.\n",
                spg_status_to_string(rstatus));
        return rstatus;
    }
    if (downloaded) {
        fprintf(stderr, "sporegeist-chat: download complete.\n");
    }
    fprintf(stderr, "sporegeist-chat: loading model %s ...\n", path);

    const struct spg_model_adapter_config cfg = {
        .kind         = SPG_MODEL_ADAPTER_GEIST,
        .model_path   = path,
        .backend_name = nullptr, /* "auto" */
        .sampling     = {.max_seq_len = CHAT_MAX_SEQ_LEN,
                         .temperature = 0.0f,
                         .top_p       = 1.0f,
                         .top_k       = 0,
                         .random_seed = 0u},
    };
    const enum spg_status istatus = spg_model_adapter_init(adapter, &cfg);
    if (istatus != SPG_OK) {
        fprintf(stderr, "sporegeist-chat: model load failed (%s)\n",
                spg_status_to_string(istatus));
    }
    return istatus;
}

int main(int argc, char **argv) {
    struct chat_args args = {};
    const int        pa   = parse_args(argc, argv, &args);
    if (pa != 0) {
        if (pa == 2) {
            print_usage(argv[0]);
        }
        return pa == 1 ? 0 : pa;
    }

    struct spg_model_adapter adapter = {};
    if (init_adapter(&args, &adapter) != SPG_OK) {
        return 1;
    }

    FILE *transcript = nullptr;
    if (args.transcript_path != nullptr) {
        transcript = fopen(args.transcript_path, "ab");
        if (transcript == nullptr) {
            fprintf(stderr, "sporegeist-chat: transcript open failed\n");
            spg_model_adapter_destroy(&adapter);
            return 1;
        }
    }

    puts("sporegeist-chat ready. Type /quit to exit, /reset to clear context.");
    char                    line[CHAT_LINE_BYTES];
    char                    output[CHAT_OUTPUT_BYTES];
    char                    history_buf[CHAT_HISTORY_BYTES];
    struct spg_chat_history history;
    spg_chat_history_init(&history, sizeof history_buf, history_buf);
    int rc = 0;
    for (;;) {
        fputs("user> ", stdout);
        fflush(stdout);
        if (fgets(line, sizeof line, stdin) == nullptr) {
            break;
        }
        trim_newline(line);
        if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0) {
            break;
        }
        if (strcmp(line, "/reset") == 0) {
            spg_chat_history_reset(&history);
            puts("assistant> (context cleared)");
            continue;
        }
        if (line[0] == '\0') {
            continue;
        }
        if (transcript_write(transcript, "user", line) != 0) {
            fprintf(stderr, "sporegeist-chat: transcript write failed\n");
            rc = 1;
            break;
        }

        /* Real path: feed the whole templated transcript each turn (reset +
         * re-prefill). Fake path: just echo the raw line. */
        const char *prompt = line;
        bool        reset  = false;
        if (!args.fake) {
            if (spg_chat_history_add_user(&history, line) != SPG_OK) {
                puts("assistant> (message too long for the context window)");
                continue;
            }
            prompt = spg_chat_history_text(&history);
            reset  = true;
        }

        const struct spg_model_generate_request req = {
            .prompt            = prompt,
            .prompt_n          = strlen(prompt),
            .reset_session     = reset,
            .max_decode_tokens = args.max_tokens,
        };
        struct spg_model_generate_result result = {
            .output_capacity = sizeof output,
            .output          = output,
        };
        const enum spg_status gstatus =
            spg_model_generate(&adapter, &req, &result);

        const char *reply =
            gstatus == SPG_OK || gstatus == SPG_E_LIMIT ? output : nullptr;
        if (reply != nullptr) {
            if (!args.fake) {
                trim_at_stop_markers(output);
                (void)spg_chat_history_add_assistant(&history, output);
            }
            printf("assistant> %s%s\n", reply,
                   result.output_truncated ? " …[truncated]" : "");
            if (transcript_write(transcript, "assistant", reply) != 0) {
                fprintf(stderr, "sporegeist-chat: transcript write failed\n");
                rc = 1;
                break;
            }
        } else {
            printf("assistant> (generation error: %s)\n",
                   spg_status_to_string(gstatus));
        }
    }

    spg_model_adapter_destroy(&adapter);
    if (transcript != nullptr && fclose(transcript) != 0) {
        fprintf(stderr, "sporegeist-chat: transcript close failed\n");
        return 1;
    }
    return rc;
}
