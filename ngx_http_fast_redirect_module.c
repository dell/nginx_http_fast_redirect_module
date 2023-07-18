
/*
 * Copyright (C) John De Mott
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#define CSV_STATUS_NORMAL 0
#define CSV_STATUS_ERROR_MAX_LINE 1
#define CSV_STATUS_NOT_ENOUGH_FIELDS 2
#define CSV_STATUS_INVALID_HTTP_CODE 3

#define return_on_error(result_var, X) \
    if ((result_var = X) != 0) { \
        return result_var; \
    }

typedef struct redirect {
    ngx_str_t *src;
    ngx_str_t *dest;
    int code;
    long start_time;
    long end_time;
    long max_age;
    struct redirect *next;
} redirect_t;

typedef struct ngx_http_fast_redirect_store_s {
    char *filename;
    ngx_str_t *name;
    struct redirect **hashmap;
    ngx_pool_t *pool;
    size_t line_count;
    ngx_log_t *log;
    struct ngx_http_fast_redirect_store_s *next;
} ngx_http_fast_redirect_store_t;

static u_char *CSV_IGNORED_CHARS;

typedef struct {
    ngx_http_fast_redirect_store_t *store;
} ngx_http_fast_redirect_loc_conf_t;

typedef struct {
    ngx_str_t time_travel_cookie_name;
    ngx_http_fast_redirect_store_t *head;
} ngx_http_fast_redirect_main_conf_t;

/* Nginx hooks and handlers */
static ngx_int_t ngx_http_fast_redirect_postconf(ngx_conf_t *cf);
static void *ngx_http_fast_redirect_create_main_conf(ngx_conf_t *cf);
static void *ngx_http_fast_redirect_create_loc_conf(ngx_conf_t *cf);
static ngx_int_t ngx_http_fast_redirect_handler(ngx_http_request_t *r);
static char *ngx_http_fast_redirect(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_http_fast_redirect_store(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

/* CSV processing functions */
static int load_redirects_file(ngx_http_fast_redirect_store_t *store);
static size_t count_lines(const char *buffer, size_t buffer_length);
static void next_csv_line(char **cursor);
static ngx_int_t read_csv_line(char **cursor, redirect_t *r, ngx_pool_t *pool, ngx_log_t *log);
static int read_csv_field(char **cursor, char *dest, size_t max_length);
static void * initialize_ignored_chars(ngx_conf_t *cf);

/* Hash table functions */
static unsigned hash(const ngx_str_t *src, size_t hash_size);
static redirect_t *install(redirect_t *redirect, redirect_t **hashmap, size_t hash_size);
static struct redirect *lookup(ngx_str_t *src, struct redirect **hashmap, size_t hash_size);


static ngx_command_t ngx_http_fast_redirect_commands[] = {

    { ngx_string("fast_redirect"), /* directive */
      NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
      ngx_http_fast_redirect, /* configuration setup function */
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fast_redirect_loc_conf_t, store),
      NULL },

    { ngx_string("fast_redirect_store"), /* directive */
      NGX_HTTP_MAIN_CONF | NGX_CONF_2MORE,
      ngx_http_fast_redirect_store, /* configuration setup function */
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },
    { ngx_string("fast_redirect_time_travel_cookie"), /* directive */
      NGX_HTTP_MAIN_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_str_slot, /* configuration setup function */
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_fast_redirect_main_conf_t, time_travel_cookie_name),
      NULL
    },

    ngx_null_command
};


static ngx_http_module_t ngx_http_fast_redirect_module_ctx = {
    NULL,                                    /* preconfiguration */
    ngx_http_fast_redirect_postconf,         /* postconfiguration */

    ngx_http_fast_redirect_create_main_conf, /* create main configuration */
    NULL,                                    /* init main configuration */

    NULL,                                    /* create server configuration */
    NULL,                                    /* merge server configuration */

    ngx_http_fast_redirect_create_loc_conf,  /* create location configuration */
    NULL                                     /* merge location configuration */
};


