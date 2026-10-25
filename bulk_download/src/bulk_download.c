#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define DEFAULT_WORKERS 8U
#define DEFAULT_PARTITIONS 256U

struct buffer {
    char *data;
    size_t length;
    size_t capacity;
};

struct entry {
    char *word;
    char *url;
};

struct entry_list {
    struct entry *items;
    size_t length;
    size_t capacity;
};

struct options {
    const char *map_path;
    const char *extractor_path;
    const char *output_path;
    size_t workers;
    size_t partitions;
    size_t limit;
    bool force;
    bool debug;
};

struct pool {
    const struct options *options;
    const struct entry_list *entries;
    pthread_mutex_t lock;
    size_t next;
    size_t completed;
    size_t downloaded;
    size_t skipped;
    size_t unavailable;
    size_t failed;
};

struct worker {
    struct pool *pool;
    size_t id;
};

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void usage(FILE *stream)
{
    fprintf(stream,
            "usage: bulk_download [options]\n"
            "\n"
            "Build a sharded JSON index with a bounded worker pool.\n"
            "\n"
            "options:\n"
            "  --map PATH         headword-to-URL JSON map\n"
            "                     (default: ../etymwb-headwords.json)\n"
            "  --extractor PATH   extract_word executable\n"
            "                     (default: ../extract_word/bin/extract_word)\n"
            "  --output DIR       index directory (default: word_index)\n"
            "  --workers N        concurrent extractor processes (default: 8)\n"
            "  --partitions N     deterministic index partitions (default: 256)\n"
            "  --limit N          process only the first N entries (testing)\n"
            "  --force            replace existing records\n"
            "  --debug            log scheduling and extractor details\n"
            "  --help             show this help\n");
}

static void log_message(const char *level, size_t worker_id,
                        const char *format, ...)
{
    struct timespec now;
    struct tm local;
    char timestamp[32];
    va_list arguments;

    clock_gettime(CLOCK_REALTIME, &now);
    localtime_r(&now.tv_sec, &local);
    strftime(timestamp, sizeof timestamp, "%Y-%m-%dT%H:%M:%S%z", &local);

    pthread_mutex_lock(&log_lock);
    fprintf(stderr, "%s level=%s", timestamp, level);
    if (worker_id != SIZE_MAX) {
        fprintf(stderr, " worker=%zu", worker_id);
    }
    fputc(' ', stderr);
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
    fflush(stderr);
    pthread_mutex_unlock(&log_lock);
}

static void die_errno(const char *operation, const char *path)
{
    log_message("ERROR", SIZE_MAX, "%s path=\"%s\" error=\"%s\"",
                operation, path, strerror(errno));
    exit(EXIT_FAILURE);
}

static void *xrealloc(void *pointer, size_t size)
{
    void *result = realloc(pointer, size);
    if (result == NULL && size != 0) {
        log_message("ERROR", SIZE_MAX, "out of memory");
        exit(EXIT_FAILURE);
    }
    return result;
}

static char *xstrdup(const char *text)
{
    char *copy = strdup(text);
    if (copy == NULL) {
        log_message("ERROR", SIZE_MAX, "out of memory");
        exit(EXIT_FAILURE);
    }
    return copy;
}

static void buffer_init(struct buffer *buffer)
{
    buffer->capacity = 256;
    buffer->length = 0;
    buffer->data = xrealloc(NULL, buffer->capacity);
    buffer->data[0] = '\0';
}

