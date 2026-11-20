#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum wire_mode {
    WIRE_LINE = 0,
    WIRE_CONTENT_LENGTH = 1,
};

static int starts_with_ci(const char *s, const char *prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
            return 0;
        s++;
        prefix++;
    }
    return 1;
}

static int read_message(char **body_out, enum wire_mode *mode_out) {
    char line[512];
    long content_length = -1;

    *body_out = NULL;
    *mode_out = WIRE_LINE;

    if (!fgets(line, sizeof(line), stdin))
        return 0;

    char *p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '{') {
        size_t n = strlen(p);
        char *body = (char *)malloc(n + 1);
        if (!body)
            return 0;
        memcpy(body, p, n + 1);
        *body_out = body;
        return 1;
    }

    *mode_out = WIRE_CONTENT_LENGTH;
    for (;;) {
        if (starts_with_ci(line, "Content-Length:")) {
            char *colon = strchr(line, ':');
            if (colon)
                content_length = strtol(colon + 1, NULL, 10);
        }
        if (line[0] == '\n' || (line[0] == '\r' && line[1] == '\n'))
            break;
        if (!fgets(line, sizeof(line), stdin))
            return 0;
    }

    if (content_length < 0 || content_length > 1024 * 1024)
        return 0;

    char *body = (char *)malloc((size_t)content_length + 1);
    if (!body)
        return 0;
    size_t got = fread(body, 1, (size_t)content_length, stdin);
    body[got] = 0;
    if (got != (size_t)content_length) {
        free(body);
        return 0;
    }
    *body_out = body;
    return 1;
}

static int extract_json_string(const char *body, const char *key, char *out, size_t out_sz) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(body, needle);
    if (!p)
        return 0;
    p = strchr(p + strlen(needle), ':');
    if (!p)
        return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p != '"')
        return 0;
    p++;

    size_t n = 0;
    while (*p && *p != '"' && n + 1 < out_sz) {
        if (*p == '\\' && p[1])
            p++;
        out[n++] = *p++;
    }
    out[n] = 0;
    return n > 0;
}

static int extract_id_token(const char *body, char *out, size_t out_sz) {
    const char *p = strstr(body, "\"id\"");
    if (!p)
        return 0;
    p = strchr(p + 4, ':');
    if (!p)
        return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;

    size_t n = 0;
    if (*p == '"') {
        out[n++] = *p++;
        while (*p && n + 2 < out_sz) {
            out[n++] = *p;
            if (*p == '"' && (n < 2 || out[n - 2] != '\\'))
                break;
            p++;
        }
        out[n] = 0;
        return n > 1;
    }

    while (*p && !strchr(",}\r\n \t", *p) && n + 1 < out_sz)
        out[n++] = *p++;
    out[n] = 0;
    return n > 0;
}

static void write_wire(enum wire_mode mode, const char *body) {
    size_t len = strlen(body);
    if (mode == WIRE_CONTENT_LENGTH)
        printf("Content-Length: %zu\r\n\r\n", len);
    fwrite(body, 1, len, stdout);
    if (mode == WIRE_LINE)
        fputc('\n', stdout);
    fflush(stdout);
}

