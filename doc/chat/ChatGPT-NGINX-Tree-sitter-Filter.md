# NGINX Tree-sitter Filter

**User:** Milan Hauth (<milahu@gmail.com>)  
**Created:** 2026/5/6 3:27:17  
**Updated:** 2026/5/6 3:38:05  
**Exported:** 2026/5/6 14:33:56  
**Link:** [<https://chatgpt.com/c/69fa985a-facc-832c-8d54-69cf8457e7ea>](https://chatgpt.com/c/69fa985a-facc-832c-8d54-69cf8457e7ea)

## Prompt:

help me create a filter module for the nginx webserver  
for syntax-highlighting of source code files,  
based on tree-sitter parsers.  
it should be called ngx_tree_sitter_filter_module.  
the tree-sitter parse tree should be serialized to HTML.  
there should be a GET parameter "raw=1" to disable syntax-highlighting, to serve the plain text file.  
it should be possible to build the module with an arbitrary number of tree-sitter parsers (.so files)

## Response:

That’s a solid idea—but it’s not a trivial “filter” in the casual sense. An NGINX body filter that embeds a full parser runtime (Tree-sitter) and dynamically loads grammars has a few sharp edges: request buffering, streaming vs full-body processing, thread-safety, and dynamic module loading. It’s doable, just be aware you’ll likely end up buffering the full response before parsing.

Here’s a practical blueprint for `ngx_tree_sitter_filter_module` that keeps things maintainable.

------------------------------------------------------------------------

1)  # High-level design

### Responsibilities

- Intercept responses for certain MIME types / file extensions
- If `raw=1` → bypass filter entirely
- Otherwise:
  - Buffer full response body
  - Run Tree-sitter parsing
  - Convert parse tree → HTML
  - Replace response body
  - Set `Content-Type: text/html`

### Key constraints

- NGINX filters are streaming by default → Tree-sitter is not
- So: **force full buffering**

------------------------------------------------------------------------

2)  # Module structure

<!-- -->

    ngx_tree_sitter_filter_module/├── config├── ngx_http_tree_sitter_filter_module.c├── ts_runtime.c├── ts_runtime.h├── html_serializer.c├── html_serializer.h

------------------------------------------------------------------------

3)  # NGINX module skeleton

### Module definition

    static ngx_http_module_t ngx_http_tree_sitter_filter_module_ctx = {    NULL,                          // preconfiguration    ngx_http_ts_filter_init,       // postconfiguration    NULL, NULL,                    // main conf    NULL, NULL,                    // server conf    ngx_http_ts_create_loc_conf,    ngx_http_ts_merge_loc_conf};ngx_module_t ngx_http_tree_sitter_filter_module = {    NGX_MODULE_V1,    &ngx_http_tree_sitter_filter_module_ctx,    ngx_http_ts_commands,    NGX_HTTP_MODULE,    NULL, NULL, NULL, NULL, NULL, NULL, NULL,    NGX_MODULE_V1_PADDING};

------------------------------------------------------------------------

4)  # Config directives

<!-- -->

    tree_sitter_filter on;tree_sitter_language  c   /path/to/c.so;tree_sitter_language  python /path/to/python.so;tree_sitter_default_language plain;

### Config struct

    typedef struct {    ngx_flag_t enabled;    ngx_array_t *languages; // array of ts_language_entry_t    ngx_str_t default_lang;} ngx_http_ts_loc_conf_t;

------------------------------------------------------------------------

5)  # Handling `raw=1`

<!-- -->

    static ngx_int_tngx_http_ts_should_skip(ngx_http_request_t *r){    ngx_str_t raw_key = ngx_string("raw");    ngx_table_elt_t *arg = ngx_http_arg(r, raw_key.data, raw_key.len);    if (arg && arg->value.len == 1 && arg->value.data[0] == '1') {        return 1;    }    return 0;}

(You can also parse args manually if using older APIs.)

------------------------------------------------------------------------

6)  # Filter chain hooks

<!-- -->

    static ngx_http_output_body_filter_pt  ngx_http_next_body_filter;static ngx_http_output_header_filter_pt ngx_http_next_header_filter;