static void buffer_append(struct buffer *buffer, const char *text, size_t length)
{
    if (length > SIZE_MAX - buffer->length - 1) {
        log_message("ERROR", SIZE_MAX, "buffer is too large");
        exit(EXIT_FAILURE);
    }
    size_t required = buffer->length + length + 1;
    if (required > buffer->capacity) {
        size_t capacity = buffer->capacity;
        while (capacity < required) {
            capacity = capacity > SIZE_MAX / 2 ? required : capacity * 2;
        }
        buffer->data = xrealloc(buffer->data, capacity);
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
}

static void buffer_byte(struct buffer *buffer, unsigned char byte)
{
    char value = (char)byte;
    buffer_append(buffer, &value, 1);
}

static char *buffer_take(struct buffer *buffer)
{
    char *data = buffer->data;
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
    return data;
}

static char *read_file(const char *path)
{
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) {
        return NULL;
    }
    struct buffer result;
    char block[8192];
    size_t count;
    buffer_init(&result);
    while ((count = fread(block, 1, sizeof block, stream)) > 0) {
        buffer_append(&result, block, count);
    }
    if (ferror(stream)) {
        int saved = errno;
        fclose(stream);
        free(result.data);
        errno = saved;
        return NULL;
    }
    fclose(stream);
    return buffer_take(&result);
}

static bool hex_digit(char value, unsigned *digit)
{
    if (value >= '0' && value <= '9') {
        *digit = (unsigned)(value - '0');
    } else if (value >= 'a' && value <= 'f') {
        *digit = (unsigned)(value - 'a') + 10U;
    } else if (value >= 'A' && value <= 'F') {
        *digit = (unsigned)(value - 'A') + 10U;
    } else {
        return false;
    }
    return true;
}

static bool parse_hex4(const char **cursor, uint32_t *value)
{
    uint32_t result = 0;
    for (size_t i = 0; i < 4; ++i) {
        unsigned digit;
        if ((*cursor)[i] == '\0' || !hex_digit((*cursor)[i], &digit)) {
            return false;
        }
        result = result * 16U + digit;
    }
    *cursor += 4;
    *value = result;
    return true;
}

