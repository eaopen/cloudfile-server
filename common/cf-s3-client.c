#define _POSIX_C_SOURCE 200809L

/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "common.h"

#include <curl/curl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "cf-s3-client.h"

struct CfS3Client {
    char *endpoint;
    char *bucket;
    char *key_id;
    char *secret_key;
    char *sigv4;
    gboolean path_style;
    long connect_timeout;
    long request_timeout;
    int max_retries;
    GAsyncQueue *handles;
};

typedef struct {
    const char *data;
    size_t len;
    size_t offset;
} MemoryReader;

typedef int (*RetryResetFunc) (void *user_data);

static size_t
discard_data (char *ptr, size_t size, size_t nmemb, void *user_data)
{
    (void)ptr;
    (void)user_data;
    return size * nmemb;
}

static size_t
append_memory (char *ptr, size_t size, size_t nmemb, void *user_data)
{
    GByteArray *buf = user_data;
    size_t len = size * nmemb;

    g_byte_array_append (buf, (guint8 *)ptr, len);
    return len;
}

static size_t
read_memory (char *ptr, size_t size, size_t nmemb, void *user_data)
{
    MemoryReader *reader = user_data;
    size_t capacity = size * nmemb;
    size_t remaining = reader->len - reader->offset;
    size_t len = MIN (capacity, remaining);

    if (len > 0) {
        memcpy (ptr, reader->data + reader->offset, len);
        reader->offset += len;
    }
    return len;
}

static size_t
write_file (char *ptr, size_t size, size_t nmemb, void *user_data)
{
    return fwrite (ptr, size, nmemb, user_data) * size;
}

static size_t
read_file (char *ptr, size_t size, size_t nmemb, void *user_data)
{
    return fread (ptr, size, nmemb, user_data) * size;
}

static CURL *
acquire_handle (CfS3Client *client)
{
    CURL *curl = g_async_queue_try_pop (client->handles);
    if (!curl)
        curl = curl_easy_init ();
    else
        curl_easy_reset (curl);
    return curl;
}

static void
release_handle (CfS3Client *client, CURL *curl)
{
    if (curl)
        g_async_queue_push (client->handles, curl);
}

static char *
xml_unescape (const char *value)
{
    char *ret = g_strdup (value);
    struct {
        const char *encoded;
        const char *plain;
    } replacements[] = {
        { "&amp;", "&" },
        { "&lt;", "<" },
        { "&gt;", ">" },
        { "&quot;", "\"" },
        { "&apos;", "'" },
    };
    size_t i;

    for (i = 0; i < G_N_ELEMENTS (replacements); ++i) {
        char **parts = g_strsplit (ret, replacements[i].encoded, -1);
        char *next = g_strjoinv (replacements[i].plain, parts);
        g_strfreev (parts);
        g_free (ret);
        ret = next;
    }
    return ret;
}

static char *
build_url (CfS3Client *client, const char *key, const char *query)
{
    char *url;

    if (client->path_style) {
        if (key && key[0])
            url = g_strdup_printf ("%s/%s/%s", client->endpoint,
                                   client->bucket, key);
        else
            url = g_strdup_printf ("%s/%s", client->endpoint,
                                   client->bucket);
    } else {
        const char *scheme_end = strstr (client->endpoint, "://");
        if (!scheme_end)
            return NULL;
        char *scheme = g_strndup (client->endpoint,
                                 scheme_end - client->endpoint);
        const char *host = scheme_end + 3;
        if (key && key[0])
            url = g_strdup_printf ("%s://%s.%s/%s", scheme,
                                   client->bucket, host, key);
        else
            url = g_strdup_printf ("%s://%s.%s", scheme,
                                   client->bucket, host);
        g_free (scheme);
    }

    if (query) {
        char *with_query = g_strdup_printf ("%s?%s", url, query);
        g_free (url);
        url = with_query;
    }
    return url;
}

