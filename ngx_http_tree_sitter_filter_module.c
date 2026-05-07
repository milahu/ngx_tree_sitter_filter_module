#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <dlfcn.h>
#include <tree_sitter/api.h>

#ifndef NGX_COMPAT
    #error "nginx must be configured with --with-compat to enable r->headers_in.accept"
#endif

#define DEBUG 0
#define DEBUG2 0
#define DEBUG3 1

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

typedef struct {
    ngx_http_request_t *r;
    ngx_chain_t *chain;
    ngx_chain_t *current;
    size_t current_start; // absolute start offset of current buffer
    size_t current_end; // absolute end offset (exclusive)
    size_t pos; // position of the next char to read
    size_t size; // total number of chars in the chain
} ts_reader_t;

typedef struct {
    ngx_http_request_t *r;
    ngx_chain_t *head;
    ngx_chain_t *tail;
    ngx_buf_t *buf;
    size_t capacity;
} ts_writer_t;

// temporary state: per request
typedef struct {
    ngx_buf_t *buf;
    // ngx_chain_t *in;
    size_t len;
    ngx_chain_t *input_head;
    ngx_chain_t *input_tail;
    ngx_flag_t done;
    ts_reader_t *reader;
    ts_writer_t *writer;
} ngx_http_ts_ctx_t;

typedef struct {
    uint32_t start;
    uint32_t end;
    const char *class_name;
    size_t class_name_len;
} ts_span_t;

typedef struct {
    ngx_chain_t *chain;
} ts_input_ctx_t;



// function prototypes

static ngx_int_t ngx_http_ts_filter_init(ngx_conf_t *cf);

static ngx_int_t ngx_http_ts_header_filter(ngx_http_request_t *r);

static ngx_int_t ngx_http_ts_body_filter(ngx_http_request_t *r, ngx_chain_t *in);

static void *ngx_http_ts_create_loc_conf(ngx_conf_t *cf);

static char *ngx_http_ts_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);

static ngx_int_t ngx_http_ts_load_language_runtime(ngx_http_request_t *r, ngx_http_ts_loc_conf_t *conf);

static const char *
ts_nginx_read(
    void *payload,
    uint32_t byte_offset,
    TSPoint position,
    uint32_t *bytes_read
);

static ngx_int_t
ngx_http_ts_highlight(
    ngx_http_request_t *r,
    ngx_http_ts_loc_conf_t *conf,
    ts_reader_t *reader,
    ts_writer_t *writer
);

static TSQuery *ngx_ts_load_query(ngx_http_request_t *r, ngx_http_ts_loc_conf_t *conf);

//static const char *ts_capture_to_class(const char *name, uint32_t len);

static ngx_int_t ts_collect_spans(ngx_http_request_t *r, ngx_http_ts_loc_conf_t *conf, TSTree *tree, ngx_array_t *spans);

static int ts_span_cmp(const void *a, const void *b);

static ngx_int_t
ts_render_html(
    ngx_http_request_t *r,
    ts_reader_t *reader,
    ngx_array_t *spans,
    ts_writer_t *writer
);

static ngx_int_t
ts_escape_char(
    ts_writer_t *writer,
    const char c
);

static ngx_int_t
ts_writer_new_buf(
    ts_writer_t *w
);

static ngx_int_t
ts_writer_write(
    ts_writer_t *w,
    const char *data,
    size_t len
);

static ngx_int_t
ts_reader_init(
    // ts_reader_t **reader_ptr,
    ts_reader_t *reader,
    ngx_http_request_t *request,
    ngx_chain_t *chain
);

static ngx_inline u_char
ts_reader_read(
    ts_reader_t *r,
    size_t pos
);

#if DEBUG3
#if false
static void
ts_dump_chain(ngx_chain_t *in, ngx_log_t *log);
#endif
#endif



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



// ================= MODULE =================

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



// ================= CONFIG =================

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



// ================= UTIL =================

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



static ngx_int_t
ts_escape_char(
    ts_writer_t *writer,
    const char c
)
{
    switch (c) {
        case '<':
            ts_writer_write(writer, "&lt;", 4);
            break;

        #if 0
        case '>':
            ts_writer_write(writer, "&gt;", 4);
            break;
        #endif

        case '&':
            ts_writer_write(writer, "&amp;", 5);
            break;

        default:
            ts_writer_write(writer, &c, 1);
            break;
    }

    return NGX_OK;
}



