/* Copyright 2002-2004 Justin Erenkrantz and Greg Stein
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <apr_lib.h>
#include <apr_strings.h>
#include <apr_hash.h>

#include "serf.h"
#include "serf_bucket_util.h"


/* the limit on the length of a line in the status-line or headers */
#define LINE_LIMIT 8000

typedef struct {
    serf_bucket_t *stream;

    enum {
        STATE_STATUS_LINE,      /* reading status line */
        STATE_HEADERS,          /* reading headers */
        STATE_BODY              /* reading body */
    } state;

    enum {
        LINE_EMPTY,
        LINE_READY,
        LINE_PARTIAL,
        LINE_CRLF_SPLIT
    } lstate;
    apr_size_t line_used;
    char line[LINE_LIMIT];

    serf_status_line sl;

    int chunked;
    apr_int64_t body_left;
} response_context_t;


SERF_DECLARE(serf_bucket_t *) serf_bucket_response_create(
    serf_bucket_t *stream,
    serf_bucket_alloc_t *allocator)
{
    response_context_t *ctx;

    ctx = serf_bucket_mem_alloc(allocator, sizeof(*ctx));
    ctx->stream = stream;

    return serf_bucket_create(&serf_bucket_type_response, allocator, ctx);
}

SERF_DECLARE(void) serf_response_destroy_and_data(serf_bucket_t *bucket)
{
    response_context_t *ctx = bucket->data;

    if (ctx->state != STATE_STATUS_LINE) {
        const void *md_v;
        serf_bucket_mem_free(bucket->allocator, (void*)ctx->sl.reason);

        serf_bucket_get_metadata(bucket, SERF_RESPONSE_HEADERS, NULL, &md_v);
        if (md_v) {
            apr_hash_t *hash = (apr_hash_t*)md_v;
            apr_hash_index_t *hi;
            apr_pool_t *p;

            p = serf_bucket_allocator_get_pool(bucket->allocator);
            for (hi = apr_hash_first(p, hash); hi; hi = apr_hash_next(hi)) {
                void *key, *val;
                apr_ssize_t key_len;

                apr_hash_this(hi, (const void**)&key, &key_len, &val);
                /* First, remove it. */
                apr_hash_set(hash, key, key_len, NULL);

                serf_bucket_mem_free(bucket->allocator, key);
                serf_bucket_mem_free(bucket->allocator, val);
            }
        }

    }

    serf_bucket_destroy(ctx->stream);
    serf_default_destroy_and_data(bucket);
}