static void append_utf8(struct buffer *buffer, uint32_t codepoint)
{
    if (codepoint <= 0x7fU) {
        buffer_byte(buffer, (unsigned char)codepoint);
    } else if (codepoint <= 0x7ffU) {
        buffer_byte(buffer, (unsigned char)(0xc0U | (codepoint >> 6)));
        buffer_byte(buffer, (unsigned char)(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0xffffU) {
        buffer_byte(buffer, (unsigned char)(0xe0U | (codepoint >> 12)));
        buffer_byte(buffer, (unsigned char)(0x80U | ((codepoint >> 6) & 0x3fU)));
        buffer_byte(buffer, (unsigned char)(0x80U | (codepoint & 0x3fU)));
    } else {
        buffer_byte(buffer, (unsigned char)(0xf0U | (codepoint >> 18)));
        buffer_byte(buffer, (unsigned char)(0x80U | ((codepoint >> 12) & 0x3fU)));
        buffer_byte(buffer, (unsigned char)(0x80U | ((codepoint >> 6) & 0x3fU)));
        buffer_byte(buffer, (unsigned char)(0x80U | (codepoint & 0x3fU)));
    }
}

static char *parse_json_string(const char **cursor)
{
    if (**cursor != '"') {
        return NULL;
    }
    ++*cursor;
    struct buffer result;
    buffer_init(&result);
    while (**cursor != '\0' && **cursor != '"') {
        unsigned char byte = (unsigned char)*(*cursor)++;
        if (byte < 0x20U) {
            free(result.data);
            return NULL;
        }
        if (byte != '\\') {
            buffer_byte(&result, byte);
            continue;
        }
        char escape = *(*cursor)++;
        switch (escape) {
        case '"': buffer_byte(&result, '"'); break;
        case '\\': buffer_byte(&result, '\\'); break;
        case '/': buffer_byte(&result, '/'); break;
        case 'b': buffer_byte(&result, '\b'); break;
        case 'f': buffer_byte(&result, '\f'); break;
        case 'n': buffer_byte(&result, '\n'); break;
        case 'r': buffer_byte(&result, '\r'); break;
        case 't': buffer_byte(&result, '\t'); break;
        case 'u': {
            uint32_t first;
            if (!parse_hex4(cursor, &first)) {
                free(result.data);
                return NULL;
            }
            uint32_t codepoint = first;
            if (first >= 0xd800U && first <= 0xdbffU) {
                if ((*cursor)[0] != '\\' || (*cursor)[1] != 'u') {
                    free(result.data);
                    return NULL;
                }
                *cursor += 2;
                uint32_t second;
                if (!parse_hex4(cursor, &second) ||
                    second < 0xdc00U || second > 0xdfffU) {
                    free(result.data);
                    return NULL;
                }
                codepoint = 0x10000U + ((first - 0xd800U) << 10) +
                            (second - 0xdc00U);
            } else if (first >= 0xdc00U && first <= 0xdfffU) {
                free(result.data);
                return NULL;
            }
            append_utf8(&result, codepoint);
            break;
        }
        default:
            free(result.data);
            return NULL;
        }
    }
    if (**cursor != '"') {
        free(result.data);
        return NULL;
    }
    ++*cursor;
    return buffer_take(&result);
}

static void skip_space(const char **cursor)
{
    while (isspace((unsigned char)**cursor)) {
        ++*cursor;
    }
}

static bool parse_map(const char *json, struct entry_list *entries)
{
    const char *cursor = json;
    skip_space(&cursor);
    if (*cursor++ != '{') {
        return false;
    }
    skip_space(&cursor);
    while (*cursor != '}') {
        char *word = parse_json_string(&cursor);
        if (word == NULL) {
            return false;
        }
        skip_space(&cursor);
        if (*cursor++ != ':') {
            free(word);
            return false;
        }
        skip_space(&cursor);
        char *url = parse_json_string(&cursor);
        if (url == NULL) {
            free(word);
            return false;
        }
        if (entries->length == entries->capacity) {
            size_t capacity = entries->capacity == 0 ? 256 : entries->capacity * 2;
            entries->items = xrealloc(entries->items,
                                      capacity * sizeof *entries->items);
            entries->capacity = capacity;
        }
        entries->items[entries->length++] = (struct entry){word, url};
        skip_space(&cursor);
        if (*cursor == ',') {
            ++cursor;
            skip_space(&cursor);
        } else if (*cursor != '}') {
            return false;
        }
    }
    ++cursor;
    skip_space(&cursor);
    return *cursor == '\0';
}

static bool parse_size(const char *text, size_t minimum, size_t *result)
{
    char *end;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' ||
        value < minimum || value > SIZE_MAX) {
        return false;
    }
    *result = (size_t)value;
    return true;
}

static void parse_options(int argc, char **argv, struct options *options)
{
    *options = (struct options){
        .map_path = "../etymwb-headwords.json",
        .extractor_path = "../extract_word/bin/extract_word",
        .output_path = "word_index",
        .workers = DEFAULT_WORKERS,
        .partitions = DEFAULT_PARTITIONS,
        .limit = SIZE_MAX,
    };
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            exit(EXIT_SUCCESS);
        } else if (strcmp(argv[i], "--force") == 0) {
            options->force = true;
        } else if (strcmp(argv[i], "--debug") == 0) {
            options->debug = true;
        } else if (i + 1 < argc && strcmp(argv[i], "--map") == 0) {
            options->map_path = argv[++i];
        } else if (i + 1 < argc && strcmp(argv[i], "--extractor") == 0) {
            options->extractor_path = argv[++i];
        } else if (i + 1 < argc && strcmp(argv[i], "--output") == 0) {
            options->output_path = argv[++i];
        } else if (i + 1 < argc && strcmp(argv[i], "--workers") == 0) {
            if (!parse_size(argv[++i], 1, &options->workers)) {
                log_message("ERROR", SIZE_MAX, "invalid --workers value=\"%s\"", argv[i]);
                exit(2);
            }
        } else if (i + 1 < argc && strcmp(argv[i], "--partitions") == 0) {
            if (!parse_size(argv[++i], 1, &options->partitions)) {
                log_message("ERROR", SIZE_MAX, "invalid --partitions value=\"%s\"", argv[i]);
                exit(2);
            }
        } else if (i + 1 < argc && strcmp(argv[i], "--limit") == 0) {
            if (!parse_size(argv[++i], 0, &options->limit)) {
                log_message("ERROR", SIZE_MAX, "invalid --limit value=\"%s\"", argv[i]);
                exit(2);
            }
        } else {
            log_message("ERROR", SIZE_MAX, "unknown or incomplete option=\"%s\"", argv[i]);
            usage(stderr);
            exit(2);
        }
    }
}