// ================= HEADER FILTER =================

static ngx_int_t
ngx_http_ts_header_filter(ngx_http_request_t *r)
{
    ngx_http_ts_loc_conf_t *conf;
    ngx_http_ts_ctx_t *ctx;

    // disable file-backed buffers for ts_reader_read
    // ensure that b->pos contains actual data
    r->filter_need_in_memory = 1;

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



// ================= BODY FILTER =================

#if 0
static ngx_int_t
ngx_http_ts_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_http_ts_loc_conf_t *conf;
    ngx_http_ts_ctx_t *ctx;
    // ngx_buf_t *b;
    // ngx_buf_t *out;
    // ngx_chain_t *cl;

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

    #if false
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
    #endif

    // syntax highlighting

    // size_t out_len;

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
        // TODO set title?
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
        // TODO allow user to insert custom HTML
        "</head>\n"
        "<body>\n"
        // TODO add conf->language_name as class
        "<pre class=\"code\">"
    );

    const char *suffix = (
        "</pre>\n"
        "</body>\n"
        "</html>\n"
    );

    if (ctx->writer == NULL) {
        ctx->writer = ngx_pcalloc(r->pool, sizeof(*ctx->writer));
        if (ctx->writer == NULL) {
            return NGX_ERROR;
        }
        #if DEBUG3
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
            "ngx_http_ts_body_filter: calling ts_writer_new_buf", &r->uri
        );
        #endif
        ctx->writer->r = r;
        if (ts_writer_new_buf(ctx->writer) != NGX_OK)
            return NGX_ERROR;
    }

    ts_writer_t *writer = ctx->writer;

    #if DEBUG2
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_ts_body_filter: writer=%p writer->r=%p", writer, writer->r);
    #endif

    #if DEBUG
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_ts_body_filter: calling ngx_http_ts_highlight");
    #endif

    #if DEBUG
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_ts_body_filter: copying prefix");
    #endif

    // FIXME write prefix only once
    // body_filter can be called multiple times (?)
    ts_writer_write(writer, prefix, ngx_strlen(prefix));

    // TODO? store reader in ctx->reader
    #if 0
    ts_reader_t reader_storage;
    ts_reader_t *reader = &reader_storage;
    ts_reader_init(reader, in);
    #else
    if (ctx->reader == NULL) {
        if (ts_reader_init(&ctx->reader, r, in) == NGX_ERROR) {
            return NGX_ERROR;
        }
    }
    ts_reader_t *reader = ctx->reader;
    #endif

    #if DEBUG2
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_ts_body_filter: reader=%p reader->r=%p", reader, reader->r);
    #endif

    if (
        conf->language != NULL &&
        // parse input, write html
        ngx_http_ts_highlight(
            r,
            conf,
            reader,
            writer
        ) == NGX_OK
    )
    {
        // highlight done
    } else {
        #if DEBUG
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_ts_body_filter: calling ngx_http_ts_highlight failed");
        #endif

        // highlight failed -> fallback
        size_t pos = reader->pos; // continue reading
        while (pos < reader->size) {
            // ts_escape_char(writer, src[pos++]);
            ts_escape_char(writer, ts_reader_read(reader, pos++));
        }
    }

    #if DEBUG
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_ts_body_filter: copying suffix");
    #endif

    ts_writer_write(writer, suffix, ngx_strlen(suffix));

    #if DEBUG
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_ts_body_filter: done copying");
    #endif

    /*
    ngx_buf_t *out_buf = ngx_calloc_buf(r->pool);
    out_buf->pos = out;
    out_buf->last = out + out_len;
    out_buf->memory = 1;
    out_buf->last_buf = 1;
    */

    // out_buf->memory = 1;
    // out_buf->last_buf = 1;

    #if DEBUG
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_ts_body_filter: creating out_chain");
    #endif

    #if DEBUG3
    // FIXME chain_idx from 0 to 12: empty buffer
    ts_dump_chain(writer->head, r->connection->log);
    size_t chain_idx = 0;
    for (ngx_chain_t *cl = writer->head; cl; cl = cl->next) {
        if (ngx_buf_size(cl->buf) == 0) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                "BUG: zero-size buffer created here at chain_idx=%d", chain_idx);
            ngx_debug_point();
        }
        chain_idx++;
    }
    #endif

    writer->tail->buf->last_buf = 1;

    return ngx_http_next_body_filter(r, writer->head);
}
#else
static ngx_int_t
ngx_http_ts_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_http_ts_loc_conf_t *conf;
    ngx_http_ts_ctx_t *ctx;

    ngx_chain_t *cl;
    ngx_buf_t *b;

    ngx_flag_t last = 0;

    conf = ngx_http_get_module_loc_conf(r, ngx_http_tree_sitter_filter_module);

    if (!conf->enabled || in == NULL) {
        // module is disabled
        return ngx_http_next_body_filter(r, in);
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_tree_sitter_filter_module);

    if (ctx == NULL) {

        ctx = ngx_pcalloc(r->pool, sizeof(*ctx));
        if (ctx == NULL) {
            return NGX_ERROR;
        }

        // set request context
        ngx_http_set_ctx(r, ctx, ngx_http_tree_sitter_filter_module);
    }

    // append incoming chain to buffered input
    for (cl = in; cl; cl = cl->next) {

        b = cl->buf;

        // ignore empty buffers unless they signal EOF
        if (ngx_buf_size(b) == 0 && !b->last_buf) {
            continue;
        }

        // clone chain link
        ngx_chain_t *copy = ngx_alloc_chain_link(r->pool);
        if (copy == NULL) {
            return NGX_ERROR;
        }

        copy->buf = b;
        copy->next = NULL;

        if (ctx->input_tail) {
            ctx->input_tail->next = copy;
        } else {
            ctx->input_head = copy;
        }

        ctx->input_tail = copy;

        if (b->last_buf) {
            last = 1;
        }
    }

    // not final chunk yet
    // wait for more body data
    if (!last) {
        return NGX_OK;
    }

    if (ctx->done) {
        // already processed
        return NGX_OK;
    }

    ctx->done = 1;

    // initialize reader
    ts_reader_t reader_storage;
    ngx_memzero(&reader_storage, sizeof(reader_storage));

    ts_reader_init(&reader_storage, r, ctx->input_head);

    // initialize writer
    ts_writer_t writer_storage;
    ngx_memzero(&writer_storage, sizeof(writer_storage));

    writer_storage.r = r;

    if (ts_writer_new_buf(&writer_storage) != NGX_OK) {
        return NGX_ERROR;
    }

    // html prefix
    static const char *prefix = (
        "<!doctype html>\n"
        "<html>\n"
        "<head>\n"
        "<meta charset=\"utf-8\">\n"
        "<style>\n"
        ".keyword { color: #c00; }\n"
        ".string { color: #080; }\n"
        ".comment { color: #888; }\n"
        ".function { color: #06c; }\n"
        ".type { color: #a0a; }\n"
        "</style>\n"
        "</head>\n"
        "<body>\n"
        "<pre>"
    );

    static const char *suffix = (
        "</pre>\n"
        "</body>\n"
        "</html>\n"
    );

    if (ts_writer_write(&writer_storage, prefix, ngx_strlen(prefix)) != NGX_OK) {
        return NGX_ERROR;
    }

    // run syntax highlighting
    if (
        conf->language != NULL &&
        ngx_http_ts_highlight(r, conf, &reader_storage, &writer_storage) != NGX_OK
    ) {
        // fallback: plain text
        size_t pos = 0;
        while (pos < reader_storage.size) {
            u_char ch = ts_reader_read(&reader_storage, pos++);
            if (ts_escape_char(&writer_storage, ch) != NGX_OK) {
                return NGX_ERROR;
            }
        }
    }

    // html suffix
    if (ts_writer_write(
            &writer_storage,
            suffix,
            ngx_strlen(suffix)
        ) != NGX_OK)
    {
        return NGX_ERROR;
    }

    // mark final output buffer
    if (writer_storage.tail &&
        writer_storage.tail->buf)
    {
        writer_storage.tail->buf->last_buf = 1;
    }

    // update content length
    // (optional: or clear it for chunked encoding)
    r->headers_out.content_length_n = -1;

    // send final generated output
    return ngx_http_next_body_filter(
        r,
        writer_storage.head
    );
}
#endif