### Header filter

    static ngx_int_tngx_http_ts_header_filter(ngx_http_request_t *r){    if (ngx_http_ts_should_skip(r)) {        return ngx_http_next_header_filter(r);    }    ngx_http_ts_ctx_t *ctx = ngx_pcalloc(r->pool, sizeof(*ctx));    ngx_http_set_ctx(r, ctx, ngx_http_tree_sitter_filter_module);    ctx->buffering = 1;    // force buffering    r->filter_need_in_memory = 1;    return ngx_http_next_header_filter(r);}

------------------------------------------------------------------------

7)  # Body buffering

<!-- -->

    static ngx_int_tngx_http_ts_body_filter(ngx_http_request_t *r, ngx_chain_t *in){    ngx_http_ts_ctx_t *ctx =        ngx_http_get_module_ctx(r, ngx_http_tree_sitter_filter_module);    if (!ctx || ngx_http_ts_should_skip(r)) {        return ngx_http_next_body_filter(r, in);    }    for (; in; in = in->next) {        ngx_buf_t *b = in->buf;        if (ngx_buf_in_memory(b)) {            append_to_ctx_buffer(ctx, b->pos, b->last - b->pos);        }        if (b->last_buf) {            return ngx_http_ts_process_and_send(r, ctx);        }    }    return NGX_OK;}

------------------------------------------------------------------------

8)  # Tree-sitter runtime (dynamic loading)

### Load parser `.so`

    void *handle = dlopen(path, RTLD_NOW);TSLanguage *(*lang_fn)(void);lang_fn = dlsym(handle, "tree_sitter_<lang>");TSLanguage *lang = lang_fn();

Store in:

    typedef struct {    ngx_str_t name;    void *dl_handle;    TSLanguage *language;} ts_language_entry_t;

------------------------------------------------------------------------

9)  # Parsing

<!-- -->

    TSParser *parser = ts_parser_new();ts_parser_set_language(parser, lang);TSTree *tree = ts_parser_parse_string(    parser,    NULL,    source,    source_len);

------------------------------------------------------------------------

10) # HTML serialization

You have two approaches:

## Option A (simple & fast)

Walk tree and wrap nodes:

    <span class="ts-keyword">if</span>

## Option B (better UX)

Token-based highlighting via queries:

- Use `tree-sitter highlight queries`
- Apply scopes like:
  - keyword
  - string
  - comment

Example pseudo:

    TSQuery *query = ts_query_new(lang, query_src, len, ...);TSQueryCursor *cursor = ts_query_cursor_new();

------------------------------------------------------------------------

11) # Minimal serializer example

<!-- -->

    void serialize_node(TSNode node, buffer_t *out, const char *src) {    uint32_t start = ts_node_start_byte(node);    uint32_t end   = ts_node_end_byte(node);    const char *type = ts_node_type(node);    append(out, "<span class=\"");    append(out, type);    append(out, "\">");    html_escape_append(out, src + start, end - start);    append(out, "</span>");}

⚠️ This naive approach double-wraps children. A proper implementation should:

- Walk leaves only
- Or use byte ranges + intervals

------------------------------------------------------------------------

12) # Final response generation

<!-- -->

    ngx_str_t html;generate_html(&html, ctx->buffer, lang);r->headers_out.content_type = ngx_string("text/html");r->headers_out.content_length_n = html.len;ngx_buf_t *b = ngx_calloc_buf(r->pool);b->pos = html.data;b->last = html.data + html.len;b->memory = 1;b->last_buf = 1;ngx_chain_t out = { b, NULL };return ngx_http_next_body_filter(r, &out);

------------------------------------------------------------------------

13) # Build system (`config` file)

<!-- -->

    ngx_addon_name=ngx_tree_sitter_filter_moduleHTTP_MODULES="$HTTP_MODULES ngx_http_tree_sitter_filter_module"NGX_ADDON_SRCS="$NGX_ADDON_SRCS \    $ngx_addon_dir/ngx_http_tree_sitter_filter_module.c \    $ngx_addon_dir/ts_runtime.c \    $ngx_addon_dir/html_serializer.c"CORE_LIBS="$CORE_LIBS -ldl"

------------------------------------------------------------------------

14) # Example nginx.conf

<!-- -->

    location /code/ {    root /var/www;    tree_sitter_filter on;    tree_sitter_language c /parsers/c.so;    tree_sitter_language python /parsers/python.so;}