static int mkdir_one(const char *path)
{
    if (mkdir(path, 0755) == 0 || errno == EEXIST) {
        struct stat details;
        return stat(path, &details) == 0 && S_ISDIR(details.st_mode) ? 0 : -1;
    }
    return -1;
}

static int mkdir_p(const char *path)
{
    char *copy = xstrdup(path);
    for (char *cursor = copy + 1; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (mkdir_one(copy) != 0) {
                free(copy);
                return -1;
            }
            *cursor = '/';
        }
    }
    int result = mkdir_one(copy);
    free(copy);
    return result;
}

static uint64_t hash_word(const char *word)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *byte = (const unsigned char *)word; *byte; ++byte) {
        hash ^= *byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void json_string(FILE *stream, const char *text)
{
    fputc('"', stream);
    for (const unsigned char *byte = (const unsigned char *)text; *byte; ++byte) {
        switch (*byte) {
        case '"': fputs("\\\"", stream); break;
        case '\\': fputs("\\\\", stream); break;
        case '\b': fputs("\\b", stream); break;
        case '\f': fputs("\\f", stream); break;
        case '\n': fputs("\\n", stream); break;
        case '\r': fputs("\\r", stream); break;
        case '\t': fputs("\\t", stream); break;
        default:
            if (*byte < 0x20U) {
                fprintf(stream, "\\u%04x", *byte);
            } else {
                fputc(*byte, stream);
            }
        }
    }
    fputc('"', stream);
}

static char *record_path(const struct options *options, size_t ordinal,
                         const char *word)
{
    uint64_t hash = hash_word(word);
    size_t partition = (size_t)(hash % options->partitions);
    int needed = snprintf(NULL, 0, "%s/parts/part-%05zu/%08zu-%016llx.json",
                          options->output_path, partition, ordinal,
                          (unsigned long long)hash);
    char *path = xrealloc(NULL, (size_t)needed + 1);
    snprintf(path, (size_t)needed + 1,
             "%s/parts/part-%05zu/%08zu-%016llx.json",
             options->output_path, partition, ordinal,
             (unsigned long long)hash);
    return path;
}

static bool regular_nonempty_file(const char *path)
{
    struct stat details;
    return stat(path, &details) == 0 && S_ISREG(details.st_mode) &&
           details.st_size > 0;
}

static int ensure_parent(const char *path)
{
    char *copy = xstrdup(path);
    char *slash = strrchr(copy, '/');
    if (slash == NULL) {
        free(copy);
        return 0;
    }
    *slash = '\0';
    int result = mkdir_p(copy);
    free(copy);
    return result;
}

static bool output_looks_json(const char *path)
{
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) {
        return false;
    }
    int byte;
    do {
        byte = fgetc(stream);
    } while (byte != EOF && isspace((unsigned char)byte));
    fclose(stream);
    return byte == '{';
}

