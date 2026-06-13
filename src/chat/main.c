#include "sporegeist/sporegeist.h"

#include <geist.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CHAT_LINE_BYTES 4096u

static void print_usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--transcript <path>]\n"
            "\n"
            "Starts the minimal sporegeist chat REPL shell.\n"
            "This app records user messages but does not run a model yet.\n",
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

int main(int argc, char **argv) {
    const char *transcript_path = nullptr;
    for (int i = 1; i < argc; i += 1) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("sporegeist-chat %s\n", SPG_VERSION_STRING);
            printf("libgeist %s\n", geist_version_string());
            return 0;
        }
        if (strcmp(argv[i], "--transcript") == 0 && i + 1 < argc) {
            transcript_path = argv[i + 1];
            i += 1;
            continue;
        }
        fprintf(stderr, "sporegeist-chat: unknown or incomplete argument: %s\n",
                argv[i]);
        print_usage(argv[0]);
        return 2;
    }

    FILE *transcript = nullptr;
    if (transcript_path != nullptr) {
        transcript = fopen(transcript_path, "ab");
        if (transcript == nullptr) {
            fprintf(stderr, "sporegeist-chat: transcript open failed\n");
            return 1;
        }
    }

    puts("sporegeist-chat ready. Type /quit to exit.");
    char line[CHAT_LINE_BYTES];
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
        if (transcript_write(transcript, "user", line) != 0) {
            fprintf(stderr, "sporegeist-chat: transcript write failed\n");
            if (transcript != nullptr) {
                (void)fclose(transcript);
            }
            return 1;
        }
        puts("assistant> model adapter not wired in chat app yet");
        if (transcript_write(transcript, "assistant",
                             "model adapter not wired in chat app yet") != 0) {
            fprintf(stderr, "sporegeist-chat: transcript write failed\n");
            if (transcript != nullptr) {
                (void)fclose(transcript);
            }
            return 1;
        }
    }

    if (transcript != nullptr && fclose(transcript) != 0) {
        fprintf(stderr, "sporegeist-chat: transcript close failed\n");
        return 1;
    }
    return 0;
}