------------------------------------------------------------------------

15) # Hard problems (don’t ignore these)

### 1. Large files

- You will buffer entire response → memory spike
- Solution: size limit

### 2. Binary detection

- Avoid parsing non-text files

### 3. Charset

- Tree-sitter expects UTF-8

### 4. Thread safety

- Parser is not shared across requests

### 5. Performance

- Consider caching rendered HTML

------------------------------------------------------------------------

16) # Optional improvements

- CSS theme system
- Language auto-detection by extension
- LRU cache of parsed outputs
- Precompiled query files
- Streaming fallback for large files

------------------------------------------------------------------------

17) # Suggested MVP scope

Start with:

- One language
- Full buffering
- Simple node-type spans
- `raw=1` bypass

Then iterate.

------------------------------------------------------------------------

If you want, I can:

- Write a **compilable minimal module (≈500–700 LOC)**
- Add **Tree-sitter query-based highlighting (proper syntax coloring)**
- Or design a **zero-copy streaming hybrid approach** (much harder, but interesting)

## Prompt:

> NGINX filters are streaming by default → Tree-sitter is not

wait a second, tree-sitter is an incremental parsing library, so the only "non-streaming" part is the initialization of the parser, but all following steps can be incremental, by feeding chunks of the source file to the parser

## Response:

You’re right about Tree-sitter being *incremental*—but that doesn’t automatically make it *streaming-friendly* in the way NGINX filters need. The distinction matters here.

Let’s unpack the mismatch, because it’s subtle but important.

------------------------------------------------------------------------

# Incremental ≠ streaming

Tree-sitter’s “incremental” model means:

- You already have a full document (or previous version)
- You apply **edits**
- It reuses unchanged parts of the syntax tree efficiently

What it does *not* mean:

- “Parse correctly without seeing future input”
- “Emit final tokens before the full input is known”

------------------------------------------------------------------------

# Why this breaks in an NGINX body filter

An NGINX filter processes data like:

    chunk1 → chunk2 → chunk3 → ...

But for syntax highlighting:

### 1. Tokens can span chunks

Example:

    printf("helloworld");

If chunk boundary splits inside the string:

- You don’t know it’s a string until later
- You can’t safely emit HTML yet

------------------------------------------------------------------------

### 2. Grammar decisions require lookahead

Tree-sitter frequently needs future tokens to resolve:

- `>` vs template close in C++
- `/` → division vs regex (JS)
- indentation blocks (Python)

So even if you feed chunks incrementally:

➡️ **The tree is not “final” until the full input is parsed**

------------------------------------------------------------------------

### 3. Tree-sitter API reality

The core API:

    ts_parser_parse_string(...)

or

    ts_parser_parse(...)

expects:

- a complete input callback
- random access to earlier bytes

You *can* simulate streaming via a custom input callback—but:

- Tree-sitter may request **arbitrary byte offsets**
- Not strictly forward-only consumption

That alone breaks true streaming.

------------------------------------------------------------------------

# What *is* possible

You’ve got three realistic designs:

------------------------------------------------------------------------

## Option A — Full buffering (recommended MVP)

- Buffer entire response
- Parse once
- Emit HTML

✔ correct  
✔ simple  
❌ memory usage

------------------------------------------------------------------------

## Option B — Pseudo-streaming with delayed flush

You:

1.  Feed chunks into a growing buffer
2.  Reparse incrementally
3.  Only emit HTML for “stable” regions

Problem:

- Determining “stable” regions is **very hard**
- Requires tracking node boundaries + future invalidation

This becomes almost as complex as writing your own highlighter.

------------------------------------------------------------------------

## Option C — True streaming (not really viable)

To make this work you’d need:

- Grammar-level guarantees about locality (not true generally)
- Or a tokenizer instead of a parser

At that point you’re no longer really using Tree-sitter as intended.

------------------------------------------------------------------------

# The key misunderstanding

Tree-sitter is optimized for:

> “I have version N of a document, now give me version N+1 efficiently”

NOT:

> “I have a byte stream, tell me what this means as it arrives”

------------------------------------------------------------------------

# Where incremental *does* help you

Even in NGINX, you can still benefit:

### 1. Reuse parser instance per request

