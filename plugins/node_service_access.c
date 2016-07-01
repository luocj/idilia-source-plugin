#include "node_service_access.h"

static gchar * db_entry_id = NULL;

CURL *curl_init(void) {
    return curl_easy_init();
}


gchar *create_json(const gchar *url) {

    json_t *object = json_object();

    json_object_set_new(object, "Source", json_string(url)); 

    return json_dumps(object,JSON_PRESERVE_ORDER);
}


gchar* get_source_id(void) {
    return db_entry_id;
}


void set_source_id(const gchar* ptr) {
    
    json_error_t error;
    json_t * object = NULL;

    object = json_loads(ptr, 0, &error);
 
    db_entry_id = g_strdup(json_string_value(json_object_get(object,"_id")));
 
    json_decref(object);
}


static size_t curl_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    	
    set_source_id(ptr);

    return size*nmemb;
}


gboolean curl_request(CURL *curl_handle,const gchar *url, const gchar *request, const gchar *requestType) {

    CURLcode curl_code = CURLE_OK;
    struct curl_slist *headers = NULL;
    gboolean retValue = TRUE;

    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "charsets: utf-8");
   

    curl_code = curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    if (CURLE_OK != curl_code) {
    	retValue = FALSE;
    }


    curl_code = curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 1L);
    if (CURLE_OK != curl_code) {
	retValue = FALSE;
    }


    curl_code = curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST,requestType);
    if(CURLE_OK != curl_code){
	retValue = FALSE;
    }


    curl_code = curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION,curl_callback);
    if (CURLE_OK != curl_code) {
        retValue = FALSE;
    }


    curl_code = curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
    if (CURLE_OK != curl_code) {
	retValue = FALSE;
    }
    

    curl_code = curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS,create_json(request));
    if (CURLE_OK != curl_code) {
	retValue = FALSE;
    }


    curl_code = curl_easy_perform(curl_handle);
    if(CURLE_OK != curl_code) {
	retValue = FALSE;
    }


    curl_slist_free_all(headers);
    return retValue;  
}


void curl_cleanup(CURL *curl_handle) {
  curl_easy_cleanup(curl_handle);   
}