static const char *
ts_nginx_read(
    void *payload,
    uint32_t byte_offset,
    TSPoint position,
    uint32_t *bytes_read
)
{
    ts_input_ctx_t *ctx = payload;
    ngx_chain_t *cl;
    ngx_buf_t *b;
    size_t chain_pos = 0;
    for (cl = ctx->chain; cl; cl = cl->next) {
        b = cl->buf;
        // note: ngx_buf_size(b) == (b->last - b->pos)
        size_t buf_size = ngx_buf_size(b);
        if (buf_size == 0) {
            continue;
        }
        size_t next_chain_pos = chain_pos + buf_size;
        // Does this buffer contain byte_offset?
        if (chain_pos <= byte_offset && byte_offset < next_chain_pos)
        {
            size_t local_offset = byte_offset - chain_pos;
            *bytes_read = buf_size - local_offset;
            return (const char *)(b->pos + local_offset);
        }
        chain_pos = next_chain_pos;
    }
    // EOF
    *bytes_read = 0;
    return NULL;
}



static ngx_int_t
ngx_http_ts_highlight(
    ngx_http_request_t *r,
    ngx_http_ts_loc_conf_t *conf,
    ts_reader_t *reader,
    ts_writer_t *writer
)
{
    // ngx_chain_t *cl;
    // ngx_buf_t *b;

    #if DEBUG
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_ts_highlight: hello");
    #endif

    TSParser *parser = ts_parser_new();
    if (parser == NULL) return NGX_ERROR;

    if (!ts_parser_set_language(parser, conf->language)) {
        ts_parser_delete(parser);
        return NGX_ERROR;
    }

    // TODO? use ngx_pcalloc
    ts_input_ctx_t input_ctx_storage;
    ts_input_ctx_t *input_ctx = &input_ctx_storage;
    // ngx_memzero(input_ctx, sizeof(*input_ctx));
    input_ctx->chain = reader->chain;

    TSInput input = {
        .payload = input_ctx,
        .read = ts_nginx_read,
        .encoding = TSInputEncodingUTF8,
    };

    TSTree *tree = ts_parser_parse(
        parser,
        NULL, // old_tree
        input
    );

    if (tree == NULL) {
        ts_parser_delete(parser);
        return NGX_ERROR;
    }



    #if 0
    TSNode root = ts_tree_root_node(tree);

    // For now: fallback = plain escaped text
    // We'll replace this with real highlighting next

    size_t cap = len * 2 + 64;
    u_char *buf = ngx_pnalloc(r->pool, cap);
    if (buf == NULL) return NGX_ERROR;

    u_char *p = buf;

    for (size_t i = 0; i < len; i++) {
        ts_escape_char(writer, src[i]);
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

    ngx_int_t rc = ts_render_html(r, reader, spans, writer);

    // TODO? move up before ts_render_html
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
        s->class_name = cap_name;
        s->class_name_len = cap_name_len;
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
    ts_reader_t *reader,
    ngx_array_t *spans,
    ts_writer_t *writer
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

    // u_char *p = buf;
    size_t pos = 0;
    size_t prev_start = (size_t)(-1);

    #if DEBUG
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ts_render_html: looping spans: n=%d", n);
    #endif

    // ngx_chain_t *cl;
    // ngx_buf_t *b;
    // size_t buf_offset = 0; // buffer position in the buffer chain
    // size_t buf_size = 0;

    // cl = in;
    // b = cl->buf;
    // buf_size = ngx_buf_size(b);

    // // seek to the first non-empty buffer
    // while (buf_size == 0 && cl) {
    //     cl = cl->next;
    //     b = cl->buf;
    //     buf_size = ngx_buf_size(b);
    // }

    // if (cl == NULL) {
    //     // EOF
    //     return NGX_OK;
    // }

    // // TODO: cl = cl->next
    // for (cl = in; cl; cl = cl->next) {
    //     // note: ngx_buf_size(b) == (b->last - b->pos)
    //     // Does this buffer contain byte_offset?
    //     if (chain_pos <= byte_offset && byte_offset < next_chain_pos)
    //     {
    //         size_t local_offset = byte_offset - chain_pos;
    //         *bytes_read = buf_size - local_offset;
    //         return (const char *)(b->pos + local_offset);
    //     }
    //     chain_pos += buf_size;
    // }

    const size_t len = reader->size;

    // loop spans
    for (ngx_uint_t i = 0; i < n; i++) {

        if (s[i].start == prev_start) {
            // dont emit empty span
            continue;
        }

        // emit text before span
        while (pos < s[i].start && pos < len) {
            // ts_escape_char(writer, src[pos++]);
            ts_escape_char(writer, ts_reader_read(reader, pos++));
        }

        #if DEBUG2
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ts_render_html: opening span: start=%d class=%s", s[i].start, s[i].class_name); // debug
        #endif

        // open span
        // NOTE class_name can contain dots like "punctuation.special"
        ts_writer_write(writer, "<span class=\"", 13);
        ts_writer_write(writer, s[i].class_name, s[i].class_name_len);
        ts_writer_write(writer, "\">", 2);

        #if 0
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ts_render_html: looping spans: i=%d class=%s start=%d end=%d len=%d",
            i, s[i].class_name, s[i].start, s[i].end, (s[i].end - s[i].start)
        );
        #endif

        // emit span content
        while (pos < s[i].end && pos < len) {
            // ts_escape_char(writer, src[pos++]);
            ts_escape_char(writer, ts_reader_read(reader, pos++));
        }

        // close
        ts_writer_write(writer, "</span>", 7);

        prev_start = s[i].start;
    }

    #if DEBUG
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ts_render_html: tail");
    #endif

    // tail
    while (pos < len) {
        // ts_escape_char(writer, src[pos++]);
        ts_escape_char(writer, ts_reader_read(reader, pos++));
    }

    return NGX_OK;
}