static int run_extractor(const struct options *options, size_t worker_id,
                         size_t ordinal, const struct entry *entry,
                         const char *destination)
{
    int needed = snprintf(NULL, 0, "%s.tmp.%ld.%zu", destination,
                          (long)getpid(), worker_id);
    char *temporary = xrealloc(NULL, (size_t)needed + 1);
    snprintf(temporary, (size_t)needed + 1, "%s.tmp.%ld.%zu", destination,
             (long)getpid(), worker_id);

    needed = snprintf(NULL, 0, "%s.error.log", destination);
    char *error_path = xrealloc(NULL, (size_t)needed + 1);
    snprintf(error_path, (size_t)needed + 1, "%s.error.log", destination);

    if (ensure_parent(destination) != 0) {
        log_message("ERROR", worker_id,
                    "event=mkdir item=%zu word=\"%s\" error=\"%s\"",
                    ordinal, entry->word, strerror(errno));
        free(temporary);
        free(error_path);
        return -1;
    }

    int output_fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (output_fd < 0) {
        log_message("ERROR", worker_id,
                    "event=open_output item=%zu path=\"%s\" error=\"%s\"",
                    ordinal, temporary, strerror(errno));
        free(temporary);
        free(error_path);
        return -1;
    }
    int error_fd = open(error_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (error_fd < 0) {
        log_message("ERROR", worker_id,
                    "event=open_error item=%zu path=\"%s\" error=\"%s\"",
                    ordinal, error_path, strerror(errno));
        close(output_fd);
        unlink(temporary);
        free(temporary);
        free(error_path);
        return -1;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, output_fd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, error_fd, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, output_fd);
    posix_spawn_file_actions_addclose(&actions, error_fd);

    char *const arguments[] = {
        (char *)options->extractor_path,
        entry->word,
        NULL,
    };
    pid_t child;
    int spawn_result = posix_spawn(&child, options->extractor_path, &actions,
                                   NULL, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(output_fd);
    close(error_fd);

    if (spawn_result != 0) {
        log_message("ERROR", worker_id,
                    "event=spawn item=%zu word=\"%s\" error=\"%s\"",
                    ordinal, entry->word, strerror(spawn_result));
        unlink(temporary);
        free(temporary);
        free(error_path);
        return -1;
    }

    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            log_message("ERROR", worker_id,
                        "event=wait item=%zu word=\"%s\" error=\"%s\"",
                        ordinal, entry->word, strerror(errno));
            unlink(temporary);
            free(temporary);
            free(error_path);
            return -1;
        }
    }

    bool succeeded = WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                     output_looks_json(temporary);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 6) {
        log_message("INFO", worker_id,
                    "event=unavailable item=%zu word=\"%s\" url=\"%s\"",
                    ordinal, entry->word, entry->url);
        unlink(temporary);
        unlink(error_path);
        free(temporary);
        free(error_path);
        return 1;
    }
    if (!succeeded) {
        if (WIFEXITED(status)) {
            log_message("WARN", worker_id,
                        "event=extract_failed item=%zu word=\"%s\" exit=%d stderr=\"%s\"",
                        ordinal, entry->word, WEXITSTATUS(status), error_path);
        } else if (WIFSIGNALED(status)) {
            log_message("WARN", worker_id,
                        "event=extract_failed item=%zu word=\"%s\" signal=%d stderr=\"%s\"",
                        ordinal, entry->word, WTERMSIG(status), error_path);
        } else {
            log_message("WARN", worker_id,
                        "event=extract_failed item=%zu word=\"%s\" stderr=\"%s\"",
                        ordinal, entry->word, error_path);
        }
        unlink(temporary);
        free(temporary);
        free(error_path);
        return -1;
    }

    if (rename(temporary, destination) != 0) {
        log_message("ERROR", worker_id,
                    "event=rename item=%zu path=\"%s\" error=\"%s\"",
                    ordinal, destination, strerror(errno));
        unlink(temporary);
        free(temporary);
        free(error_path);
        return -1;
    }
    unlink(error_path);
    free(temporary);
    free(error_path);
    return 0;
}

