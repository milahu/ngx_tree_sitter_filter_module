#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <dlfcn.h>
#include <tree_sitter/api.h>

#ifndef NGX_COMPAT
    #error "nginx must be configured with --with-compat to enable r->headers_in.accept"
#endif

#define DEBUG 0

#define HTML_META_GENERATOR "https://github.com/milahu/ngx_tree_sitter_filter_module"

// forward declare module
extern ngx_module_t ngx_http_tree_sitter_filter_module;

// persistent state: NGX_HTTP_LOC_CONF = per location, per worker
// shared between requests
typedef struct {
    ngx_flag_t enabled;
    ngx_str_t language_name;
    ngx_str_t parser_path;
    ngx_str_t highlights_path;
    ngx_str_t css_style;
    ngx_flag_t language_loaded;
    void *dl_handle;
    TSLanguage *language;
    TSQuery *query;
} ngx_http_ts_loc_conf_t;

// temporary state: per request
typedef struct {
    ngx_buf_t *buf;
    ngx_chain_t *in;
    size_t len;
    unsigned done:1;
} ngx_http_ts_ctx_t;

typedef struct {
    uint32_t start;
    uint32_t end;
    const char *class_name;
} ts_span_t;



// function prototypes

static ngx_int_t ngx_http_ts_filter_init(ngx_conf_t *cf);

static ngx_int_t ngx_http_ts_header_filter(ngx_http_request_t *r);

static ngx_int_t ngx_http_ts_body_filter(ngx_http_request_t *r, ngx_chain_t *in);

static void *ngx_http_ts_create_loc_conf(ngx_conf_t *cf);

static char *ngx_http_ts_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);

static ngx_int_t ngx_http_ts_load_language_runtime(ngx_http_request_t *r, ngx_http_ts_loc_conf_t *conf);

static ngx_int_t ngx_http_ts_highlight(ngx_http_request_t *r, ngx_http_ts_loc_conf_t *conf, u_char *src, size_t len, u_char **out, size_t *out_len);

static TSQuery *ngx_ts_load_query(ngx_http_request_t *r, ngx_http_ts_loc_conf_t *conf);

//static const char *ts_capture_to_class(const char *name, uint32_t len);

static ngx_int_t ts_collect_spans(ngx_http_request_t *r, ngx_http_ts_loc_conf_t *conf, TSTree *tree, ngx_array_t *spans);

static int ts_span_cmp(const void *a, const void *b);

static ngx_int_t ts_render_html(ngx_http_request_t *r, u_char *src, size_t len, ngx_array_t *spans, u_char **out, size_t *out_len);

static ngx_inline u_char *ts_escape_char(u_char *p, u_char c);



// global state

static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;

static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;



// nginx config directives
static ngx_command_t ngx_http_ts_directives[] = {

    {
        ngx_string("tree_sitter_filter"),
        NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
        ngx_conf_set_flag_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_ts_loc_conf_t, enabled),
        NULL
    },

    {
        ngx_string("tree_sitter_language_name"),
        NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_ts_loc_conf_t, language_name),
        NULL
    },

    {
        ngx_string("tree_sitter_parser_path"),
        NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_ts_loc_conf_t, parser_path),
        NULL
    },

    {
        ngx_string("tree_sitter_highlights_path"),
        NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_ts_loc_conf_t, highlights_path),
        NULL
    },

    {
        ngx_string("tree_sitter_css_style"),
        NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_ts_loc_conf_t, css_style),
        NULL
    },

    ngx_null_command
};



/* ================= MODULE ================= */

static ngx_http_module_t ngx_http_ts_module_ctx = {
    NULL, // preconfiguration
    ngx_http_ts_filter_init, // postconfiguration

    NULL, // create main configuration
    NULL, // init main configuration

    NULL, // create server configuration
    NULL, // merge server configuration

    ngx_http_ts_create_loc_conf, // create location configuration
    ngx_http_ts_merge_loc_conf // merge location configuration
};

