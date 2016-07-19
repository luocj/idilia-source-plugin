#include <curl/curl.h>
#include <jansson.h>
#include <glib.h>


CURL * curl_init(void);
void curl_cleanup(CURL *curl_handle);

gboolean  curl_request(CURL *curl_handle,const gchar *url, const gchar *request, const gchar*requestType, gchar ** db_entry_ida, gboolean ispost);