static void *worker_main(void *argument)
{
    struct worker *worker = argument;
    struct pool *pool = worker->pool;
    if (pool->options->debug) {
        log_message("DEBUG", worker->id, "event=worker_started");
    }
    for (;;) {
        pthread_mutex_lock(&pool->lock);
        size_t ordinal = pool->next++;
        pthread_mutex_unlock(&pool->lock);
        if (ordinal >= pool->entries->length) {
            break;
        }

        const struct entry *entry = &pool->entries->items[ordinal];
        char *destination = record_path(pool->options, ordinal, entry->word);
        bool skipped = !pool->options->force && regular_nonempty_file(destination);
        int result = 0;
        if (pool->options->debug) {
            log_message("DEBUG", worker->id,
                        "event=%s item=%zu word=\"%s\" path=\"%s\"",
                        skipped ? "skip" : "extract", ordinal, entry->word,
                        destination);
        }
        if (!skipped) {
            result = run_extractor(pool->options, worker->id, ordinal, entry,
                                   destination);
        }
        free(destination);

        pthread_mutex_lock(&pool->lock);
        ++pool->completed;
        if (skipped) {
            ++pool->skipped;
        } else if (result == 0) {
            ++pool->downloaded;
        } else if (result == 1) {
            ++pool->unavailable;
        } else {
            ++pool->failed;
        }
        size_t completed = pool->completed;
        size_t downloaded = pool->downloaded;
        size_t skipped_count = pool->skipped;
        size_t unavailable = pool->unavailable;
        size_t failed = pool->failed;
        pthread_mutex_unlock(&pool->lock);

        if (completed == pool->entries->length || completed % 100 == 0) {
            log_message("INFO", SIZE_MAX,
                        "event=progress completed=%zu total=%zu downloaded=%zu skipped=%zu unavailable=%zu failed=%zu",
                        completed, pool->entries->length, downloaded,
                        skipped_count, unavailable, failed);
        }
    }
    if (pool->options->debug) {
        log_message("DEBUG", worker->id, "event=worker_stopped");
    }
    return NULL;
}

static int write_manifest(const struct options *options,
                          const struct entry_list *entries)
{
    int needed = snprintf(NULL, 0, "%s/manifest.jsonl.tmp.%ld",
                          options->output_path, (long)getpid());
    char *temporary = xrealloc(NULL, (size_t)needed + 1);
    snprintf(temporary, (size_t)needed + 1, "%s/manifest.jsonl.tmp.%ld",
             options->output_path, (long)getpid());
    needed = snprintf(NULL, 0, "%s/manifest.jsonl", options->output_path);
    char *destination = xrealloc(NULL, (size_t)needed + 1);
    snprintf(destination, (size_t)needed + 1, "%s/manifest.jsonl",
             options->output_path);

    FILE *stream = fopen(temporary, "wb");
    if (stream == NULL) {
        free(temporary);
        free(destination);
        return -1;
    }
    for (size_t i = 0; i < entries->length; ++i) {
        char *path = record_path(options, i, entries->items[i].word);
        const char *relative = path + strlen(options->output_path) + 1;
        uint64_t hash = hash_word(entries->items[i].word);
        size_t partition = (size_t)(hash % options->partitions);
        fputs("{\"ordinal\":", stream);
        fprintf(stream, "%zu,\"partition\":%zu,\"word\":", i, partition);
        json_string(stream, entries->items[i].word);
        fputs(",\"url\":", stream);
        json_string(stream, entries->items[i].url);
        fputs(",\"file\":", stream);
        json_string(stream, relative);
        fputs("}\n", stream);
        free(path);
    }
    if (fclose(stream) != 0 || rename(temporary, destination) != 0) {
        int saved = errno;
        unlink(temporary);
        free(temporary);
        free(destination);
        errno = saved;
        return -1;
    }
    free(temporary);
    free(destination);
    return 0;
}