ngx_module_t ngx_http_tree_sitter_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_ts_module_ctx, // module context
    ngx_http_ts_directives, // module directives
    NGX_HTTP_MODULE, // module type
    NULL, // init master
    NULL, // init module
    NULL, // init process
    NULL, // init thread
    NULL, // exit thread
    NULL, // exit process
    NULL, // exit master
    NGX_MODULE_V1_PADDING
};



/* ================= CONFIG ================= */

static void *
ngx_http_ts_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_ts_loc_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(*conf));
    if (conf == NULL) return NULL;

    conf->enabled = NGX_CONF_UNSET;

    return conf;
}

static char *
ngx_http_ts_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_ts_loc_conf_t *prev = parent;
    ngx_http_ts_loc_conf_t *conf = child;

    ngx_conf_merge_value(conf->enabled, prev->enabled, 0);

    return NGX_CONF_OK;
}



// init
// Hook into config lifecycle
static ngx_int_t
ngx_http_ts_filter_init(ngx_conf_t *cf)
{
    #if DEBUG
    ngx_log_error(NGX_LOG_ERR, cf->log, 0, "ngx_http_ts_filter_init: hello"); // debug
    #endif

    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_ts_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_ts_body_filter;

    return NGX_OK;
}



/* ================= UTIL ================= */

static ngx_int_t
ngx_http_ts_should_skip(ngx_http_request_t *r)
{
    u_char *p = r->args.data;
    u_char *last = p + r->args.len;

    #if DEBUG
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_ts_should_skip: hello");
    #endif

    while (p < last) {
        if (ngx_strncmp(p, "raw=1", 5) == 0) {
            #if DEBUG
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_ts_should_skip: found 'raw=1' in args.data -> skipping");
            #endif
            return 1;
        }

        while (p < last && *p != '&') p++;
        p++;
    }

    ngx_table_elt_t  *accept;
    accept = r->headers_in.accept;
    if (accept != NULL) {
        // TODO better? use a regex?
        if (accept->value.len >= 10 && ngx_strstr(accept->value.data, "text/plain") != NULL) {
            #if DEBUG
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_ts_should_skip: found 'text/plain' in headers_in.accept=%s -> skipping", accept->value.data);
            #endif
            return 1;
        }
        #if DEBUG
        else {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_ts_should_skip: not found 'text/plain' in headers_in.accept=%s", accept->value.data);
        }
        #endif
    }
    #if DEBUG
    else {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_ts_should_skip: headers_in.accept is empty");
    }
    #endif

    return 0;
}



static ngx_int_t
ngx_http_ts_load_language_runtime(
    ngx_http_request_t *r,
    ngx_http_ts_loc_conf_t *conf
)
{
    #if DEBUG
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_ts_load_language_runtime: hello");
    #endif

    if (conf->language_loaded) {
        return NGX_OK;
    }

    if (conf->parser_path.data == NULL) {
        conf->language_loaded = 1;
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
            "tree_sitter: no parser_path");
        return NGX_OK;
    }

    if (conf->highlights_path.data == NULL) {
        conf->language_loaded = 1;
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
            "tree_sitter: no highlights_path");
        return NGX_OK;
    }

    { // keep indent

        if (conf->language != NULL) {
            // already loaded
            return NGX_OK;
        }

        conf->dl_handle = dlopen((char *)conf->parser_path.data, RTLD_NOW | RTLD_LOCAL);
        if (conf->dl_handle == NULL) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                "tree_sitter: dlopen(%V) failed: %s",
                &conf->parser_path, dlerror());
            return NGX_ERROR;
        }

        // build symbol name
        char symbol[128];
        ngx_snprintf((u_char *)symbol, sizeof(symbol),
                     "tree_sitter_%V%Z", &conf->language_name);

        TSLanguage *(*fn)(void);
        fn = (TSLanguage *(*)(void)) dlsym(conf->dl_handle, symbol);

        if (fn == NULL) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                "tree_sitter: symbol %s not found in %V",
                symbol, &conf->parser_path);
            return NGX_ERROR;
        }

        conf->language = fn();

        // load parser/queries/highlights.scm
        conf->query = ngx_ts_load_query(r, conf);

        if (conf->language == NULL) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                "tree_sitter: %s() returned NULL", symbol);
            return NGX_ERROR;
        }
    }

    conf->language_loaded = 1;

    return NGX_OK;
}