static void
set_common_options (CfS3Client *client, CURL *curl, const char *url)
{
    curl_easy_setopt (curl, CURLOPT_URL, url);
    curl_easy_setopt (curl, CURLOPT_USERNAME, client->key_id);
    curl_easy_setopt (curl, CURLOPT_PASSWORD, client->secret_key);
    curl_easy_setopt (curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_AWS_SIGV4);
    curl_easy_setopt (curl, CURLOPT_AWS_SIGV4, client->sigv4);
    curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT, client->connect_timeout);
    curl_easy_setopt (curl, CURLOPT_TIMEOUT, client->request_timeout);
    curl_easy_setopt (curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt (curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt (curl, CURLOPT_FAILONERROR, 0L);
    curl_easy_setopt (curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, discard_data);
}

static int
reset_byte_array (void *user_data)
{
    g_byte_array_set_size (user_data, 0);
    return 0;
}

static int
reset_memory_reader (void *user_data)
{
    MemoryReader *reader = user_data;
    reader->offset = 0;
    return 0;
}

static int
reset_input_file (void *user_data)
{
    FILE *file = user_data;
    clearerr (file);
    return fseek (file, 0, SEEK_SET);
}

static int
reset_output_file (void *user_data)
{
    FILE *file = user_data;

    clearerr (file);
    if (fflush (file) != 0 || ftruncate (fileno (file), 0) < 0)
        return -1;
    return fseek (file, 0, SEEK_SET);
}

static gboolean
retryable_curl_error (CURLcode rc)
{
    switch (rc) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
    case CURLE_HTTP2:
        return TRUE;
    default:
        return FALSE;
    }
}

static gboolean
retryable_http_status (long status)
{
    return status == 408 || status == 429 ||
        status == 500 || status == 502 ||
        status == 503 || status == 504;
}

static int
perform (CfS3Client *client, CURL *curl,
         const char *operation, const char *key, long *status,
         RetryResetFunc reset, void *reset_data)
{
    CURLcode rc = CURLE_OK;
    int attempt;

    for (attempt = 0; attempt <= client->max_retries; ++attempt) {
        if (attempt > 0) {
            guint delay = (100U << (attempt - 1)) * 1000U;
            delay += g_random_int_range (0, 100) * 1000U;
            g_usleep (delay);
            if (reset && reset (reset_data) < 0)
                return -1;
        }

        *status = 0;
        rc = curl_easy_perform (curl);
        if (rc == CURLE_OK) {
            curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, status);
            if (*status >= 200 && *status < 300)
                return 0;
            if (!retryable_http_status (*status))
                break;
        } else if (!retryable_curl_error (rc)) {
            break;
        }
    }

    if (rc != CURLE_OK)
        seaf_warning ("S3 %s failed for %s after %d attempt(s): %s.\n",
                      operation, key,
                      MIN (attempt + 1, client->max_retries + 1),
                      curl_easy_strerror (rc));
    else if (*status != 404)
        seaf_warning ("S3 %s failed for %s after %d attempt(s) "
                      "with HTTP %ld.\n",
                      operation, key,
                      MIN (attempt + 1, client->max_retries + 1), *status);
    return -1;
}

static gboolean
config_boolean (GKeyFile *config, const char *section,
                const char *key, gboolean default_value,
                gboolean *valid)
{
    GError *error = NULL;
    gboolean value;

    if (!g_key_file_has_key (config, section, key, NULL))
        return default_value;
    value = g_key_file_get_boolean (config, section, key, &error);
    if (error) {
        *valid = FALSE;
        g_error_free (error);
    }
    return value;
}

static long
config_integer (GKeyFile *config, const char *section,
                const char *key, long default_value,
                gboolean *valid)
{
    GError *error = NULL;
    long value;

    if (!g_key_file_has_key (config, section, key, NULL))
        return default_value;
    value = g_key_file_get_integer (config, section, key, &error);
    if (error) {
        *valid = FALSE;
        g_error_free (error);
    }
    return value;
}