ngx_module_t ngx_http_fast_redirect_module = {
    NGX_MODULE_V1,
    &ngx_http_fast_redirect_module_ctx,      /* module context */
    ngx_http_fast_redirect_commands,         /* module directives */
    NGX_HTTP_MODULE,                         /* module type */
    NULL,                                    /* init master */
    NULL,                                    /* init module */
    NULL,                                    /* init process */
    NULL,                                    /* init thread */
    NULL,                                    /* exit thread */
    NULL,                                    /* exit process */
    NULL,                                    /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t
ngx_http_fast_redirect_postconf(ngx_conf_t *cf)
{
    ngx_http_handler_pt *h;
    ngx_http_core_main_conf_t *cmcf;
    const ngx_http_fast_redirect_main_conf_t *frcf;

    frcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_fast_redirect_module);

    if (frcf->head == NULL) {
        return NGX_OK;
    }

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_REWRITE_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_fast_redirect_handler;

    return NGX_OK;
}

static void *
ngx_http_fast_redirect_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_fast_redirect_main_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_fast_redirect_main_conf_t));

    if (conf == NULL) {
        return NULL;
    }

    return conf;
}


static void *
ngx_http_fast_redirect_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_fast_redirect_loc_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_fast_redirect_loc_conf_t));

    if (conf == NULL) {
        return NULL;
    }

    conf->store = ngx_pcalloc(cf->pool, sizeof(ngx_http_fast_redirect_store_t));

    if (conf->store == NULL) {
        return NULL;
    }

    conf->store->line_count = 0;

    return conf;
}


/**
 * Content handler.
 *
 * @param r
 *   Pointer to the request structure. See http_request.h.
 * @return
 *   The status of the response generation.
 */
static ngx_int_t
ngx_http_fast_redirect_handler(ngx_http_request_t *r)
{
    ngx_buf_t *b;
    ngx_chain_t out;
    struct redirect *rp;
    ngx_table_elt_t *loc_header;
    ngx_int_t retvalue;
    ngx_http_fast_redirect_loc_conf_t *loc_conf;
    time_t current_time = ngx_time();
    ngx_http_fast_redirect_main_conf_t *main_conf;


    loc_conf = ngx_http_get_module_loc_conf(r, ngx_http_fast_redirect_module);
    main_conf = ngx_http_get_module_main_conf(r, ngx_http_fast_redirect_module);
    ngx_str_t cookie_value;

    if (loc_conf->store->line_count == 0) {
        // If line count is zero there are no redirects for this location.
        return NGX_DECLINED;
    }

    if (main_conf->time_travel_cookie_name.data
       && ngx_http_parse_multi_header_lines(&(r->headers_in.cookies), &main_conf->time_travel_cookie_name, &cookie_value) != NGX_DECLINED) {
            current_time = ngx_atoi(cookie_value.data, cookie_value.len);
    }

    rp = lookup(&r->uri, loc_conf->store->hashmap, loc_conf->store->line_count);

    if (!rp) {
        return NGX_DECLINED;
    }

    if (rp->start_time && rp->start_time > current_time) {
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
            "start time %l greater than current time %l\n", rp->start_time, ngx_time()
        );
        return NGX_DECLINED;
    }

    if (rp->end_time && rp->end_time < current_time) {
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
            "end time %l less than current time %l\n", rp->end_time, ngx_time()
        );
        return NGX_DECLINED;
    }

    /* Allocate a new buffer for sending out the reply. */
    b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));

    /* Insertion in the buffer chain. */
    out.buf = b;
    out.next = NULL; /* just one buffer */

    b->pos = rp->dest->data;                     /* first position in memory of the data */
    b->last = rp->dest->data + rp->dest->len;    /* last position in memory of the data */
    b->memory = 1;                               /* content is in read-only memory */
    b->last_buf = 1;                             /* there will be no more buffers in the request */

    loc_header = ngx_list_push(&r->headers_out.headers);
    ngx_str_set(&loc_header->key, "Location");

    loc_header->value = *rp->dest;
    loc_header->hash = 1;


    if (rp->max_age) {
        loc_header = ngx_list_push(&r->headers_out.headers);
        ngx_str_set(&loc_header->key, "Cache-Control");

        loc_header->value.data = ngx_pnalloc(r->pool,
                                    sizeof("max-age=") + NGX_TIME_T_LEN + 1);

        if (loc_header->value.data == NULL) {
            return NGX_ERROR;
        }

        loc_header->value.len = ngx_sprintf(loc_header->value.data, "max-age=%T", rp->max_age)
                        - loc_header->value.data;

        loc_header->hash = 1;
    }


    /* Sending the headers for the reply. */
    r->headers_out.status = rp->code;
    r->headers_out.location = loc_header;
    ngx_http_send_header(r);

    /* Send the body, and return the status code of the output filter chain. */
    retvalue = ngx_http_output_filter(r, &out);

    return retvalue;
}


