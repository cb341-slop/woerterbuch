#define _POSIX_C_SOURCE 200809L

#include <curl/curl.h>
#include <libxml/HTMLparser.h>
#include <libxml/HTMLtree.h>
#include <libxml/uri.h>
#include <libxml/xpath.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USER_AGENT "extract_word/1.0"
#define INITIAL_BUFFER_CAPACITY 4096U

struct buffer {
    char *data;
    size_t length;
    size_t capacity;
};

struct string_list {
    char **items;
    size_t length;
    size_t capacity;
};

struct headword {
    char *word;
    char *grammar_label;
    char *grammar_description;
    const char *part_of_speech;
    const char *gender;
    char *text;
};

struct headword_list {
    struct headword *items;
    size_t length;
    size_t capacity;
};

struct link {
    char *text;
    char *url;
};

struct link_list {
    struct link *items;
    size_t length;
    size_t capacity;
};

struct source_info {
    char *collection;
    char *attribution;
    char *canonical_url;
    char *citation;
    char *tdm_reservation;
    char *tdm_policy_url;
};

static void die(const char *message)
{
    fprintf(stderr, "extract_word: %s\n", message);
    exit(EXIT_FAILURE);
}

static void *xrealloc(void *pointer, size_t size)
{
    void *result = realloc(pointer, size);
    if (result == NULL && size != 0) {
        die("out of memory");
    }
    return result;
}

static char *xstrdup(const char *text)
{
    char *copy = strdup(text);
    if (copy == NULL) {
        die("out of memory");
    }
    return copy;
}

static void buffer_init(struct buffer *buffer)
{
    buffer->data = xrealloc(NULL, INITIAL_BUFFER_CAPACITY);
    buffer->data[0] = '\0';
    buffer->length = 0;
    buffer->capacity = INITIAL_BUFFER_CAPACITY;
}

static void buffer_reserve(struct buffer *buffer, size_t extra)
{
    if (extra > SIZE_MAX - buffer->length - 1) {
        die("input is too large");
    }
    size_t needed = buffer->length + extra + 1;
    if (needed <= buffer->capacity) {
        return;
    }
    size_t capacity = buffer->capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    buffer->data = xrealloc(buffer->data, capacity);
    buffer->capacity = capacity;
}

static void buffer_append_n(struct buffer *buffer, const char *text, size_t length)
{
    buffer_reserve(buffer, length);
    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
}

static void buffer_append_byte(struct buffer *buffer, unsigned char byte)
{
    char value = (char)byte;
    buffer_append_n(buffer, &value, 1);
}

static char *buffer_take(struct buffer *buffer)
{
    char *data = buffer->data;
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
    return data;
}

static char *read_stream(FILE *stream)
{
    struct buffer contents;
    char block[8192];
    size_t count;
    buffer_init(&contents);
    while ((count = fread(block, 1, sizeof block, stream)) > 0) {
        buffer_append_n(&contents, block, count);
    }
    if (ferror(stream)) {
        free(contents.data);
        return NULL;
    }
    return buffer_take(&contents);
}

static char *read_file(const char *path)
{
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) {
        return NULL;
    }
    char *contents = read_stream(stream);
    int saved_errno = errno;
    fclose(stream);
    errno = saved_errno;
    return contents;
}

static bool hex_digit(char value, unsigned *result)
{
    if (value >= '0' && value <= '9') {
        *result = (unsigned)(value - '0');
        return true;
    }
    if (value >= 'a' && value <= 'f') {
        *result = (unsigned)(value - 'a') + 10U;
        return true;
    }
    if (value >= 'A' && value <= 'F') {
        *result = (unsigned)(value - 'A') + 10U;
        return true;
    }
    return false;
}