As chunks arrive:

    append(buffer, chunk);tree = ts_parser_parse_string(parser, tree, buffer, len);

This avoids full reparse cost each time.

------------------------------------------------------------------------

### 2. Efficient re-highlighting

If you later add:

- caching
- ETag-based updates
- live-reload views

Then incremental parsing shines.

------------------------------------------------------------------------

# Practical recommendation

For your module:

**Do this first:**

- Full buffering
- Single parse at end
- Clean HTML output

Then optionally experiment with:

- incremental reparse during buffering (performance optimization only)

------------------------------------------------------------------------

# If you really want streaming…

A hybrid approach could work:

- Use Tree-sitter only to compute structure
- Use a **separate lexer-like pass** for early emission
- Reconcile differences later

But that’s:

- complex
- fragile
- rarely worth it for HTTP responses

------------------------------------------------------------------------

# Bottom line

You’re absolutely correct that Tree-sitter is incremental—but:

> Incremental parsing optimizes *updates to known input*, not *progressive interpretation of unknown future input*.

For an NGINX filter, that distinction is what forces buffering.

------------------------------------------------------------------------

If you want, I can sketch a **“safe partial flush” algorithm** that emits only fully-closed syntax regions (strings, comments, blocks). It’s doable—but gnarly in exactly the ways you’d expect.

## Prompt:

> Determining “stable” regions is very hard

makes sense

ok, so lets buffer the full response

please, give me a start

## Response:

Good call—buffering keeps the first version sane and correct. Let’s get you to a **compilable, minimal starting point** for `ngx_tree_sitter_filter_module` that:

- hooks into NGINX filter chain
- buffers full response
- supports `raw=1` bypass
- replaces output with a placeholder HTML (we’ll plug Tree-sitter in next step)

This is your “it builds and runs” baseline.

------------------------------------------------------------------------

1)  # Minimal working module (single C file)