static ngx_inline u_char *
ts_escape_char(u_char *p, u_char c)
{
    switch (c) {
        case '<':
            p = ngx_cpymem(p, "&lt;", 4);
            break;

        #if 0
        case '>':
            p = ngx_cpymem(p, "&gt;", 4);
            break;
        #endif

        case '&':
            p = ngx_cpymem(p, "&amp;", 5);
            break;

        default:
            *p++ = c;
            break;
    }

    return p;
}



/* ================= HEADER FILTER ================= */

static ngx_int_t
ngx_http_ts_header_filter(ngx_http_request_t *r)
{
    ngx_http_ts_loc_conf_t *conf;
    ngx_http_ts_ctx_t *ctx;

    conf = ngx_http_get_module_loc_conf(r, ngx_http_tree_sitter_filter_module);

    #if DEBUG
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
        "ngx_http_ts_header_filter: enabled=%d uri=%V",
        conf->enabled, &r->uri
    );
    #endif

    if (!conf->enabled || ngx_http_ts_should_skip(r)) {
        return ngx_http_next_header_filter(r);
    }

    ctx = ngx_pcalloc(r->pool, sizeof(*ctx));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    // set request context
    ngx_http_set_ctx(r, ctx, ngx_http_tree_sitter_filter_module);

    r->filter_need_in_memory = 1;

    // we dont know content-length in advance
    r->headers_out.content_length_n = -1;

    // TODO move to body_filter
    if (!conf->language_loaded) {
        if (ngx_http_ts_load_language_runtime(r, conf) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    // we have to set content-type:text/html here
    // so later in body_filter, we cannot fall back to text/plain
    ngx_str_t mime = ngx_string("text/html;charset=utf-8");
    r->headers_out.content_type = mime;
    r->main_filter_need_in_memory = 1;

    return ngx_http_next_header_filter(r);
}



/* ================= BODY FILTER ================= */

static ngx_int_t
ngx_http_ts_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_http_ts_loc_conf_t *conf;
    ngx_http_ts_ctx_t *ctx;
    ngx_buf_t *b;
    // ngx_buf_t *out;
    ngx_chain_t *cl;

    conf = ngx_http_get_module_loc_conf(r, ngx_http_tree_sitter_filter_module);

    ctx = ngx_http_get_module_ctx(r, ngx_http_tree_sitter_filter_module);

    if (ctx == NULL || ngx_http_ts_should_skip(r)) {
        #if 0
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
            "ngx_http_ts_body_filter: uri=%V -> skip", &r->uri
        );
        #endif
        return ngx_http_next_body_filter(r, in);
    }

    #if DEBUG
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
        "ngx_http_ts_body_filter: uri=%V -> filter", &r->uri
    );
    #endif

    // copy input from cl to ctx->in
    for (cl = in; cl; cl = cl->next) {

        b = cl->buf;

        size_t size = ngx_buf_size(b);

        if (size) {
            u_char *data = ngx_pnalloc(r->pool, size);
            if (data == NULL) return NGX_ERROR;

            ngx_memcpy(data, b->pos, size);

            ngx_buf_t *nb = ngx_calloc_buf(r->pool);
            nb->pos = data;
            nb->last = data + size;
            nb->memory = 1;

            ngx_chain_t *ncl = ngx_alloc_chain_link(r->pool);
            ncl->buf = nb;
            ncl->next = NULL;

            if (ctx->in == NULL) {
                ctx->in = ncl;
            } else {
                ngx_chain_t *tmp = ctx->in;
                while (tmp->next) tmp = tmp->next;
                tmp->next = ncl;
            }

            ctx->len += size;
        }

        if (b->last_buf) {
            ctx->done = 1;
        }
    }

    if (!ctx->done) {
        return NGX_OK;
    }

    /* ===== Build final buffer ===== */
    // copy input from ctx->in to all

    u_char *all = ngx_pnalloc(r->pool, ctx->len + 1);
    if (all == NULL) return NGX_ERROR;

    u_char *p = all;

    for (cl = ctx->in; cl; cl = cl->next) {
        size_t size = ngx_buf_size(cl->buf);
        p = ngx_cpymem(p, cl->buf->pos, size);
    }

    all[ctx->len] = '\0';

    u_char *highlighted;
    size_t out_len;
    size_t highlighted_len;

    const char *prefix = (
        "<!doctype html>\n"
        // explain the API
        "<!--\n"
        "  to get the raw version of this file\n"
        "  append the request path with `?raw=1`\n"
        "  or add the request header `accept:text/plain`\n"
        "-->\n"
        "<html>\n"
        "<head>\n"
        "<meta charset=\"utf8\">\n"
        "<meta name=\"generator\" content=\"" HTML_META_GENERATOR "\">\n"
        "<style>\n"
        // TODO use conf->css_style
        // ".keyword { color: #c00; }\n" // darkred
        // ".keyword { color: #a910d3; }\n" // purple
        ".keyword { color: #1b10e6; }\n" // blue
        // ".string { color: #080; }\n" // green
        ".string { color: #a21f2d; }\n" // red
        // ".comment { color: #888; }\n" // gray
        ".comment { color: #008036; }\n" // green
        ".function { color: #06c; }\n" // blue
        ".type { color: #a0a; }\n" // purple
        "</style>\n"
        "</head>\n"
        // TODO add conf->language_name as class
        "<body class=\"code\">\n"
        "<pre>"
    );

    const char *suffix = (
        "</pre>\n"
        "</body>\n"
        "</html>\n"
    );

    // create output buffer
    ngx_buf_t *out_buf = ngx_calloc_buf(r->pool);
    /*
    out_buf->pos = out;
    out_buf->last = out + out_len;
    out_buf->memory = 1;
    out_buf->last_buf = 1;
    */

    #if DEBUG
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_ts_body_filter: calling ngx_http_ts_highlight");
    #endif

    if (
        conf->language != NULL &&
        // parse input
        ngx_http_ts_highlight(
            r,
            conf,
            all,
            ctx->len,
            &highlighted,
            &highlighted_len
        ) == NGX_OK
    )
    {
        // highlight done

        out_len = ngx_strlen(prefix) + highlighted_len + ngx_strlen(suffix);

        u_char *out = ngx_pnalloc(r->pool, out_len);
        if (out == NULL) return NGX_ERROR;

        out_buf->pos = out;
        out_buf->last = out;

        #if DEBUG
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_ts_body_filter: copying prefix");
        #endif

        out_buf->last = ngx_cpymem(out_buf->last, prefix, ngx_strlen(prefix));

        #if DEBUG
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_ts_body_filter: copying highlighted: len=%d", highlighted_len);
        #endif

        out_buf->last = ngx_cpymem(out_buf->last, highlighted, highlighted_len);

        #if DEBUG
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_ts_body_filter: copying suffix");
        #endif

        out_buf->last = ngx_cpymem(out_buf->last, suffix, ngx_strlen(suffix));

        #if DEBUG
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_ts_body_filter: done copying");
        #endif

        // TODO? free highlighted
    } else {
        // FIXME return valid HTML, since in header_filter, we have already sent content-type:text/html
        #if DEBUG
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_ts_body_filter: calling ngx_http_ts_highlight failed");
        #endif
        // highlight failed -> fallback
        out_len = ctx->len;
        // out_buf->last = ngx_cpymem(out_buf->last, all, out_len);
        out_buf->pos = all;
        out_buf->last = all + ctx->len;
    }

    /*
    ngx_buf_t *out_buf = ngx_calloc_buf(r->pool);
    out_buf->pos = out;
    out_buf->last = out + out_len;
    out_buf->memory = 1;
    out_buf->last_buf = 1;
    */

    out_buf->memory = 1;
    out_buf->last_buf = 1;

    #if DEBUG
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_ts_body_filter: creating out_chain");
    #endif

    ngx_chain_t out_chain = { out_buf, NULL };

    r->headers_out.content_length_n = out_len;

    return ngx_http_next_body_filter(r, &out_chain);
}