static int write_metadata(const struct options *options, size_t records)
{
    int needed = snprintf(NULL, 0, "%s/index.json", options->output_path);
    char *path = xrealloc(NULL, (size_t)needed + 1);
    snprintf(path, (size_t)needed + 1, "%s/index.json", options->output_path);
    FILE *stream = fopen(path, "wb");
    if (stream == NULL) {
        free(path);
        return -1;
    }
    fputs("{\n  \"schema_version\": 1,\n  \"source_map\": ", stream);
    json_string(stream, options->map_path);
    fprintf(stream,
            ",\n  \"records\": %zu,\n  \"partitions\": %zu,\n"
            "  \"manifest\": \"manifest.jsonl\"\n}\n",
            records, options->partitions);
    int result = fclose(stream);
    free(path);
    return result == 0 ? 0 : -1;
}

static void free_entries(struct entry_list *entries)
{
    for (size_t i = 0; i < entries->length; ++i) {
        free(entries->items[i].word);
        free(entries->items[i].url);
    }
    free(entries->items);
}

int main(int argc, char **argv)
{
    struct options options;
    parse_options(argc, argv, &options);

    char *json = read_file(options.map_path);
    if (json == NULL) {
        die_errno("cannot read map", options.map_path);
    }
    struct entry_list entries = {0};
    if (!parse_map(json, &entries)) {
        log_message("ERROR", SIZE_MAX, "invalid headword map path=\"%s\"",
                    options.map_path);
        free(json);
        free_entries(&entries);
        return EXIT_FAILURE;
    }
    free(json);
    if (setenv("ETYMWB_HEADWORDS", options.map_path, 1) != 0) {
        die_errno("cannot configure extractor map", options.map_path);
    }
    if (options.limit < entries.length) {
        for (size_t i = options.limit; i < entries.length; ++i) {
            free(entries.items[i].word);
            free(entries.items[i].url);
        }
        entries.length = options.limit;
    }
    if (mkdir_p(options.output_path) != 0) {
        die_errno("cannot create output directory", options.output_path);
    }
    if (write_manifest(&options, &entries) != 0 ||
        write_metadata(&options, entries.length) != 0) {
        die_errno("cannot write index metadata", options.output_path);
    }

    size_t worker_count = options.workers;
    if (entries.length > 0 && worker_count > entries.length) {
        worker_count = entries.length;
    }
    if (entries.length == 0) {
        worker_count = 0;
    }
    log_message("INFO", SIZE_MAX,
                "event=start entries=%zu workers=%zu partitions=%zu output=\"%s\" force=%s",
                entries.length, worker_count, options.partitions,
                options.output_path, options.force ? "true" : "false");

    struct pool pool = {
        .options = &options,
        .entries = &entries,
        .lock = PTHREAD_MUTEX_INITIALIZER,
    };
    pthread_t *threads = xrealloc(NULL, worker_count * sizeof *threads);
    struct worker *workers = xrealloc(NULL, worker_count * sizeof *workers);
    size_t started = 0;
    for (size_t i = 0; i < worker_count; ++i) {
        workers[i] = (struct worker){.pool = &pool, .id = i};
        int result = pthread_create(&threads[i], NULL, worker_main, &workers[i]);
        if (result != 0) {
            log_message("ERROR", SIZE_MAX,
                        "cannot create worker=%zu error=\"%s\"", i,
                        strerror(result));
            break;
        }
        ++started;
    }
    for (size_t i = 0; i < started; ++i) {
        pthread_join(threads[i], NULL);
    }
    if (started != worker_count) {
        pool.failed += entries.length - pool.completed;
    }

    log_message(pool.failed == 0 ? "INFO" : "ERROR", SIZE_MAX,
                "event=complete total=%zu downloaded=%zu skipped=%zu unavailable=%zu failed=%zu",
                entries.length, pool.downloaded, pool.skipped, pool.unavailable,
                pool.failed);
    bool failed = pool.failed != 0;
    pthread_mutex_destroy(&pool.lock);
    free(threads);
    free(workers);
    free_entries(&entries);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