static ngx_int_t
ts_writer_new_buf(
    ts_writer_t *w
)
{
    // const size_t buf_size = 4096;
    const size_t buf_size = 5349;

    #if DEBUG2
    ngx_log_error(NGX_LOG_ERR, w->r->connection->log, 0, "ts_writer_new_buf: ngx_create_temp_buf"); // debug
    #endif

    ngx_buf_t *b = ngx_create_temp_buf(w->r->pool, buf_size);
    if (!b) return NGX_ERROR;

    b->memory = 1;

    // TODO remove?
    // ensure clean state
    b->pos = b->start;
    b->last = b->start;

    #if DEBUG2
    ngx_log_error(NGX_LOG_ERR, w->r->connection->log, 0, "ts_writer_new_buf: ngx_alloc_chain_link"); // debug
    #endif

    ngx_chain_t *cl = ngx_alloc_chain_link(w->r->pool);
    if (!cl) return NGX_ERROR;

    cl->buf = b;
    cl->next = NULL;

    if (w->tail) {
        #if DEBUG2
        ngx_log_error(NGX_LOG_ERR, w->r->connection->log, 0, "ts_writer_new_buf: w->tail->next = cl;"); // debug
        #endif
        w->tail->next = cl;
    } else {
        #if DEBUG2
        ngx_log_error(NGX_LOG_ERR, w->r->connection->log, 0, "ts_writer_new_buf: w->head = cl;"); // debug
        #endif
        w->head = cl;
    }

    #if DEBUG2
    ngx_log_error(NGX_LOG_ERR, w->r->connection->log, 0, "ts_writer_new_buf: w->tail = cl;"); // debug
    #endif

    w->tail = cl;
    w->buf = b;
    w->capacity = buf_size;

    #if DEBUG2
    ngx_log_error(NGX_LOG_ERR, w->r->connection->log, 0, "ts_writer_new_buf: return NGX_OK;"); // debug
    #endif

    return NGX_OK;
}