static ngx_int_t
ngx_http_ts_highlight(
    ngx_http_request_t *r,
    ngx_http_ts_loc_conf_t *conf,
    u_char *src,
    size_t len,
    u_char **out,
    size_t *out_len
)
{
    #if DEBUG
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_ts_highlight: hello");
    #endif

    TSParser *parser = ts_parser_new();
    if (parser == NULL) return NGX_ERROR;

    if (!ts_parser_set_language(parser, conf->language)) {
        ts_parser_delete(parser);
        return NGX_ERROR;
    }

    TSTree *tree = ts_parser_parse_string(
        parser,
        NULL,
        (const char *)src,
        len
    );

    if (tree == NULL) {
        ts_parser_delete(parser);
        return NGX_ERROR;
    }



    #if 0
    TSNode root = ts_tree_root_node(tree);

    /* For now: fallback = plain escaped text */
    /* We'll replace this with real highlighting next */

    size_t cap = len * 2 + 64;
    u_char *buf = ngx_pnalloc(r->pool, cap);
    if (buf == NULL) return NGX_ERROR;

    u_char *p = buf;

    for (size_t i = 0; i < len; i++) {
        p = ts_escape_char(p, src[i]);
    }

    *out = buf;
    *out_len = p - buf;

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    return NGX_OK;
    #endif



    ngx_array_t *spans =
        ngx_array_create(r->pool, 128, sizeof(ts_span_t));

    if (!spans) return NGX_ERROR;

    if (ts_collect_spans(r, conf, tree, spans) != NGX_OK) {
        return NGX_ERROR;
    }

    ngx_int_t rc = ts_render_html(r, src, len, spans, out, out_len);

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    return rc;
}