static char *
ngx_http_fast_redirect(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t *name;
    ngx_http_fast_redirect_store_t **store;
    ngx_http_fast_redirect_main_conf_t *main_conf;

    char *confp = conf;
    name = cf->args->elts;
    name++;

    store = (ngx_http_fast_redirect_store_t **)(confp + cmd->offset);

    main_conf = ngx_http_conf_get_module_main_conf(cf, ngx_http_fast_redirect_module);

    *store = main_conf->head;
    while (*store) {
        if ((*store)->name->len == name->len && ngx_strncmp((*store)->name->data, name->data, name->len) == 0) {
            return NGX_CONF_OK;
        }
        *store = (*store)->next;
    }

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "No redirect store named '%V' is defined", name);
    return NGX_CONF_ERROR;
}


static char *
ngx_http_fast_redirect_store(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t file, *value;
    ngx_http_fast_redirect_store_t *store;
    ngx_http_fast_redirect_main_conf_t *main_conf;
    main_conf = conf;


    store = ngx_pcalloc(cf->pool, sizeof(ngx_http_fast_redirect_store_t));
    store->pool = cf->pool;

    store->next = main_conf->head;
    main_conf->head = store;

    if (store == NULL) {
        return NULL;
    }

    value = cf->args->elts;

    file.len = 0;
    store->name = ngx_pcalloc(cf->pool, sizeof(ngx_str_t));
    *store->name = value[1];

    for (ngx_uint_t x = 0; x < cf->args->nelts; x++) {
        if (ngx_strncmp(value[x].data, "file=", 5) == 0) {
            file.data = value[x].data + 5;
            file.len = value[x].len - 5;
        }
    }

    if (file.len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "No fast redirect file specified");
        return NGX_CONF_ERROR;
    }

    store->filename = ngx_palloc(store->pool, (file.len + 1) * sizeof(char));
    ngx_memcpy(store->filename, (char *) file.data, file.len * sizeof(char));
    store->filename[file.len] = '\0';
    store->log = cf->log;

    initialize_ignored_chars(cf);

    if (load_redirects_file(store) != 0) {
        return NGX_CONF_ERROR;
    }


    return NGX_CONF_OK;
}


/* CSV processing functions. */