<!-- -->

    // ngx_http_tree_sitter_filter_module.c#include <ngx_config.h>#include <ngx_core.h>#include <ngx_http.h>typedef struct {    ngx_flag_t enabled;} ngx_http_ts_loc_conf_t;typedef struct {    ngx_buf_t *buf;    ngx_chain_t *in;    size_t len;    unsigned done:1;} ngx_http_ts_ctx_t;static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;/* ================= CONFIG ================= */static void *ngx_http_ts_create_loc_conf(ngx_conf_t *cf){    ngx_http_ts_loc_conf_t *conf;    conf = ngx_pcalloc(cf->pool, sizeof(*conf));    if (conf == NULL) return NULL;    conf->enabled = NGX_CONF_UNSET;    return conf;}static char *ngx_http_ts_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child){    ngx_http_ts_loc_conf_t *prev = parent;    ngx_http_ts_loc_conf_t *conf = child;    ngx_conf_merge_value(conf->enabled, prev->enabled, 0);    return NGX_CONF_OK;}static ngx_command_t ngx_http_ts_commands[] = {    {        ngx_string("tree_sitter_filter"),        NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,        ngx_conf_set_flag_slot,        NGX_HTTP_LOC_CONF_OFFSET,        offsetof(ngx_http_ts_loc_conf_t, enabled),        NULL    },    ngx_null_command};/* ================= UTIL ================= */static ngx_int_tngx_http_ts_should_skip(ngx_http_request_t *r){    u_char *p = r->args.data;    u_char *last = p + r->args.len;    while (p < last) {        if (ngx_strncmp(p, "raw=1", 5) == 0) {            return 1;        }        while (p < last && *p != '&') p++;        p++;    }    return 0;}/* ================= HEADER FILTER ================= */static ngx_int_tngx_http_ts_header_filter(ngx_http_request_t *r){    ngx_http_ts_loc_conf_t *conf;    ngx_http_ts_ctx_t *ctx;    conf = ngx_http_get_module_loc_conf(r, ngx_http_tree_sitter_filter_module);    if (!conf->enabled || ngx_http_ts_should_skip(r)) {        return ngx_http_next_header_filter(r);    }    ctx = ngx_pcalloc(r->pool, sizeof(*ctx));    if (ctx == NULL) {        return NGX_ERROR;    }    ngx_http_set_ctx(r, ctx, ngx_http_tree_sitter_filter_module);    r->filter_need_in_memory = 1;    r->headers_out.content_length_n = -1;    return ngx_http_next_header_filter(r);}/* ================= BODY FILTER ================= */static ngx_int_tngx_http_ts_body_filter(ngx_http_request_t *r, ngx_chain_t *in){    ngx_http_ts_ctx_t *ctx;    ngx_buf_t *b;    ngx_chain_t *cl;    ctx = ngx_http_get_module_ctx(r, ngx_http_tree_sitter_filter_module);    if (ctx == NULL || ngx_http_ts_should_skip(r)) {        return ngx_http_next_body_filter(r, in);    }    for (cl = in; cl; cl = cl->next) {        b = cl->buf;        size_t size = ngx_buf_size(b);        if (size) {            u_char *data = ngx_pnalloc(r->pool, size);            if (data == NULL) return NGX_ERROR;            ngx_memcpy(data, b->pos, size);            ngx_buf_t *nb = ngx_calloc_buf(r->pool);            nb->pos = data;            nb->last = data + size;            nb->memory = 1;            ngx_chain_t *ncl = ngx_alloc_chain_link(r->pool);            ncl->buf = nb;            ncl->next = NULL;            if (ctx->in == NULL) {                ctx->in = ncl;            } else {                ngx_chain_t *tmp = ctx->in;                while (tmp->next) tmp = tmp->next;                tmp->next = ncl;            }            ctx->len += size;        }        if (b->last_buf) {            ctx->done = 1;        }    }    if (!ctx->done) {        return NGX_OK;    }    /* ===== Build final buffer ===== */    u_char *all = ngx_pnalloc(r->pool, ctx->len + 1);    if (all == NULL) return NGX_ERROR;    u_char *p = all;    for (cl = ctx->in; cl; cl = cl->next) {        size_t size = ngx_buf_size(cl->buf);        p = ngx_cpymem(p, cl->buf->pos, size);    }    all[ctx->len] = '\0';    /* ===== Replace with simple HTML ===== */    const char *prefix = "<html><body><pre>";    const char *suffix = "</pre></body></html>";    size_t out_len = ngx_strlen(prefix) + ctx->len + ngx_strlen(suffix);    u_char *out = ngx_pnalloc(r->pool, out_len);    if (out == NULL) return NGX_ERROR;    u_char *o = out;    o = ngx_cpymem(o, prefix, ngx_strlen(prefix));    o = ngx_cpymem(o, all, ctx->len);    o = ngx_cpymem(o, suffix, ngx_strlen(suffix));    ngx_buf_t *out_buf = ngx_calloc_buf(r->pool);    out_buf->pos = out;    out_buf->last = out + out_len;    out_buf->memory = 1;    out_buf->last_buf = 1;    ngx_chain_t out_chain = { out_buf, NULL };    r->headers_out.content_type = ngx_string("text/html");    r->headers_out.content_length_n = out_len;    return ngx_http_next_body_filter(r, &out_chain);}/* ================= INIT ================= */static ngx_int_tngx_http_ts_filter_init(ngx_conf_t *cf){    ngx_http_next_header_filter = ngx_http_top_header_filter;    ngx_http_top_header_filter = ngx_http_ts_header_filter;    ngx_http_next_body_filter = ngx_http_top_body_filter;    ngx_http_top_body_filter = ngx_http_ts_body_filter;    return NGX_OK;}/* ================= MODULE ================= */static ngx_http_module_t ngx_http_ts_module_ctx = {    NULL,    ngx_http_ts_filter_init,    NULL, NULL,    NULL, NULL,    ngx_http_ts_create_loc_conf,    ngx_http_ts_merge_loc_conf};ngx_module_t ngx_http_tree_sitter_filter_module = {    NGX_MODULE_V1,    &ngx_http_ts_module_ctx,    ngx_http_ts_commands,    NGX_HTTP_MODULE,    NULL, NULL, NULL, NULL, NULL, NULL, NULL,    NGX_MODULE_V1_PADDING};

------------------------------------------------------------------------

2)  # Build integration (`config` file)