static TSQuery *
ngx_ts_load_query(ngx_http_request_t *r, ngx_http_ts_loc_conf_t *conf)
{
    #if DEBUG
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_ts_load_query: hello");
    #endif

    FILE *f = fopen((const char *)conf->highlights_path.data, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    size_t len = ftell(f);
    rewind(f);

    char *src = malloc(len);
    size_t read_len = fread(src, 1, len, f);
    if (read_len < len) {
        // error: failed to read queries file
        fclose(f);
        free(src);
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
            "tree_sitter: failed to read queries file. read only %d of %d bytes from %s", read_len, len, conf->highlights_path.data);
        return NULL;
    }
    fclose(f);

    uint32_t err_offset;
    TSQueryError err;

    TSQuery *q = ts_query_new(
        conf->language,
        src,
        len,
        &err_offset,
        &err
    );

    free(src);

    if (!q) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
            "tree_sitter: query compile failed at %d", err_offset);
        return NULL;
    }

    return q;
}



#if false
static const char *
ts_capture_to_class(const char *name, uint32_t len)
{
    if (strncmp(name, "keyword", len) == 0) return "kw";
    if (strncmp(name, "string", len) == 0) return "str";
    if (strncmp(name, "comment", len) == 0) return "com";
    if (strncmp(name, "function", len) == 0) return "fn";
    if (strncmp(name, "type", len) == 0) return "type";

    return "tok";
}
#endif