static int
load_redirects_file(ngx_http_fast_redirect_store_t *store)
{
    FILE *fp = fopen(store->filename, "r");

    if (!fp) {
        ngx_log_error(NGX_LOG_EMERG, store->log, 0, "Could not open file %s\n", store->filename);
        return NGX_ERROR;
    }

    char *dest, *p;
    size_t file_size;
    ngx_int_t read_line_status;

    fseek(fp, 0L, SEEK_END);
    file_size = ftell(fp) + 1;
    rewind(fp);

    dest = ngx_palloc(store->pool, file_size);
    if (dest == NULL) {
        ngx_log_error(NGX_LOG_EMERG, store->log, 0, "Could not allocate memory to read CSV file");
        return NGX_ERROR;
    }
    if (fread(dest, sizeof(char), file_size, fp) < file_size - 1) {
        ngx_log_error(NGX_LOG_EMERG, store->log, 0, "Could not fully read CSV file");
        return NGX_ERROR;
    }
    fclose(fp);
    dest[file_size - 1] = '\n';
    p = dest;

    store->line_count = count_lines(p, file_size);

    if (store->line_count < 1) {
        ngx_log_error(NGX_LOG_ALERT, store->log, 0, "Empty CSV file loaded");
        return NGX_OK;
    }

    redirect_t *r;
    redirect_t *redirects = ngx_pcalloc(store->pool, store->line_count * sizeof(redirect_t));

    if (redirects == NULL) {
        ngx_log_error(NGX_LOG_EMERG, store->log, 0, "Could not allocate memory for redirects");
        return NGX_ERROR;
    }

    store->hashmap = ngx_pcalloc(store->pool, store->line_count * sizeof(store->hashmap));
    if (store->hashmap == NULL) {
        ngx_log_error(NGX_LOG_EMERG, store->log, 0, "Could not allocate memory for hashmap");
        return NGX_ERROR;
    }


    // Skip mandatatory header line.
    next_csv_line(&p);

    size_t count = 0;
    while ((size_t) (p - dest) < file_size) {
        r = redirects + count++;
        read_line_status = read_csv_line(&p, r, store->pool, store->log);
        if (read_line_status != CSV_STATUS_NORMAL) {
            switch (read_line_status) {
                case CSV_STATUS_NOT_ENOUGH_FIELDS:
                    ngx_log_error(NGX_LOG_EMERG, store->log, 0, "Invalid CSV - destination field not set on line %l of %s \n", count + 1, store->filename);
                    break;
                case CSV_STATUS_ERROR_MAX_LINE:
                    ngx_log_error(NGX_LOG_EMERG, store->log, 0, "Maximum length of field exceeded on line %l of %s \n", count + 1, store->filename);
                    break;
                default:
                    ngx_log_error(NGX_LOG_EMERG, store->log, 0, "CSV read failed on line %l of %s \n", count + 1, store->filename);

            }
            return NGX_ERROR;
        }

        if (r->src) {
            install(r, store->hashmap, store->line_count);
        }
    }

    return NGX_OK;
}

static size_t
count_lines(const char *buffer, size_t buffer_length) {
    size_t count = 0;
    for (size_t x = 0; x < buffer_length; x++) {
        if (buffer[x] == '\n') {
            count++;
        }
    }
    return count;
}


static void
next_csv_line(char **cursor) {
    while(**cursor != '\n') {
        (*cursor)++;
    }
    (*cursor)++;
}


static ngx_int_t
read_csv_line(char **cursor, redirect_t *r, ngx_pool_t *pool, ngx_log_t *log) {
    char src_field[10000], dest_field[10000], start_time_field[11], end_time_field[11], max_age_field[11], code_field[4];
    unsigned long len, count = 0;
    ngx_int_t result;

    return_on_error(result, read_csv_field(cursor, src_field, sizeof(src_field)))
    return_on_error(result, read_csv_field(cursor, dest_field, sizeof(dest_field)))
    return_on_error(result, read_csv_field(cursor, max_age_field, sizeof(max_age_field)))
    return_on_error(result, read_csv_field(cursor, code_field, sizeof(code_field)))
    return_on_error(result, read_csv_field(cursor, start_time_field, sizeof(start_time_field)))
    return_on_error(result, read_csv_field(cursor, end_time_field, sizeof(end_time_field)))

    if (*src_field) {
        r->src = ngx_pcalloc(pool, sizeof(ngx_str_t));
        len = ngx_strlen(src_field);
        r->src->data = ngx_pcalloc(pool, sizeof(char) * (len + 1));
        r->src->len = len;
        ngx_cpystrn(r->src->data, (u_char *) src_field, len + 1);
    }
    else {
        goto DONE;
    }

    if (*dest_field) {
        r->dest = ngx_palloc(pool, sizeof(ngx_str_t));
        len = ngx_strlen(dest_field);
        r->dest->data = ngx_palloc(pool, sizeof(char) * (len + 1));
        r->dest->len = len;
        ngx_cpystrn(r->dest->data, (u_char *) dest_field, len + 1);
    }
    else {
        return CSV_STATUS_NOT_ENOUGH_FIELDS;
    }

    if (*max_age_field) {
        r->max_age = strtol(max_age_field, NULL, 10);
    }
    else {
        r->max_age = 0;
    }

    if (*code_field) {
        if (code_field[0] < '1' || code_field[0] > '5') {
            ngx_log_error(NGX_LOG_EMERG, log, 0, "Invalid HTTP code \"%s\" on line %lu\n", code_field, count + 1);
            return CSV_STATUS_INVALID_HTTP_CODE;
        }
        r->code = (int) strtol(code_field, NULL, 10);
    }
    else {
        r->code = 302;
    }

    if (*start_time_field) {
        r->start_time = strtol(start_time_field, NULL, 10);
    }
    else {
        r->start_time = 0;
    }

    if (*end_time_field) {
        r->end_time = strtol(end_time_field, NULL, 10);
    }
    else {
        r->end_time = 0;
    }

    count++;

    DONE:
        next_csv_line(cursor);

    return CSV_STATUS_NORMAL;

}