CfS3Client *
cf_s3_client_from_config (GKeyFile *config, const char *section)
{
    char *host = NULL;
    char *bucket = NULL;
    char *key_id = NULL;
    char *secret_key = NULL;
    char *region = NULL;
    CfS3Client *client = NULL;
    gboolean valid = TRUE;
    gboolean use_v4;
    gboolean use_https;
    gboolean path_style;
    long connect_timeout;
    long request_timeout;
    long max_retries;

    host = g_key_file_get_string (config, section, "endpoint", NULL);
    if (!host)
        host = g_key_file_get_string (config, section, "host", NULL);
    bucket = g_key_file_get_string (config, section, "bucket", NULL);
    key_id = g_key_file_get_string (config, section, "key_id", NULL);
    secret_key = g_key_file_get_string (config, section, "key", NULL);
    region = g_key_file_get_string (config, section, "aws_region", NULL);
    if (!host || !host[0] || !bucket || !bucket[0] ||
        !key_id || !key_id[0] || !secret_key || !secret_key[0]) {
        seaf_warning ("Incomplete S3 configuration in [%s].\n", section);
        goto out;
    }
    use_v4 = config_boolean (config, section, "use_v4_signature",
                             TRUE, &valid);
    use_https = config_boolean (config, section, "use_https",
                                TRUE, &valid);
    path_style = config_boolean (config, section, "path_style_request",
                                 TRUE, &valid);
    connect_timeout = config_integer (
        config, section, "connection_timeout", 10, &valid);
    request_timeout = config_integer (
        config, section, "request_timeout", 60, &valid);
    max_retries = config_integer (
        config, section, "max_retries", 2, &valid);
    if (!valid) {
        seaf_warning ("Invalid S3 option type in [%s].\n", section);
        goto out;
    }
    if (!use_v4) {
        seaf_warning ("Only S3 Signature V4 is supported in [%s].\n", section);
        goto out;
    }
    if (connect_timeout <= 0 || request_timeout <= 0) {
        seaf_warning ("S3 timeouts must be positive in [%s].\n", section);
        goto out;
    }
    if (max_retries < 0 || max_retries > 10) {
        seaf_warning ("S3 max_retries must be between 0 and 10 in [%s].\n",
                      section);
        goto out;
    }

    client = cf_s3_client_new (
        host, bucket, key_id, secret_key, region,
        use_https, path_style, connect_timeout, request_timeout);
    if (client)
        client->max_retries = max_retries;

out:
    g_free (host);
    g_free (bucket);
    g_free (key_id);
    g_free (secret_key);
    g_free (region);
    return client;
}

CfS3Client *
cf_s3_client_new (const char *host,
                  const char *bucket,
                  const char *key_id,
                  const char *secret_key,
                  const char *region,
                  gboolean use_https,
                  gboolean path_style,
                  long connect_timeout,
                  long request_timeout)
{
    static gsize curl_initialized = 0;
    CfS3Client *client;
    const char *scheme;
    const char *normalized_host;

    if (!host || !host[0] || !bucket || !bucket[0] ||
        !key_id || !key_id[0] || !secret_key || !secret_key[0])
        return NULL;

    if (g_once_init_enter (&curl_initialized)) {
        curl_global_init (CURL_GLOBAL_DEFAULT);
        g_once_init_leave (&curl_initialized, 1);
    }

    scheme = use_https ? "https" : "http";
    normalized_host = host;
    if (g_str_has_prefix (host, "http://")) {
        scheme = "http";
        normalized_host = host + strlen ("http://");
    } else if (g_str_has_prefix (host, "https://")) {
        scheme = "https";
        normalized_host = host + strlen ("https://");
    }
    char *host_copy = g_strdup (normalized_host);
    g_strchomp (host_copy);
    if (g_str_has_suffix (host_copy, "/"))
        host_copy[strlen (host_copy) - 1] = '\0';
    if (!host_copy[0] || strchr (host_copy, '/') ||
        strchr (host_copy, '?') || strchr (host_copy, '#')) {
        seaf_warning ("S3 host must not contain a path.\n");
        g_free (host_copy);
        return NULL;
    }

    client = g_new0 (CfS3Client, 1);
    client->endpoint = g_strdup_printf ("%s://%s", scheme, host_copy);
    g_free (host_copy);
    client->bucket = g_strdup (bucket);
    client->key_id = g_strdup (key_id);
    client->secret_key = g_strdup (secret_key);
    client->sigv4 = g_strdup_printf ("aws:amz:%s:s3",
                                     region && region[0] ? region : "us-east-1");
    client->path_style = path_style;
    client->connect_timeout = connect_timeout > 0 ? connect_timeout : 10;
    client->request_timeout = request_timeout > 0 ? request_timeout : 60;
    client->max_retries = 2;
    client->handles = g_async_queue_new ();
    return client;
}