static void append_utf8(struct buffer *out, uint32_t codepoint)
{
    if (codepoint <= 0x7fU) {
        buffer_append_byte(out, (unsigned char)codepoint);
    } else if (codepoint <= 0x7ffU) {
        buffer_append_byte(out, (unsigned char)(0xc0U | (codepoint >> 6)));
        buffer_append_byte(out, (unsigned char)(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0xffffU) {
        buffer_append_byte(out, (unsigned char)(0xe0U | (codepoint >> 12)));
        buffer_append_byte(out, (unsigned char)(0x80U | ((codepoint >> 6) & 0x3fU)));
        buffer_append_byte(out, (unsigned char)(0x80U | (codepoint & 0x3fU)));
    } else {
        buffer_append_byte(out, (unsigned char)(0xf0U | (codepoint >> 18)));
        buffer_append_byte(out, (unsigned char)(0x80U | ((codepoint >> 12) & 0x3fU)));
        buffer_append_byte(out, (unsigned char)(0x80U | ((codepoint >> 6) & 0x3fU)));
        buffer_append_byte(out, (unsigned char)(0x80U | (codepoint & 0x3fU)));
    }
}

static bool parse_hex4(const char **cursor, uint32_t *value)
{
    uint32_t result = 0;
    for (int i = 0; i < 4; ++i) {
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
            buffer_append_byte(&result, byte);
            continue;
        }
        char escape = *(*cursor)++;
        switch (escape) {
        case '"': buffer_append_byte(&result, '"'); break;
        case '\\': buffer_append_byte(&result, '\\'); break;
        case '/': buffer_append_byte(&result, '/'); break;
        case 'b': buffer_append_byte(&result, '\b'); break;
        case 'f': buffer_append_byte(&result, '\f'); break;
        case 'n': buffer_append_byte(&result, '\n'); break;
        case 'r': buffer_append_byte(&result, '\r'); break;
        case 't': buffer_append_byte(&result, '\t'); break;
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
                if (!parse_hex4(cursor, &second) || second < 0xdc00U || second > 0xdfffU) {
                    free(result.data);
                    return NULL;
                }
                codepoint = 0x10000U + ((first - 0xd800U) << 10) + (second - 0xdc00U);
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

static char *lookup_url(const char *json, const char *word)
{
    const char *cursor = json;
    skip_space(&cursor);
    if (*cursor++ != '{') {
        return NULL;
    }
    for (;;) {
        skip_space(&cursor);
        if (*cursor == '}') {
            return NULL;
        }
        char *key = parse_json_string(&cursor);
        if (key == NULL) {
            return NULL;
        }
        skip_space(&cursor);
        if (*cursor++ != ':') {
            free(key);
            return NULL;
        }
        skip_space(&cursor);
        char *value = parse_json_string(&cursor);
        if (value == NULL) {
            free(key);
            return NULL;
        }
        bool match = strcmp(key, word) == 0;
        free(key);
        if (match) {
            return value;
        }
        free(value);
        skip_space(&cursor);
        if (*cursor == ',') {
            ++cursor;
            continue;
        }
        if (*cursor == '}') {
            return NULL;
        }
        return NULL;
    }
}

static const char *find_map_path(void)
{
    const char *configured = getenv("ETYMWB_HEADWORDS");
    if (configured != NULL && configured[0] != '\0') {
        return configured;
    }
    static const char *candidates[] = {
        "etymwb-headwords.json",
        "../etymwb-headwords.json",
        NULL
    };
    for (size_t i = 0; candidates[i] != NULL; ++i) {
        FILE *stream = fopen(candidates[i], "rb");
        if (stream != NULL) {
            fclose(stream);
            return candidates[i];
        }
    }
    return NULL;
}

static size_t curl_write(void *data, size_t size, size_t count, void *context)
{
    if (size != 0 && count > SIZE_MAX / size) {
        return 0;
    }
    size_t length = size * count;
    struct buffer *body = context;
    buffer_append_n(body, data, length);
    return length;
}

static char *fetch_url(const char *url)
{
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        die("could not initialize libcurl");
    }
    struct buffer body;
    buffer_init(&body);
    char error[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK) {
        fprintf(stderr, "extract_word: request failed for %s: %s\n", url,
                error[0] != '\0' ? error : curl_easy_strerror(result));
        free(body.data);
        return NULL;
    }
    if (status < 200 || status >= 300) {
        fprintf(stderr, "extract_word: URL returned HTTP %ld: %s\n", status, url);
        free(body.data);
        return NULL;
    }
    return buffer_take(&body);
}

static bool node_has_class(xmlNodePtr node, const char *wanted)
{
    xmlChar *classes = xmlGetProp(node, BAD_CAST "class");
    if (classes == NULL) {
        return false;
    }
    bool found = false;
    const char *cursor = (const char *)classes;
    size_t wanted_length = strlen(wanted);
    while (*cursor != '\0') {
        while (isspace((unsigned char)*cursor)) {
            ++cursor;
        }
        const char *start = cursor;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
            ++cursor;
        }
        if ((size_t)(cursor - start) == wanted_length &&
            memcmp(start, wanted, wanted_length) == 0) {
            found = true;
            break;
        }
    }
    xmlFree(classes);
    return found;
}

static char *normalize_text(const xmlChar *input)
{
    struct buffer result;
    buffer_init(&result);
    bool pending_space = false;
    for (const unsigned char *cursor = input; *cursor != '\0'; ++cursor) {
        if (isspace(*cursor)) {
            pending_space = result.length > 0;
            continue;
        }
        if (pending_space) {
            buffer_append_byte(&result, ' ');
            pending_space = false;
        }
        buffer_append_byte(&result, *cursor);
    }
    return buffer_take(&result);
}

static char *node_text(xmlNodePtr node)
{
    xmlChar *raw = xmlNodeGetContent(node);
    if (raw == NULL) {
        return xstrdup("");
    }
    char *result = normalize_text(raw);
    xmlFree(raw);
    return result;
}

static xmlNodePtr first_descendant_with_class(xmlNodePtr node, const char *class_name)
{
    for (xmlNodePtr current = node; current != NULL; current = current->next) {
        if (current->type == XML_ELEMENT_NODE && node_has_class(current, class_name)) {
            return current;
        }
        xmlNodePtr nested = first_descendant_with_class(current->children, class_name);
        if (nested != NULL) {
            return nested;
        }
    }
    return NULL;
}

static void string_list_add_unique(struct string_list *list, char *value)
{
    if (value[0] == '\0') {
        free(value);
        return;
    }
    for (size_t i = 0; i < list->length; ++i) {
        if (strcmp(list->items[i], value) == 0) {
            free(value);
            return;
        }
    }
    if (list->length == list->capacity) {
        list->capacity = list->capacity == 0 ? 16 : list->capacity * 2;
        list->items = xrealloc(list->items, list->capacity * sizeof *list->items);
    }
    list->items[list->length++] = value;
}

static const char *grammar_part_of_speech(const char *label)
{
    if (strcmp(label, "m.") == 0 || strcmp(label, "f.") == 0 || strcmp(label, "n.") == 0) {
        return "noun";
    }
    if (strcmp(label, "Vb.") == 0) return "verb";
    if (strcmp(label, "Adj.") == 0) return "adjective";
    if (strcmp(label, "Adv.") == 0) return "adverb";
    if (strcmp(label, "Präp.") == 0) return "preposition";
    if (strcmp(label, "Num. Ord.") == 0) return "ordinal_numeral";
    if (strcmp(label, "Part.adj.") == 0) return "participial_adjective";
    return "unknown";
}

static const char *grammar_gender(const char *label)
{
    if (strcmp(label, "m.") == 0) return "masculine";
    if (strcmp(label, "f.") == 0) return "feminine";
    if (strcmp(label, "n.") == 0) return "neuter";
    return NULL;
}

static xmlNodePtr first_descendant_named(xmlNodePtr node, const char *name)
{
    for (xmlNodePtr current = node; current != NULL; current = current->next) {
        if (current->type == XML_ELEMENT_NODE &&
            xmlStrEqual(current->name, BAD_CAST name)) {
            return current;
        }
        xmlNodePtr nested = first_descendant_named(current->children, name);
        if (nested != NULL) return nested;
    }
    return NULL;
}

static char *node_property(xmlNodePtr node, const char *name)
{
    if (node == NULL) return xstrdup("");
    xmlChar *value = xmlGetProp(node, BAD_CAST name);
    if (value == NULL) return xstrdup("");
    char *result = xstrdup((const char *)value);
    xmlFree(value);
    return result;
}

static void headword_list_add(struct headword_list *list, char *word,
                              char *grammar_label, char *grammar_description)
{
    if (word[0] == '\0') {
        free(word);
        free(grammar_label);
        free(grammar_description);
        return;
    }
    if (list->length == list->capacity) {
        list->capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        list->items = xrealloc(list->items, list->capacity * sizeof *list->items);
    }
    list->items[list->length++] = (struct headword){
        .word = word,
        .grammar_label = grammar_label,
        .grammar_description = grammar_description,
        .part_of_speech = grammar_part_of_speech(grammar_label),
        .gender = grammar_gender(grammar_label),
        .text = NULL
    };
}

static void link_list_add_unique(struct link_list *list, char *text, char *url)
{
    if (text[0] == '\0' || url[0] == '\0') {
        free(text);
        free(url);
        return;
    }
    for (size_t i = 0; i < list->length; ++i) {
        if (strcmp(list->items[i].text, text) == 0 &&
            strcmp(list->items[i].url, url) == 0) {
            free(text);
            free(url);
            return;
        }
    }
    if (list->length == list->capacity) {
        list->capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        list->items = xrealloc(list->items, list->capacity * sizeof *list->items);
    }
    list->items[list->length++] = (struct link){text, url};
}

static char *resolve_url(const char *base, const xmlChar *href)
{
    xmlChar *resolved = xmlBuildURI(href, BAD_CAST base);
    if (resolved == NULL) {
        return xstrdup((const char *)href);
    }
    char *result = xstrdup((const char *)resolved);
    xmlFree(resolved);
    return result;
}

static xmlNodePtr first_element_with_attribute(xmlNodePtr node, const char *element,
                                                const char *attribute,
                                                const char *value)
{
    for (xmlNodePtr current = node; current != NULL; current = current->next) {
        if (current->type == XML_ELEMENT_NODE &&
            (element == NULL || xmlStrEqual(current->name, BAD_CAST element))) {
            xmlChar *property = xmlGetProp(current, BAD_CAST attribute);
            bool match = property != NULL && strcmp((const char *)property, value) == 0;
            xmlFree(property);
            if (match) return current;
        }
        xmlNodePtr nested = first_element_with_attribute(current->children, element,
                                                          attribute, value);
        if (nested != NULL) return nested;
    }
    return NULL;
}

static char *meta_content(xmlNodePtr root, const char *attribute, const char *value)
{
    xmlNodePtr meta = first_element_with_attribute(root, "meta", attribute, value);
    return node_property(meta, "content");
}

static char *trimmed_slice(const char *start, const char *end)
{
    while (start < end && isspace((unsigned char)*start)) ++start;
    while (end > start && isspace((unsigned char)end[-1])) --end;
    if ((size_t)(end - start) >= 3 &&
        memcmp(end - 3, "\xe2\x80\x93", 3) == 0) {
        end -= 3;
        while (end > start && isspace((unsigned char)end[-1])) --end;
    }
    size_t length = (size_t)(end - start);
    char *result = xrealloc(NULL, length + 1);
    memcpy(result, start, length);
    result[length] = '\0';
    return result;
}

static void finish_headword_text(struct buffer *section, struct headword *headword)
{
    char *normalized = normalize_text(BAD_CAST section->data);
    headword->text = trimmed_slice(normalized, normalized + strlen(normalized));
    free(normalized);
    free(section->data);
}

static void assign_headword_texts(xmlNodePtr entry, struct headword_list *headwords)
{
    struct buffer section = {0};
    size_t headword_index = 0;
    bool active = false;
    for (xmlNodePtr current = entry->children; current != NULL; current = current->next) {
        if (current->type == XML_ELEMENT_NODE &&
            node_has_class(current, "etymwb-headword")) {
            if (active && headword_index > 0) {
                finish_headword_text(&section, &headwords->items[headword_index - 1]);
            }
            if (headword_index >= headwords->length) return;
            buffer_init(&section);
            active = true;
            ++headword_index;
        }
        if (active) {
            xmlChar *raw = xmlNodeGetContent(current);
            if (raw != NULL) {
                buffer_append_n(&section, (const char *)raw, strlen((const char *)raw));
                xmlFree(raw);
            }
        }
    }
    if (active && headword_index > 0) {
        finish_headword_text(&section, &headwords->items[headword_index - 1]);
    }
}

static void collect_entry_data(xmlNodePtr node, const char *base_url,
                               struct headword_list *headwords,
                               struct string_list *mentions,
                               struct link_list *links)
{
    for (xmlNodePtr current = node; current != NULL; current = current->next) {
        if (current->type != XML_ELEMENT_NODE) {
            collect_entry_data(current->children, base_url, headwords, mentions, links);
            continue;
        }
        if (node_has_class(current, "etymwb-headword")) {
            xmlNodePtr orth = first_descendant_with_class(current->children, "etymwb-orth");
            xmlNodePtr grammar_node = first_descendant_with_class(current->children, "etymwb-gramgrp");
            if (orth != NULL) {
                char *word = node_text(orth);
                char *grammar = grammar_node != NULL ? node_text(grammar_node) : xstrdup("");
                xmlNodePtr abbreviation = grammar_node != NULL
                    ? first_descendant_named(grammar_node->children, "abbr") : NULL;
                char *description = node_property(abbreviation, "title");
                headword_list_add(headwords, word, grammar, description);
            }
        }
        if (node_has_class(current, "etymwb-mentioned")) {
            string_list_add_unique(mentions, node_text(current));
        }
        if (xmlStrEqual(current->name, BAD_CAST "a")) {
            xmlChar *href = xmlGetProp(current, BAD_CAST "href");
            if (href != NULL) {
                char *text = node_text(current);
                char *url = resolve_url(base_url, href);
                link_list_add_unique(links, text, url);
                xmlFree(href);
            }
        }
        collect_entry_data(current->children, base_url, headwords, mentions, links);
    }
}

static void print_json_string(const char *text)
{
    putchar('"');
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor != '\0'; ++cursor) {
        switch (*cursor) {
        case '"': fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\b': fputs("\\b", stdout); break;
        case '\f': fputs("\\f", stdout); break;
        case '\n': fputs("\\n", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        default:
            if (*cursor < 0x20U) {
                printf("\\u%04x", (unsigned)*cursor);
            } else {
                putchar((int)*cursor);
            }
        }
    }
    putchar('"');
}

static void print_schema(void)
{
    fputs(
        "{\n"
        "  \"$schema\": \"https://json-schema.org/draft/2020-12/schema\",\n"
        "  \"title\": \"DWDS etymology lookup record\",\n"
        "  \"description\": \"A query-aware, sectioned etymology record emitted by extract_word.\",\n"
        "  \"type\": \"object\",\n"
        "  \"additionalProperties\": false,\n"
        "  \"required\": [\"schema_version\", \"query\", \"source\", \"article\", \"headwords\", \"mentioned_forms\", \"links\"],\n"
        "  \"properties\": {\n"
        "    \"schema_version\": {\"const\": 2},\n"
        "    \"query\": {\n"
        "      \"type\": \"object\", \"additionalProperties\": false,\n"
        "      \"required\": [\"word\", \"requested_url\", \"match_type\", \"matched_headword_index\"],\n"
        "      \"properties\": {\n"
        "        \"word\": {\"type\": \"string\", \"minLength\": 1},\n"
        "        \"requested_url\": {\"type\": \"string\", \"format\": \"uri\"},\n"
        "        \"match_type\": {\"enum\": [\"exact_headword\", \"article_redirect\"]},\n"
        "        \"matched_headword_index\": {\"type\": [\"integer\", \"null\"], \"minimum\": 0}\n"
        "      }\n"
        "    },\n"
        "    \"source\": {\n"
        "      \"type\": \"object\", \"additionalProperties\": false,\n"
        "      \"required\": [\"provider\", \"collection\", \"canonical_url\"],\n"
        "      \"properties\": {\n"
        "        \"provider\": {\"const\": \"DWDS\"},\n"
        "        \"collection\": {\"type\": \"string\", \"minLength\": 1},\n"
        "        \"attribution\": {\"type\": \"string\", \"minLength\": 1},\n"
        "        \"canonical_url\": {\"type\": \"string\", \"format\": \"uri\"},\n"
        "        \"citation\": {\"type\": \"string\", \"minLength\": 1},\n"
        "        \"tdm\": {\n"
        "          \"type\": \"object\", \"additionalProperties\": false,\n"
        "          \"required\": [\"reservation\", \"policy_url\"],\n"
        "          \"properties\": {\n"
        "            \"reservation\": {\"type\": \"string\", \"minLength\": 1},\n"
        "            \"policy_url\": {\"type\": \"string\", \"format\": \"uri\"}\n"
        "          }\n"
        "        }\n"
        "      }\n"
        "    },\n"
        "    \"article\": {\n"
        "      \"type\": \"object\", \"additionalProperties\": false,\n"
        "      \"required\": [\"title\", \"text\"],\n"
        "      \"properties\": {\n"
        "        \"title\": {\"type\": \"string\", \"minLength\": 1},\n"
        "        \"text\": {\"type\": \"string\", \"minLength\": 1}\n"
        "      }\n"
        "    },\n"
        "    \"headwords\": {\n"
        "      \"type\": \"array\",\n"
        "      \"minItems\": 1,\n"
        "      \"items\": {\n"
        "        \"type\": \"object\",\n"
        "        \"additionalProperties\": false,\n"
        "        \"required\": [\"word\", \"grammar\", \"text\"],\n"
        "        \"properties\": {\n"
        "          \"word\": {\"type\": \"string\", \"minLength\": 1},\n"
        "          \"grammar\": {\n"
        "            \"type\": \"object\", \"additionalProperties\": false,\n"
        "            \"required\": [\"label\", \"part_of_speech\"],\n"
        "            \"properties\": {\n"
        "              \"label\": {\"type\": \"string\"},\n"
        "              \"description\": {\"type\": \"string\", \"minLength\": 1},\n"
        "              \"part_of_speech\": {\"enum\": [\"noun\", \"verb\", \"adjective\", \"adverb\", \"preposition\", \"ordinal_numeral\", \"participial_adjective\", \"unknown\"]},\n"
        "              \"gender\": {\"enum\": [\"masculine\", \"feminine\", \"neuter\"]}\n"
        "            }\n"
        "          },\n"
        "          \"text\": {\"type\": \"string\", \"minLength\": 1}\n"
        "        }\n"
        "      }\n"
        "    },\n"
        "    \"mentioned_forms\": {\n"
        "      \"type\": \"array\",\n"
        "      \"uniqueItems\": true,\n"
        "      \"items\": {\"type\": \"string\", \"minLength\": 1}\n"
        "    },\n"
        "    \"links\": {\n"
        "      \"type\": \"array\",\n"
        "      \"uniqueItems\": true,\n"
        "      \"items\": {\n"
        "        \"type\": \"object\",\n"
        "        \"additionalProperties\": false,\n"
        "        \"required\": [\"text\", \"url\", \"relation\"],\n"
        "        \"properties\": {\n"
        "          \"text\": {\"type\": \"string\", \"minLength\": 1},\n"
        "          \"url\": {\"type\": \"string\", \"format\": \"uri\"},\n"
        "          \"relation\": {\"const\": \"cross_reference\"}\n"
        "        }\n"
        "      }\n"
        "    }\n"
        "  }\n"
        "}\n",
        stdout);
}

static void print_record(const char *word, const char *url, const char *title,
                         const char *entry_text, const struct source_info *source,
                         const struct headword_list *headwords,
                         const struct string_list *mentions, const struct link_list *links)
{
    size_t matched_index = 0;
    bool matched = false;
    for (size_t i = 0; i < headwords->length; ++i) {
        if (strcmp(word, headwords->items[i].word) == 0) {
            matched_index = i;
            matched = true;
            break;
        }
    }

    fputs("{\n  \"schema_version\": 2,\n  \"query\": {\n    \"word\": ", stdout);
    print_json_string(word);
    fputs(",\n    \"requested_url\": ", stdout);
    print_json_string(url);
    fputs(",\n    \"match_type\": ", stdout);
    print_json_string(matched ? "exact_headword" : "article_redirect");
    fputs(",\n    \"matched_headword_index\": ", stdout);
    if (matched) printf("%zu", matched_index); else fputs("null", stdout);
    fputs("\n  },\n  \"source\": {\n    \"provider\": \"DWDS\",\n    \"collection\": ", stdout);
    print_json_string(source->collection);
    if (source->attribution[0] != '\0') {
        fputs(",\n    \"attribution\": ", stdout);
        print_json_string(source->attribution);
    }
    fputs(",\n    \"canonical_url\": ", stdout);
    print_json_string(source->canonical_url);
    if (source->citation[0] != '\0') {
        fputs(",\n    \"citation\": ", stdout);
        print_json_string(source->citation);
    }
    if (source->tdm_reservation[0] != '\0' && source->tdm_policy_url[0] != '\0') {
        fputs(",\n    \"tdm\": {\"reservation\": ", stdout);
        print_json_string(source->tdm_reservation);
        fputs(", \"policy_url\": ", stdout);
        print_json_string(source->tdm_policy_url);
        putchar('}');
    }
    fputs("\n  },\n  \"article\": {\n    \"title\": ", stdout);
    print_json_string(title);
    fputs(",\n    \"text\": ", stdout);
    print_json_string(entry_text);
    fputs("\n  },\n  \"headwords\": [", stdout);
    for (size_t i = 0; i < headwords->length; ++i) {
        fputs(i == 0 ? "\n    {\"word\": " : ",\n    {\"word\": ", stdout);
        print_json_string(headwords->items[i].word);
        fputs(", \"grammar\": {\"label\": ", stdout);
        print_json_string(headwords->items[i].grammar_label);
        if (headwords->items[i].grammar_description[0] != '\0') {
            fputs(", \"description\": ", stdout);
            print_json_string(headwords->items[i].grammar_description);
        }
        fputs(", \"part_of_speech\": ", stdout);
        print_json_string(headwords->items[i].part_of_speech);
        if (headwords->items[i].gender != NULL) {
            fputs(", \"gender\": ", stdout);
            print_json_string(headwords->items[i].gender);
        }
        fputs("}, \"text\": ", stdout);
        print_json_string(headwords->items[i].text);
        putchar('}');
    }
    fputs(headwords->length == 0 ? "]," : "\n  ],", stdout);
    fputs("\n  \"mentioned_forms\": [", stdout);
    for (size_t i = 0; i < mentions->length; ++i) {
        fputs(i == 0 ? "\n    " : ",\n    ", stdout);
        print_json_string(mentions->items[i]);
    }
    fputs(mentions->length == 0 ? "]," : "\n  ],", stdout);
    fputs("\n  \"links\": [", stdout);
    for (size_t i = 0; i < links->length; ++i) {
        fputs(i == 0 ? "\n    {\"text\": " : ",\n    {\"text\": ", stdout);
        print_json_string(links->items[i].text);
        fputs(", \"url\": ", stdout);
        print_json_string(links->items[i].url);
        fputs(", \"relation\": \"cross_reference\"", stdout);
        putchar('}');
    }
    fputs(links->length == 0 ? "]\n}\n" : "\n  ]\n}\n", stdout);
}

static void free_lists(struct headword_list *headwords, struct string_list *mentions,
                       struct link_list *links)
{
    for (size_t i = 0; i < headwords->length; ++i) {
        free(headwords->items[i].word);
        free(headwords->items[i].grammar_label);
        free(headwords->items[i].grammar_description);
        free(headwords->items[i].text);
    }
    free(headwords->items);
    for (size_t i = 0; i < mentions->length; ++i) {
        free(mentions->items[i]);
    }
    free(mentions->items);
    for (size_t i = 0; i < links->length; ++i) {
        free(links->items[i].text);
        free(links->items[i].url);
    }
    free(links->items);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--schema") == 0) {
        print_schema();
        return EXIT_SUCCESS;
    }
    if (argc != 2 || strcmp(argv[1], "--help") == 0) {
        fprintf(argc == 2 ? stdout : stderr,
                "usage: %s <word>\n"
                "       %s --schema\n",
                argc > 0 ? argv[0] : "extract_word",
                argc > 0 ? argv[0] : "extract_word");
        return argc == 2 ? EXIT_SUCCESS : 2;
    }
    const char *map_path = find_map_path();
    if (map_path == NULL) {
        die("cannot find etymwb-headwords.json; set ETYMWB_HEADWORDS");
    }
    char *map = read_file(map_path);
    if (map == NULL) {
        fprintf(stderr, "extract_word: cannot read %s: %s\n", map_path, strerror(errno));
        return EXIT_FAILURE;
    }
    char *url = lookup_url(map, argv[1]);
    free(map);
    if (url == NULL) {
        fprintf(stderr, "extract_word: word not found in map: %s\n", argv[1]);
        return 3;
    }

    char *html;
    const char *fixture = getenv("EXTRACT_WORD_HTML_FILE");
    if (fixture != NULL && fixture[0] != '\0') {
        html = read_file(fixture);
        if (html == NULL) {
            fprintf(stderr, "extract_word: cannot read %s: %s\n", fixture, strerror(errno));
            free(url);
            return EXIT_FAILURE;
        }
    } else {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            free(url);
            die("could not initialize libcurl globally");
        }
        html = fetch_url(url);
        curl_global_cleanup();
        if (html == NULL) {
            free(url);
            return 4;
        }
    }

    size_t html_length = strlen(html);
    if (html_length > INT_MAX) {
        fprintf(stderr, "extract_word: HTML response is too large: %s\n", url);
        free(html);
        free(url);
        return 5;
    }
    htmlDocPtr document = htmlReadMemory(html, (int)html_length, url, "UTF-8",
                                         HTML_PARSE_RECOVER | HTML_PARSE_NOERROR |
                                         HTML_PARSE_NOWARNING | HTML_PARSE_NONET);
    free(html);
    if (document == NULL) {
        fprintf(stderr, "extract_word: could not parse HTML from %s\n", url);
        free(url);
        return 5;
    }
    xmlNodePtr root = xmlDocGetRootElement(document);
    xmlNodePtr entry = first_descendant_with_class(root, "etymwb-entry");
    if (entry == NULL) {
        fprintf(stderr, "extract_word: page has no .etymwb-entry: %s\n", url);
        xmlFreeDoc(document);
        free(url);
        return 6;
    }

    struct headword_list headwords = {0};
    struct string_list mentions = {0};
    struct link_list links = {0};
    collect_entry_data(entry->children, url, &headwords, &mentions, &links);
    char *entry_text = node_text(entry);
    assign_headword_texts(entry, &headwords);

    xmlNodePtr title_node = first_descendant_with_class(root, "dwdswb-ft-lemmaansatz");
    char *title = title_node != NULL ? node_text(title_node)
        : (headwords.length > 0 ? xstrdup(headwords.items[0].word) : xstrdup(argv[1]));

    xmlNodePtr source_node = first_descendant_with_class(root, "ext-quelle");
    xmlNodePtr collection_link = source_node != NULL
        ? first_descendant_named(source_node->children, "a") : NULL;
    xmlNodePtr citation_wrapper = first_descendant_with_class(root, "citation-help");
    xmlNodePtr citation_node = citation_wrapper != NULL
        ? first_descendant_named(citation_wrapper->children, "cite") : NULL;
    struct source_info source = {
        .collection = collection_link != NULL ? node_text(collection_link)
            : xstrdup("Etymologisches Wörterbuch des Deutschen"),
        .attribution = source_node != NULL ? node_text(source_node) : xstrdup(""),
        .canonical_url = meta_content(root, "property", "og:url"),
        .citation = citation_node != NULL ? node_text(citation_node) : xstrdup(""),
        .tdm_reservation = meta_content(root, "name", "tdm-reservation"),
        .tdm_policy_url = meta_content(root, "name", "tdm-policy")
    };
    if (source.canonical_url[0] == '\0') {
        free(source.canonical_url);
        source.canonical_url = xstrdup(url);
    }

    print_record(argv[1], url, title, entry_text, &source,
                 &headwords, &mentions, &links);

    free(source.collection);
    free(source.attribution);
    free(source.canonical_url);
    free(source.citation);
    free(source.tdm_reservation);
    free(source.tdm_policy_url);
    free(title);
    free(entry_text);
    free_lists(&headwords, &mentions, &links);
    xmlFreeDoc(document);
    xmlCleanupParser();
    free(url);
    return EXIT_SUCCESS;
}