static int
read_csv_field(char **cursor, char *dest, size_t max_length) {
    size_t x = 0, y = 0;
    char *s = *cursor;
    int is_in_quotes = 0;

    *dest = '\0';

    while(is_in_quotes || (s[x] != ',' && s[x] != '\n')) {
        /** Plus one for extra character for string termination. */
        if (y + 1 > max_length) {
            return CSV_STATUS_ERROR_MAX_LINE;
        }
        if (s[x] == '"') {
            is_in_quotes = !is_in_quotes;
            x++;
            continue;
        }
        if (is_in_quotes || !CSV_IGNORED_CHARS[(u_char) s[x]]) {
            dest[y] = s[x];
            y++;
        }
        x++;
    }

    dest[y] = '\0';

    /** Advance to next field, but not to next line */
    if (s[x] == ',') {
        x++;
    }

    *cursor = s + x;

    return CSV_STATUS_NORMAL;
}


/* Hash table functions. */

static unsigned
hash(const ngx_str_t *src, size_t hash_size)
{
    unsigned hashval = 0;
    for (size_t count = 0; count < src->len; count++)
        hashval = src->data[count] + 31 * hashval;

    return hashval % hash_size;
}


static redirect_t *
install(redirect_t *redirect, redirect_t **hashmap, size_t hash_size)
{
    unsigned hashval;

    hashval = hash(redirect->src, hash_size);
    redirect->next = hashmap[hashval];
    hashmap[hashval] = redirect;

    return redirect;
}


static struct redirect *
lookup(ngx_str_t *src, struct redirect **hashmap, size_t hash_size)
{
    unsigned hashval = hash(src, hash_size);

    for (struct redirect *rp = hashmap[hashval]; rp != NULL; rp = rp->next) {
        if (rp->src->len == src->len && strncmp((char *) src->data, (char *) rp->src->data, src->len) == 0)
            return rp;
    }
    return NULL;
}

static void *
initialize_ignored_chars(ngx_conf_t *cf) {
    if (CSV_IGNORED_CHARS == NULL) {
        CSV_IGNORED_CHARS = ngx_pcalloc(cf->pool, sizeof(char) * 256);
        if (CSV_IGNORED_CHARS == NULL) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "No fast redirect file specified");
            return NGX_CONF_ERROR;
        }
        CSV_IGNORED_CHARS[(u_char) '"'] = 1;
        CSV_IGNORED_CHARS[(u_char) '\r'] = 1;
        CSV_IGNORED_CHARS[(u_char) '\t'] = 1;
        CSV_IGNORED_CHARS[(u_char) '\''] = 1;
        CSV_IGNORED_CHARS[(u_char) ' '] = 1;
    }

    return NGX_CONF_OK;
}
