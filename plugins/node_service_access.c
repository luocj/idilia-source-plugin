#include "node_service_access.h"

CURL *curl_init(void) {
    return curl_easy_init();
}


static size_t curl_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {

    json_error_t error;
        
    *((json_t **)userdata) = json_loads(ptr, 0, &error);    

    return size*nmemb;
}


gboolean curl_request(CURL *curl_handle,const gchar *url, const gchar *request, const gchar *requestType, json_t ** db_entry_id) {

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

    curl_code = curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
    if (CURLE_OK != curl_code) {
        retValue = FALSE;
    }

    curl_code = curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION,curl_callback);
    if (CURLE_OK != curl_code) {
        retValue = FALSE;
    }

    const gchar *POST = "POST";
    if(g_strcmp0(requestType,POST) == 0 ) {

        curl_code = curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA,(void *) db_entry_id);
            if (CURLE_OK != curl_code) {
                retValue = FALSE;
            }
    }   

    curl_code = curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS,request);
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