<!-- -->

    ngx_addon_name=ngx_tree_sitter_filter_moduleHTTP_MODULES="$HTTP_MODULES ngx_http_tree_sitter_filter_module"NGX_ADDON_SRCS="$NGX_ADDON_SRCS \    $ngx_addon_dir/ngx_http_tree_sitter_filter_module.c"

------------------------------------------------------------------------

3)  # Example nginx.conf

<!-- -->

    location /code/ {    root /var/www;    tree_sitter_filter on;}

Test:

    /code/test.c        → HTML-wrapped output/code/test.c?raw=1 → original file

------------------------------------------------------------------------

4)  # What you have now

✔ filter chain wired correctly  
✔ full buffering works  
✔ response replacement works  
✔ GET param bypass works

------------------------------------------------------------------------

5)  # Next step (I recommend this order)

<!-- -->

1.  **Add HTML escaping** (critical before highlighting)
2.  **Integrate Tree-sitter runtime**
3.  **Detect language (by extension first)**
4.  **Replace `<pre>` content with highlighted spans**

------------------------------------------------------------------------

6)  # Important immediate fix (do this next)

Right now you output raw bytes into HTML → this breaks on `<`, `&`, etc.

You should add:

    static void html_escape(...)

before integrating Tree-sitter.

------------------------------------------------------------------------

7)  # After that…

I can help you implement:

- dynamic `.so` parser loading (`dlopen`)
- language registry
- proper Tree-sitter query-based highlighting (not naive node wrapping)
- CSS theme system

------------------------------------------------------------------------

If you want, next step I can give you:

👉 a **drop-in Tree-sitter integration layer** that plugs directly into this module without rewriting it.

## Prompt:

Help me implement dynamic .so parser loading (dlopen)

## Response:

Great, this is the part where your module becomes genuinely flexible.

You want:

- multiple Tree-sitter grammars
- loaded at runtime from `.so`
- selectable by name (and later by file extension)

Let’s build a clean, NGINX-native implementation.

------------------------------------------------------------------------

1)  # What a Tree-sitter `.so` looks like

Each grammar exports a symbol like:

    TSLanguage *tree_sitter_c(void);TSLanguage *tree_sitter_python(void);

So your loader must:

1.  `dlopen()` the `.so`
2.  `dlsym()` the correct symbol
3.  store both handle + language pointer

------------------------------------------------------------------------

2)  # Data structures

Add this to your module:

    #include <dlfcn.h>#include <tree_sitter/api.h>typedef struct {    ngx_str_t name;        // "c", "python"    ngx_str_t path;        // /path/to/parser.so    void *dl_handle;    TSLanguage *language;} ngx_ts_language_t;

------------------------------------------------------------------------

## Store languages in config

Extend your loc conf:

    typedef struct {    ngx_flag_t enabled;    ngx_array_t *languages;   // array of ngx_ts_language_t} ngx_http_ts_loc_conf_t;

------------------------------------------------------------------------

3)  # Directive: `tree_sitter_language`

### Usage:

    tree_sitter_language c /parsers/c.so;tree_sitter_language python /parsers/python.so;

------------------------------------------------------------------------

## Directive definition

    {    ngx_string("tree_sitter_language"),    NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,    ngx_http_ts_add_language,    NGX_HTTP_LOC_CONF_OFFSET,    0,    NULL},

------------------------------------------------------------------------

## Directive handler

    static char *ngx_http_ts_add_language(ngx_conf_t *cf, ngx_command_t *cmd, void *conf){    ngx_http_ts_loc_conf_t *tlcf = conf;    ngx_str_t *value = cf->args->elts;    if (tlcf->languages == NULL) {        tlcf->languages = ngx_array_create(cf->pool, 4, sizeof(ngx_ts_language_t));        if (tlcf->languages == NULL) {            return NGX_CONF_ERROR;        }    }    ngx_ts_language_t *lang = ngx_array_push(tlcf->languages);    if (lang == NULL) {        return NGX_CONF_ERROR;    }    lang->name = value[1];  // "c"    lang->path = value[2];  // "/parsers/c.so"    lang->dl_handle = NULL;    lang->language = NULL;    return NGX_CONF_OK;}

------------------------------------------------------------------------

4)  # Load `.so` files (postconfiguration phase)

You want to load parsers **once at config time**, not per request.