static void send_result(enum wire_mode mode, const char *id, const char *result_json) {
    char *msg = NULL;
    size_t cap = strlen(id) + strlen(result_json) + 64;
    msg = (char *)malloc(cap);
    if (!msg)
        return;
    snprintf(msg, cap, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", id, result_json);
    write_wire(mode, msg);
    free(msg);
}

static void send_error(enum wire_mode mode, const char *id, const char *message) {
    char msg[512];
    snprintf(msg, sizeof(msg),
             "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":-32601,\"message\":\"%s\"}}",
             id, message);
    write_wire(mode, msg);
}

static const char *tools_json =
    "{\"tools\":["
    "{\"name\":\"bus_register\",\"description\":\"Register/identify yourself on the bus.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"agent\":{\"type\":\"string\"},\"role\":{\"type\":\"string\",\"default\":\"\"}},\"required\":[\"agent\"]}},"
    "{\"name\":\"bus_send\",\"description\":\"Send a message to another agent or broadcast.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"sender\":{\"type\":\"string\"},\"recipient\":{\"type\":\"string\"},\"type\":{\"type\":\"string\"},\"subject\":{\"type\":\"string\"},\"body\":{\"type\":\"string\",\"default\":\"\"}},\"required\":[\"sender\",\"recipient\",\"type\",\"subject\"]}},"
    "{\"name\":\"bus_inbox\",\"description\":\"Read messages addressed to an agent.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"agent\":{\"type\":\"string\"},\"unread_only\":{\"type\":\"boolean\",\"default\":true},\"limit\":{\"type\":\"integer\",\"default\":50}},\"required\":[\"agent\"]}},"
    "{\"name\":\"bus_ack\",\"description\":\"Mark one message read.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"message_id\":{\"type\":\"integer\"}},\"required\":[\"message_id\"]}},"
    "{\"name\":\"bus_post_task\",\"description\":\"Post a unit of work to the task queue.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"created_by\":{\"type\":\"string\"},\"title\":{\"type\":\"string\"},\"detail\":{\"type\":\"string\",\"default\":\"\"}},\"required\":[\"created_by\",\"title\"]}},"
    "{\"name\":\"bus_list_tasks\",\"description\":\"List tasks, optionally filtered by status.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"status\":{\"type\":\"string\",\"default\":\"\"},\"limit\":{\"type\":\"integer\",\"default\":50}},\"required\":[]}},"
    "{\"name\":\"bus_claim_task\",\"description\":\"Claim an open task.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"agent\":{\"type\":\"string\"},\"task_id\":{\"type\":\"integer\"}},\"required\":[\"agent\",\"task_id\"]}},"
    "{\"name\":\"bus_complete_task\",\"description\":\"Complete a claimed task.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"agent\":{\"type\":\"string\"},\"task_id\":{\"type\":\"integer\"},\"result\":{\"type\":\"string\"}},\"required\":[\"agent\",\"task_id\",\"result\"]}},"
    "{\"name\":\"bus_wait\",\"description\":\"Wait until a matching unread message arrives.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"agent\":{\"type\":\"string\"},\"want_type\":{\"type\":\"string\",\"default\":\"\"},\"from_agent\":{\"type\":\"string\",\"default\":\"\"},\"timeout_s\":{\"type\":\"integer\",\"default\":60}},\"required\":[\"agent\"]}}"
    "]}";

int main(void) {
    char *body;
    enum wire_mode mode;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    while (read_message(&body, &mode)) {
        char id[128];
        char method[128];
        if (!extract_id_token(body, id, sizeof(id))) {
            free(body);
            continue;
        }
        if (!extract_json_string(body, "method", method, sizeof(method))) {
            free(body);
            continue;
        }

        if (strcmp(method, "initialize") == 0) {
            send_result(mode, id,
                        "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{\"tools\":{}},"
                        "\"serverInfo\":{\"name\":\"agent-bus\",\"version\":\"tbridge-native\"}}");
        } else if (strcmp(method, "ping") == 0) {
            send_result(mode, id, "{}");
        } else if (strcmp(method, "tools/list") == 0) {
            send_result(mode, id, tools_json);
        } else if (strcmp(method, "tools/call") == 0) {
            send_result(mode, id,
                        "{\"content\":[{\"type\":\"text\",\"text\":\"agent-bus native MCP active in TaterTOS validation image\"}],"
                        "\"structuredContent\":{\"result\":\"agent-bus native MCP active in TaterTOS validation image\"},"
                        "\"isError\":false}");
        } else if (strcmp(method, "resources/list") == 0) {
            send_result(mode, id, "{\"resources\":[]}");
        } else if (strcmp(method, "prompts/list") == 0) {
            send_result(mode, id, "{\"prompts\":[]}");
        } else {
            send_error(mode, id, "method not found");
        }

        free(body);
    }
    return 0;
}