void
cf_s3_client_free (CfS3Client *client)
{
    CURL *curl;

    if (!client)
        return;
    while ((curl = g_async_queue_try_pop (client->handles)) != NULL)
        curl_easy_cleanup (curl);
    g_async_queue_unref (client->handles);
    g_free (client->endpoint);
    g_free (client->bucket);
    g_free (client->key_id);
    g_free (client->secret_key);
    g_free (client->sigv4);
    g_free (client);
}

int
cf_s3_client_get (CfS3Client *client,
                  const char *key,
                  void **data,
                  size_t *len)
{
    CURL *curl = acquire_handle (client);
    GByteArray *buf = g_byte_array_new ();
    char *url = build_url (client, key, NULL);
    long status = 0;
    int ret;

    set_common_options (client, curl, url);
    curl_easy_setopt (curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, append_memory);
    curl_easy_setopt (curl, CURLOPT_WRITEDATA, buf);
    ret = perform (client, curl, "GET", key, &status,
                   reset_byte_array, buf);
    if (ret == 0) {
        *len = buf->len;
        *data = g_byte_array_free (buf, FALSE);
    } else {
        g_byte_array_free (buf, TRUE);
    }

    g_free (url);
    release_handle (client, curl);
    return ret;
}

int
cf_s3_client_get_file (CfS3Client *client,
                       const char *key,
                       FILE *file)
{
    CURL *curl = acquire_handle (client);
    char *url = build_url (client, key, NULL);
    long status = 0;
    int ret;

    set_common_options (client, curl, url);
    curl_easy_setopt (curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt (curl, CURLOPT_WRITEDATA, file);
    ret = perform (client, curl, "GET", key, &status,
                   reset_output_file, file);

    g_free (url);
    release_handle (client, curl);
    return ret;
}

int
cf_s3_client_put (CfS3Client *client,
                  const char *key,
                  const void *data,
                  size_t len)
{
    CURL *curl = acquire_handle (client);
    MemoryReader reader = { data, len, 0 };
    char *url = build_url (client, key, NULL);
    long status = 0;
    int ret;

    set_common_options (client, curl, url);
    curl_easy_setopt (curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt (curl, CURLOPT_READFUNCTION, read_memory);
    curl_easy_setopt (curl, CURLOPT_READDATA, &reader);
    curl_easy_setopt (curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)len);
    ret = perform (client, curl, "PUT", key, &status,
                   reset_memory_reader, &reader);

    g_free (url);
    release_handle (client, curl);
    return ret;
}

int
cf_s3_client_put_file (CfS3Client *client,
                       const char *key,
                       FILE *file,
                       guint64 len)
{
    CURL *curl = acquire_handle (client);
    char *url = build_url (client, key, NULL);
    long status = 0;
    int ret;

    rewind (file);
    set_common_options (client, curl, url);
    curl_easy_setopt (curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt (curl, CURLOPT_READFUNCTION, read_file);
    curl_easy_setopt (curl, CURLOPT_READDATA, file);
    curl_easy_setopt (curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)len);
    ret = perform (client, curl, "PUT", key, &status,
                   reset_input_file, file);

    g_free (url);
    release_handle (client, curl);
    return ret;
}

static int
check_bucket_access (CfS3Client *client)
{
    CURL *curl = acquire_handle (client);
    char *url = build_url (client, NULL, "list-type=2&max-keys=1");
    long status = 0;
    int ret;

    set_common_options (client, curl, url);
    curl_easy_setopt (curl, CURLOPT_HTTPGET, 1L);
    ret = perform (client, curl, "CHECK-BUCKET", client->bucket,
                   &status, NULL, NULL);
    g_free (url);
    release_handle (client, curl);
    return ret;
}

int
cf_s3_client_head (CfS3Client *client,
                   const char *key,
                   guint64 *size)
{
    CURL *curl = acquire_handle (client);
    char *url = build_url (client, key, NULL);
    long status = 0;
    int ret;

    set_common_options (client, curl, url);
    curl_easy_setopt (curl, CURLOPT_NOBODY, 1L);
    ret = perform (client, curl, "HEAD", key, &status, NULL, NULL);
    if (status == 404)
        ret = check_bucket_access (client);
    else if (ret == 0 && size) {
        curl_off_t content_length = 0;
        if (curl_easy_getinfo (curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,
                               &content_length) != CURLE_OK)
            ret = -1;
        else
            *size = (guint64)content_length;
    }

    g_free (url);
    release_handle (client, curl);
    if (status == 404)
        return ret == 0 ? 0 : -1;
    return ret == 0 ? 1 : -1;
}

int
cf_s3_client_delete (CfS3Client *client, const char *key)
{
    CURL *curl = acquire_handle (client);
    char *url = build_url (client, key, NULL);
    long status = 0;
    int ret;

    set_common_options (client, curl, url);
    curl_easy_setopt (curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    ret = perform (client, curl, "DELETE", key, &status, NULL, NULL);
    if (status == 404)
        ret = check_bucket_access (client);

    g_free (url);
    release_handle (client, curl);
    return ret;
}

int
cf_s3_client_list (CfS3Client *client,
                   const char *prefix,
                   CfS3ListFunc process,
                   void *user_data)
{
    char *continuation = NULL;
    int ret = 0;

    do {
        CURL *curl = acquire_handle (client);
        char *escaped_prefix = curl_easy_escape (curl, prefix, 0);
        char *escaped_token = continuation ?
            curl_easy_escape (curl, continuation, 0) : NULL;
        char *query = escaped_token ?
            g_strdup_printf ("list-type=2&prefix=%s&continuation-token=%s",
                             escaped_prefix, escaped_token) :
            g_strdup_printf ("list-type=2&prefix=%s", escaped_prefix);
        char *url = build_url (client, NULL, query);
        GByteArray *buf = g_byte_array_new ();
        long status = 0;

        set_common_options (client, curl, url);
        curl_easy_setopt (curl, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, append_memory);
        curl_easy_setopt (curl, CURLOPT_WRITEDATA, buf);
        if (perform (client, curl, "LIST", prefix, &status,
                     reset_byte_array, buf) < 0) {
            ret = -1;
        } else {
            g_byte_array_append (buf, (const guint8 *)"", 1);
            GRegex *entry_re = g_regex_new (
                "<Contents>.*?<Key>([^<]+)</Key>.*?<Size>([0-9]+)</Size>.*?</Contents>",
                G_REGEX_DOTALL, 0, NULL);
            GMatchInfo *match = NULL;
            g_regex_match (entry_re, (char *)buf->data, 0, &match);
            while (g_match_info_matches (match)) {
                char *encoded_key = g_match_info_fetch (match, 1);
                char *size_text = g_match_info_fetch (match, 2);
                char *key = xml_unescape (encoded_key);
                guint64 size = g_ascii_strtoull (size_text, NULL, 10);
                if (!process (key, size, user_data)) {
                    ret = 1;
                    g_free (key);
                    g_free (encoded_key);
                    g_free (size_text);
                    break;
                }
                g_free (key);
                g_free (encoded_key);
                g_free (size_text);
                g_match_info_next (match, NULL);
            }
            g_match_info_free (match);
            g_regex_unref (entry_re);

            g_free (continuation);
            continuation = NULL;
            if (ret == 0) {
                GRegex *token_re = g_regex_new (
                    "<NextContinuationToken>([^<]+)</NextContinuationToken>",
                    0, 0, NULL);
                GMatchInfo *token_match = NULL;
                if (g_regex_match (token_re, (char *)buf->data, 0,
                                   &token_match)) {
                    char *encoded = g_match_info_fetch (token_match, 1);
                    continuation = xml_unescape (encoded);
                    g_free (encoded);
                }
                g_match_info_free (token_match);
                g_regex_unref (token_re);
            }
        }

        curl_free (escaped_prefix);
        curl_free (escaped_token);
        g_free (query);
        g_free (url);
        g_byte_array_free (buf, TRUE);
        release_handle (client, curl);
    } while (ret == 0 && continuation);

    g_free (continuation);
    return ret < 0 ? -1 : 0;
}