static ngx_int_t
ts_writer_write(
    ts_writer_t *w,
    const char *data,
    size_t len
)
{
    #if DEBUG2
    ngx_log_error(NGX_LOG_ERR, w->r->connection->log, 0, "ts_writer_write: hello"); // debug
    #endif

    // FIXME handle len > buf_size

    while (len > 0) {

        #if DEBUG2
        ngx_log_error(NGX_LOG_ERR, w->r->connection->log, 0, "ts_writer_write: w->buf->end=%d w->buf->last=%d", w->buf->end, w->buf->last); // debug
        #endif

        size_t space = w->buf->end - w->buf->last;

        #if DEBUG2
        ngx_log_error(NGX_LOG_ERR, w->r->connection->log, 0, "ts_writer_write: len=%d space=%d", len, space); // debug
        #endif

        // if (space == 0) {
        // MIN_CHUNK = 512 -> avoid pathological fragmentation
        if (space < 512) {
            #if DEBUG3
            ngx_log_error(NGX_LOG_ERR, w->r->connection->log, 0, "ts_writer_write: len=%d space=%d -> calling ts_writer_new_buf", len, space); // debug
            #endif
            if (ts_writer_new_buf(w) != NGX_OK) {
                #if DEBUG3
                ngx_log_error(NGX_LOG_ERR, w->r->connection->log, 0, "ts_writer_write: ts_writer_new_buf failed"); // debug
                #endif
                return NGX_ERROR;
            }
            continue;
        }

        size_t n = (len < space) ? len : space;

        #if DEBUG2
        ngx_log_error(NGX_LOG_ERR, w->r->connection->log, 0, "ts_writer_write: n=%d: ngx_cpymem", n); // debug
        // ngx_log_error(NGX_LOG_ERR, w->r->connection->log, 0, "ts_writer_write: w=%p", w); // debug
        // ngx_log_error(NGX_LOG_ERR, w->r->connection->log, 0, "ts_writer_write: w->buf=%p", w->buf); // debug
        // ngx_log_error(NGX_LOG_ERR, w->r->connection->log, 0, "ts_writer_write: w->buf->last=%p", w->buf->last); // debug
        #endif

        w->buf->last = ngx_cpymem(w->buf->last, data, n);

        #if DEBUG2
        ngx_log_error(NGX_LOG_ERR, w->r->connection->log, 0, "ts_writer_write: ngx_cpymem done"); // debug
        #endif

        data += n;
        len -= n;
    }

    #if DEBUG2
    ngx_log_error(NGX_LOG_ERR, w->r->connection->log, 0, "ts_writer_write: return NGX_OK;"); // debug
    #endif

    return NGX_OK;
}



