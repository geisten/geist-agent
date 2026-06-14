#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#    define _DARWIN_C_SOURCE 1
#endif

#include "sporegeist/chat_tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int open_temp(struct spg_mem_store *store, char dir[static 64]) {
    memcpy(dir, "/tmp/spg_chattool_XXXXXX", 25u);
    if (mkdtemp(dir) == nullptr) {
        return 1;
    }
    return spg_mem_store_open(store, dir) == SPG_OK ? 0 : 1;
}

static int test_not_a_tool(void) {
    struct spg_mem_store store;
    char                 dir[64];
    if (open_temp(&store, dir) != 0) {
        return 1;
    }
    char out[64];
    bool was_tool = true;
    const char text[] = "Just a normal reply, not a tool call.";
    if (spg_chat_tool_dispatch(&store, strlen(text), text, sizeof out, out,
                               &was_tool) != SPG_OK) {
        return 1;
    }
    return (!was_tool && out[0] == '\0') ? 0 : 1;
}

static int test_save_then_read(void) {
    struct spg_mem_store store;
    char                 dir[64];
    if (open_temp(&store, dir) != 0) {
        return 1;
    }
    char out[1024];
    bool was_tool = false;

    const char save[] =
        "(tool memory_save (slug \"fav\") (description \"a fav\") "
        "(body \"the body content\"))";
    if (spg_chat_tool_dispatch(&store, strlen(save), save, sizeof out, out,
                               &was_tool) != SPG_OK ||
        !was_tool || strstr(out, "saved fav") == nullptr) {
        return 1;
    }

    const char read[] = "(tool memory_read (slug \"fav\"))";
    if (spg_chat_tool_dispatch(&store, strlen(read), read, sizeof out, out,
                               &was_tool) != SPG_OK ||
        !was_tool || strstr(out, "the body content") == nullptr) {
        return 1;
    }

    const char list[] = "(tool memory_list)";
    if (spg_chat_tool_dispatch(&store, strlen(list), list, sizeof out, out,
                               &was_tool) != SPG_OK ||
        !was_tool || strstr(out, "fav: a fav") == nullptr) {
        return 1;
    }
    return 0;
}

static int test_delete(void) {
    struct spg_mem_store store;
    char                 dir[64];
    if (open_temp(&store, dir) != 0) {
        return 1;
    }
    if (spg_mem_save(&store, "gone", "d", "b") != SPG_OK) {
        return 1;
    }
    char out[256];
    bool was_tool = false;
    const char del[] = "(tool memory_delete (slug \"gone\"))";
    if (spg_chat_tool_dispatch(&store, strlen(del), del, sizeof out, out,
                               &was_tool) != SPG_OK ||
        !was_tool || strstr(out, "deleted gone") == nullptr) {
        return 1;
    }
    char body[64];
    return spg_mem_read(&store, "gone", sizeof body, body, nullptr) ==
                   SPG_E_NOT_FOUND
               ? 0
               : 1;
}

static int test_unknown_and_bad_args(void) {
    struct spg_mem_store store;
    char                 dir[64];
    if (open_temp(&store, dir) != 0) {
        return 1;
    }
    char out[128];
    bool was_tool = false;
    const char unk[] = "(tool memory_frobnicate (slug \"x\"))";
    if (spg_chat_tool_dispatch(&store, strlen(unk), unk, sizeof out, out,
                               &was_tool) != SPG_OK ||
        !was_tool || strstr(out, "unknown tool") == nullptr) {
        return 1;
    }
    const char bad[] = "(tool memory_read)"; /* missing slug */
    if (spg_chat_tool_dispatch(&store, strlen(bad), bad, sizeof out, out,
                               &was_tool) != SPG_OK ||
        !was_tool || strstr(out, "error") == nullptr) {
        return 1;
    }
    return 0;
}

int main(void) {
    if (test_not_a_tool() != 0) {
        fprintf(stderr, "test_not_a_tool failed\n");
        return 1;
    }
    if (test_save_then_read() != 0) {
        fprintf(stderr, "test_save_then_read failed\n");
        return 1;
    }
    if (test_delete() != 0) {
        fprintf(stderr, "test_delete failed\n");
        return 1;
    }
    if (test_unknown_and_bad_args() != 0) {
        fprintf(stderr, "test_unknown_and_bad_args failed\n");
        return 1;
    }
    return 0;
}
