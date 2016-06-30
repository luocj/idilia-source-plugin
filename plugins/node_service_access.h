#include <curl/curl.h>
#include <jansson.h>
#include <glib.h>

gchar *create_json(const gchar *url);

CURL * curl_init(void);
void curl_cleanup(CURL *curl_handle);

gboolean  curl_request(CURL *curl_handle,const gchar *url, const gchar *request);