static ngx_int_t
ts_reader_init(
    // ts_reader_t **reader_ptr,
    ts_reader_t *reader,
    ngx_http_request_t *request,
    ngx_chain_t *chain
)
{
    // *reader_ptr = ngx_pcalloc(request->pool, sizeof(**reader_ptr));
    // if (*reader_ptr == NULL) {
    //     return NGX_ERROR;
    // }
    // ts_reader_t *reader = *reader_ptr;
    reader->r = request;
    reader->chain = chain;
    reader->current = chain;
    if (chain && chain->buf) {
        size_t size = ngx_buf_size(chain->buf);
        reader->current_start = 0;
        reader->current_end = size;
        // reader->size = 0;
        for (ngx_chain_t *cl = chain; cl; cl = cl->next) {
            reader->size += ngx_buf_size(cl->buf);
        }
    }
    return NGX_OK;
}



static ngx_inline u_char
ts_reader_read(
    ts_reader_t *r,
    size_t pos
)
{
    ngx_buf_t *b;
    // Fast path:
    // requested byte still inside current buffer.
    if (r->current &&
        pos >= r->current_start &&
        pos < r->current_end)
    {
        b = r->current->buf;
        r->pos = pos + 1;
        return b->pos[pos - r->current_start];
    }
    // Slow path:
    // walk chain until buffer containing pos.
    ngx_chain_t *cl = r->current;
    size_t start = r->current_start;
    // If seeking backwards,
    // restart from beginning.
    if (pos < start) {
        cl = r->chain;
        start = 0;
    }
    for (; cl; cl = cl->next) {
        b = cl->buf;
        size_t size = ngx_buf_size(b);
        size_t end = start + size;
        if (pos >= start && pos < end) {
            r->current = cl;
            r->current_start = start;
            r->current_end = end;
            r->pos = pos + 1;
            return b->pos[pos - start];
        }
        start = end;
    }
    // Out of range.
    r->pos = pos;
    return '\0';
}



#if DEBUG3
#if false
static void
ts_dump_chain(ngx_chain_t *in, ngx_log_t *log)
{
    size_t i = 0;

    for (ngx_chain_t *cl = in; cl; cl = cl->next, i++) {

        ngx_buf_t *b = cl->buf;

        ngx_log_error(NGX_LOG_ERR, log, 0,
            "chain[%uz]: pos=%p last=%p size=%z file=%d temp=%d last_buf=%d",
            i,
            b->pos,
            b->last,
            ngx_buf_size(b),
            b->in_file,
            b->temporary,
            b->last_buf);
    }
}
#endif
#endif
