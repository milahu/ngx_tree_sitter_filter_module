#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

typedef struct {
    ngx_flag_t enabled;
} ngx_http_ts_loc_conf_t;

typedef struct {
    ngx_buf_t *buf;
    ngx_chain_t *in;
    size_t len;
    unsigned done:1;
} ngx_http_ts_ctx_t;

static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;

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

static ngx_command_t ngx_http_ts_commands[] = {

    {
        ngx_string("tree_sitter_filter"),
        NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
        ngx_conf_set_flag_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_ts_loc_conf_t, enabled),
        NULL
    },

    ngx_null_command
};

/* ================= UTIL ================= */

static ngx_int_t
ngx_http_ts_should_skip(ngx_http_request_t *r)
{
    u_char *p = r->args.data;
    u_char *last = p + r->args.len;

    while (p < last) {
        if (ngx_strncmp(p, "raw=1", 5) == 0) {
            return 1;
        }

        while (p < last && *p != '&') p++;
        p++;
    }

    return 0;
}

/* ================= HEADER FILTER ================= */

static ngx_int_t
ngx_http_ts_header_filter(ngx_http_request_t *r)
{
    ngx_http_ts_loc_conf_t *conf;
    ngx_http_ts_ctx_t *ctx;

    conf = ngx_http_get_module_loc_conf(r, ngx_http_tree_sitter_filter_module);

    if (!conf->enabled || ngx_http_ts_should_skip(r)) {
        return ngx_http_next_header_filter(r);
    }

    ctx = ngx_pcalloc(r->pool, sizeof(*ctx));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_http_set_ctx(r, ctx, ngx_http_tree_sitter_filter_module);

    r->filter_need_in_memory = 1;
    r->headers_out.content_length_n = -1;

    return ngx_http_next_header_filter(r);
}

/* ================= BODY FILTER ================= */

static ngx_int_t
ngx_http_ts_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_http_ts_ctx_t *ctx;
    ngx_buf_t *b;
    ngx_chain_t *cl;

    ctx = ngx_http_get_module_ctx(r, ngx_http_tree_sitter_filter_module);

    if (ctx == NULL || ngx_http_ts_should_skip(r)) {
        return ngx_http_next_body_filter(r, in);
    }

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

    u_char *all = ngx_pnalloc(r->pool, ctx->len + 1);
    if (all == NULL) return NGX_ERROR;

    u_char *p = all;

    for (cl = ctx->in; cl; cl = cl->next) {
        size_t size = ngx_buf_size(cl->buf);
        p = ngx_cpymem(p, cl->buf->pos, size);
    }

    all[ctx->len] = '\0';

    /* ===== Replace with simple HTML ===== */

    const char *prefix = "<html><body><pre>";
    const char *suffix = "</pre></body></html>";

    size_t out_len = ngx_strlen(prefix) + ctx->len + ngx_strlen(suffix);

    u_char *out = ngx_pnalloc(r->pool, out_len);
    if (out == NULL) return NGX_ERROR;

    u_char *o = out;

    o = ngx_cpymem(o, prefix, ngx_strlen(prefix));
    o = ngx_cpymem(o, all, ctx->len);
    o = ngx_cpymem(o, suffix, ngx_strlen(suffix));

    ngx_buf_t *out_buf = ngx_calloc_buf(r->pool);
    out_buf->pos = out;
    out_buf->last = out + out_len;
    out_buf->memory = 1;
    out_buf->last_buf = 1;

    ngx_chain_t out_chain = { out_buf, NULL };

    r->headers_out.content_type = ngx_string("text/html");
    r->headers_out.content_length_n = out_len;

    return ngx_http_next_body_filter(r, &out_chain);
}

/* ================= INIT ================= */

static ngx_int_t
ngx_http_ts_filter_init(ngx_conf_t *cf)
{
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_ts_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_ts_body_filter;

    return NGX_OK;
}

/* ================= MODULE ================= */

static ngx_http_module_t ngx_http_ts_module_ctx = {
    NULL,
    ngx_http_ts_filter_init,

    NULL, NULL,
    NULL, NULL,
    ngx_http_ts_create_loc_conf,
    ngx_http_ts_merge_loc_conf
};

ngx_module_t ngx_http_tree_sitter_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_ts_module_ctx,
    ngx_http_ts_commands,
    NGX_HTTP_MODULE,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NGX_MODULE_V1_PADDING
};