static apr_status_t fetch_line(response_context_t *ctx,
                               serf_bucket_t *bkt)
{
    /* If we had a complete line, then assume the caller has used it, so
     * we can now reset the state.
     */
    if (ctx->lstate == LINE_READY) {
        ctx->lstate = LINE_EMPTY;

        /* Reset the line_used, too, so we don't have to test the state
         * before using this value.
         */
        ctx->line_used = 0;
    }

    while (1) {
        apr_status_t status;
        const char *data;
        apr_size_t len;

        if (ctx->lstate == LINE_CRLF_SPLIT) {
            /* On the previous read, we received just a CR. The LF might
             * be present, but the bucket couldn't see it. We need to
             * examine a single character to determine how to handle the
             * split CRLF.
             */

            status = serf_bucket_peek(bkt, &data, &len);
            if (len > 0) {
                if (*data == '\n') {
                    /* We saw the second part of CRLF. We don't need to
                     * save that character, so do an actual read to suck
                     * up that character.
                     */
                    (void) serf_bucket_read(bkt, 1, &data, &len);
                }
                /* else:
                 *   We got the first character of the next line. Thus,
                 *   the current line is terminated by the CR. Just
                 *   ignore whatever we peeked at. The next reader will
                 *   see it and handle it as appropriate.
                 */

                /* Whatever was read, the line is now ready for use. */
                ctx->lstate = LINE_READY;
            }
            /* else len == 0 */

            /* ### status */
        }
        else {
            int found;

            /* RFC 2616 says that CRLF is the only line ending, but we
             * can easily accept any kind of line ending.
             */
            status = serf_bucket_readline(bkt, SERF_NEWLINE_ANY, &found,
                                          &data, &len);

            /* Typically EAGAIN. */
            if (status) {
                return status;
            }
            if (ctx->line_used + len > sizeof(ctx->line)) {
                /* ### need a "line too long" error */
                return APR_EGENERAL;
            }

            /* Note: our logic doesn't change for LINE_PARTIAL. That only
             * affects how we fill the buffer. It is a communication to our
             * caller on whether the line is ready or not.
             */

            /* If we didn't see a newline, then we should mark the line
             * buffer as partially complete.
             */
            if (found == SERF_NEWLINE_NONE) {
                ctx->lstate = LINE_PARTIAL;
            }
            else if (found == SERF_NEWLINE_CRLF_SPLIT) {
                ctx->lstate = LINE_CRLF_SPLIT;

                /* Toss the partial CR. We won't ever need it. */
                --len;
            }
            else {
                /* We got a newline (of some form). We don't need it
                 * in the line buffer, so back up the length. Then
                 * mark the line as ready.
                 */
                len -= 1 + (found == SERF_NEWLINE_CRLF);

                ctx->lstate = LINE_READY;
            }

            /* ### it would be nice to avoid this copy if at all possible,
               ### and just return the a data/len pair to the caller. we're
               ### keeping it simple for now. */
            memcpy(&ctx->line[ctx->line_used], data, len);
            ctx->line_used += len;
        }

        /* If we saw anything besides "success. please read again", then
         * we should return that status. If the line was completed, then
         * we should also return.
         */
        if (status || ctx->lstate == LINE_READY)
            return status;

        /* We got APR_SUCCESS and the line buffer is not complete. Let's
         * loop to read some more data.
         */
    }
    /* NOTREACHED */
}

static apr_status_t run_machine(serf_bucket_t *bkt, response_context_t *ctx)
{
    apr_status_t status;

    switch (ctx->state) {
    case STATE_STATUS_LINE:
        status = fetch_line(ctx, ctx->stream);
        if (!status) {
            ctx->state = STATE_HEADERS;
        }
        break;
    case STATE_HEADERS:
        do {
            status = fetch_line(ctx, ctx->stream);
            if (status) {
                return status;
            }
            if (ctx->lstate == LINE_READY && ctx->line_used) {
                const char *end_key, *c;
                char *k, *v;

                end_key = c = memchr(ctx->line, ':', ctx->line_used);
                if (!c) {
                    /* Bad headers? */
                    return APR_EGENERAL;
                }

                /* Skip over initial : and spaces. */
                while (apr_isspace(*++c));

                k = serf_bstrmemdup(bkt->allocator, ctx->line,
                                    end_key - ctx->line);
                v = serf_bstrmemdup(bkt->allocator, c,
                                    ctx->line + ctx->line_used - c);

                serf_bucket_set_metadata(bkt, SERF_RESPONSE_HEADERS,
                                         k, v);
            }
        }
        while (!status && ctx->line_used);
        if (!status) {
            const void *v;

            /* Are we C-L, chunked, or conn close? */
            serf_bucket_get_metadata(bkt, SERF_RESPONSE_HEADERS,
                                     "Content-Length", &v);
            if (v) {
                const char *cl = v;
                ctx->chunked = 0;
                ctx->body_left = apr_strtoi64(v, NULL, 10);
                if (errno == ERANGE) {
                    return errno;
                }
            }
            ctx->state = STATE_BODY;
        }
        break;
    case STATE_BODY:
        /* Don't do anything. */
        break;
    default:
        abort();
    }

    return status;
}