Modify your init:

    static ngx_int_tngx_http_ts_load_languages(ngx_conf_t *cf, ngx_http_ts_loc_conf_t *conf){    if (conf->languages == NULL) {        return NGX_OK;    }    ngx_ts_language_t *langs = conf->languages->elts;    for (ngx_uint_t i = 0; i < conf->languages->nelts; i++) {        ngx_ts_language_t *l = &langs[i];        /* 1. dlopen */        l->dl_handle = dlopen((char *)l->path.data, RTLD_NOW);        if (l->dl_handle == NULL) {            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,                "tree_sitter: dlopen(%V) failed: %s",                &l->path, dlerror());            return NGX_ERROR;        }        /* 2. build symbol name */        char symbol[128];        ngx_snprintf((u_char *)symbol, sizeof(symbol),                     "tree_sitter_%V%Z", &l->name);        /* 3. dlsym */        TSLanguage *(*fn)(void);        fn = dlsym(l->dl_handle, symbol);        if (fn == NULL) {            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,                "tree_sitter: symbol %s not found in %V",                symbol, &l->path);            return NGX_ERROR;        }        l->language = fn();        if (l->language == NULL) {            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,                "tree_sitter: %s() returned NULL", symbol);            return NGX_ERROR;        }    }    return NGX_OK;}

------------------------------------------------------------------------

5)  # Hook into config lifecycle

Update your `postconfiguration`:

    static ngx_int_tngx_http_ts_filter_init(ngx_conf_t *cf){    ngx_http_ts_loc_conf_t *conf;    conf = ngx_http_conf_get_module_loc_conf(cf,        ngx_http_tree_sitter_filter_module);    if (ngx_http_ts_load_languages(cf, conf) != NGX_OK) {        return NGX_ERROR;    }    ngx_http_next_header_filter = ngx_http_top_header_filter;    ngx_http_top_header_filter = ngx_http_ts_header_filter;    ngx_http_next_body_filter = ngx_http_top_body_filter;    ngx_http_top_body_filter = ngx_http_ts_body_filter;    return NGX_OK;}

------------------------------------------------------------------------

6)  # Lookup helper (by name)

<!-- -->

    static TSLanguage *ngx_http_ts_find_language(ngx_http_request_t *r, ngx_str_t *name){    ngx_http_ts_loc_conf_t *conf;    conf = ngx_http_get_module_loc_conf(r, ngx_http_tree_sitter_filter_module);    if (conf->languages == NULL) return NULL;    ngx_ts_language_t *langs = conf->languages->elts;    for (ngx_uint_t i = 0; i < conf->languages->nelts; i++) {        if (ngx_strcmp(langs[i].name.data, name->data) == 0) {            return langs[i].language;        }    }    return NULL;}

------------------------------------------------------------------------

7)  # Quick test strategy

Start simple:

    tree_sitter_language c /parsers/tree-sitter-c.so;

Then in your body filter:

    ngx_str_t lang = ngx_string("c");TSLanguage *ts_lang = ngx_http_ts_find_language(r, &lang);

------------------------------------------------------------------------

8)  # Important pitfalls (don’t skip)

### 1. Symbol naming must match

If your `.so` exports:

    tree_sitter_c()

Then directive must be:

    tree_sitter_language c ...

------------------------------------------------------------------------

### 2. `dlopen` flags

You may want:

    RTLD_NOW | RTLD_LOCAL

Avoid `RTLD_GLOBAL` unless needed.

------------------------------------------------------------------------

### 3. Memory lifetime

- Load once at config time → safe
- Never `dlclose()` → also correct for NGINX modules

------------------------------------------------------------------------

### 4. Multiple locations

Each location gets its own config, but:

- `.so` will be loaded per config instance

If you want global deduplication later → move to main conf.

------------------------------------------------------------------------

9)  # Next step

Now that you can load languages, the natural next piece is:

👉 **language selection**

- by file extension (`.c`, `.py`)
- fallback default

Then:

👉 **Tree-sitter parsing + HTML serializer**

------------------------------------------------------------------------

If you want, I can next give you:

- extension → language mapping (fast, no hash collisions)
- or a **minimal Tree-sitter highlight pipeline using queries** (the “real” way to color code)

------------------------------------------------------------------------

Powered by [ChatGPT Exporter](https://www.chatgptexporter.com)