static ngx_int_t
ts_collect_spans(
    ngx_http_request_t *r,
    ngx_http_ts_loc_conf_t *conf,
    TSTree *tree,
    ngx_array_t *spans
)
{
    #if DEBUG
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ts_collect_spans: hello");
    #endif

    TSQueryCursor *cursor = ts_query_cursor_new();
    if (!cursor) return NGX_ERROR;

    TSNode root = ts_tree_root_node(tree);

    ts_query_cursor_exec(cursor, conf->query, root);

    TSQueryMatch match;
    uint32_t capture_index;

    #if DEBUG
    uint32_t capture_count = ts_query_capture_count(conf->query);

    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
        "ts_collect_spans: capture_count=%d", capture_count);
    #endif

    while (ts_query_cursor_next_capture(cursor, &match, &capture_index)) {

        TSQueryCapture capture = match.captures[capture_index];

        TSNode node = capture.node;

        uint32_t start = ts_node_start_byte(node);
        uint32_t end = ts_node_end_byte(node);

        uint32_t cap_name_len;

        const char *cap_name = ts_query_capture_name_for_id(
            conf->query,
            capture.index,
            &cap_name_len
        );

        #if 0
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
            "ts_collect_spans: start=%d end=%d capture_index=%d capture.index=%d cap_name=%s",
            start, end, capture_index, capture.index, cap_name
        );
        #endif

        ts_span_t *s = ngx_array_push(spans);
        if (!s) return NGX_ERROR;

        s->start = start;
        s->end = end;
        // s->class_name = ts_capture_to_class(cap_name, cap_name_len);
        s->class_name = cap_name; // TODO copy string?
    }

    ts_query_cursor_delete(cursor);
    return NGX_OK;
}



static int
ts_span_cmp(const void *a, const void *b)
{
    const ts_span_t *A = a;
    const ts_span_t *B = b;
    return (int)(A->start - B->start);
}



static ngx_int_t
ts_render_html(
    ngx_http_request_t *r,
    u_char *src, size_t len,
    ngx_array_t *spans,
    u_char **out, size_t *out_len
)
{
    #if DEBUG
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ts_render_html: spans->elts");
    #endif
    ts_span_t *s = spans->elts;

    #if DEBUG
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ts_render_html: spans->nelts");
    #endif
    ngx_uint_t n = spans->nelts;

    #if DEBUG
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ts_render_html: qsort");
    #endif
    qsort(s, n, sizeof(ts_span_t), ts_span_cmp);

    // size_t cap = len * 4 + 1024; // too small?
    size_t cap = len * 10 + 10240;
    #if DEBUG
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ts_render_html: buf = ngx_pnalloc");
    #endif
    u_char *buf = ngx_pnalloc(r->pool, cap);
    if (!buf) return NGX_ERROR;

    u_char *p = buf;
    size_t pos = 0;
    size_t prev_start = (size_t)(-1);

    #if DEBUG
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ts_render_html: looping spans: n=%d", n);
    #endif

    for (ngx_uint_t i = 0; i < n; i++) {

        if (s[i].start == prev_start) {
            // dont emit empty span
            continue;
        }

        /* emit text before span */
        while (pos < s[i].start && pos < len) {
            p = ts_escape_char(p, src[pos++]);
        }

        /* open span */
        p += sprintf((char *)p, "<span class=\"%s\">", s[i].class_name);

        #if 0
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ts_render_html: looping spans: i=%d class=%s start=%d end=%d len=%d",
            i, s[i].class_name, s[i].start, s[i].end, (s[i].end - s[i].start)
        );
        #endif

        /* emit span content */
        while (pos < s[i].end && pos < len) {
            p = ts_escape_char(p, src[pos++]);
        }

        /* close */
        p += sprintf((char *)p, "</span>");

        prev_start = s[i].start;
    }

    #if DEBUG
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ts_render_html: tail");
    #endif

    /* tail */
    while (pos < len) {
        p = ts_escape_char(p, src[pos++]);
    }

    *out = buf;
    *out_len = p - buf;

    return NGX_OK;
}