static apr_status_t wait_for_sline(serf_bucket_t *bkt, response_context_t *ctx)
{
    /* Keep looping while we're still working on the Status-Line and we
     * don't have any issues reading from the input stream.
     */
    while (ctx->state == STATE_STATUS_LINE) {
        apr_status_t status = run_machine(bkt, ctx);
        if (status) {
            /* we stop at anything.  Most likely, it'll be EAGAIN. */
            return status;
        }
    }

    return APR_SUCCESS;
}

static apr_status_t wait_for_body(serf_bucket_t *bkt, response_context_t *ctx)
{
    /* Keep looping while we're still working on the Status-Line and we
     * don't have any issues reading from the input stream.
     */
    while (ctx->state != STATE_BODY) {
        apr_status_t status = run_machine(bkt, ctx);
        if (status) {
            /* we stop at anything.  Most likely, it'll be EAGAIN. */
            return status;
        }
    }

    return APR_SUCCESS;
}

SERF_DECLARE(apr_status_t) serf_bucket_response_status(
    serf_bucket_t *bkt,
    serf_status_line *sline)
{
    response_context_t *ctx = bkt->data;
    apr_status_t status;
    int res;
    char *reason;

    if (ctx->state != STATE_STATUS_LINE)
    {
        *sline = ctx->sl;
        return APR_SUCCESS;
    }

    if ((status = wait_for_sline(bkt, ctx)) != APR_SUCCESS) {
        return status;
    }

    /* ctx->line should be of form: HTTP/1.1 200 OK */
    res = apr_date_checkmask(ctx->line, "HTTP/#.# ###*");
    if (!res) {
        /* Not an HTTP response?  Well, at least we won't understand it. */
        return APR_EGENERAL;
    }

    ctx->sl.version = SERF_HTTP_VERSION(ctx->line[5] - '0', ctx->line[7] - '0');
    ctx->sl.code = apr_strtoi64(ctx->line + 8, &reason, 10);
    if (apr_isspace(*reason)) {
        reason++;
    }
    /* Should we copy this value? */
    ctx->sl.reason = serf_bstrmemdup(bkt->allocator, reason,
                                     ctx->line_used - (reason - ctx->line));

    *sline = ctx->sl;
    return APR_SUCCESS;
}

static apr_status_t serf_response_read(serf_bucket_t *bucket,
                                       apr_size_t requested,
                                       const char **data, apr_size_t *len)
{
    response_context_t *ctx = bucket->data;
    apr_status_t rv;

    rv = wait_for_body(bucket, ctx);
    if (rv) {
        return rv;
    }

    if (requested > ctx->body_left) {
        requested = ctx->body_left;
    }

    /* Delegate to the stream bucket to do the read. */
    rv = serf_bucket_read(ctx->stream, requested, data, len);
    if (!rv) {
        ctx->body_left -= *len;
        if (!ctx->body_left) {
            rv = APR_EOF;
        }
    }
    return rv;
}

static apr_status_t serf_response_readline(serf_bucket_t *bucket,
                                           int acceptable, int *found,
                                           const char **data, apr_size_t *len)
{
    response_context_t *ctx = bucket->data;
    apr_status_t rv;

    rv = wait_for_body(bucket, ctx);
    if (rv) {
        return rv;
    }

    /* Delegate to the stream bucket to do the readline. */
    return serf_bucket_readline(ctx->stream, acceptable, found, data, len);
}

/* ### need to implement */
#define serf_response_read_iovec NULL
#define serf_response_read_for_sendfile NULL
#define serf_response_peek NULL

SERF_DECLARE_DATA const serf_bucket_type_t serf_bucket_type_response = {
    "RESPONSE",
    serf_response_read,
    serf_response_readline,
    serf_response_read_iovec,
    serf_response_read_for_sendfile,
    serf_default_read_bucket,
    serf_response_peek,
    serf_default_get_metadata,
    serf_default_set_metadata,
    serf_response_destroy_and_data,
};
