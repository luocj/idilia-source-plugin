#include "node_service_access.h"


CURL *curl_init(void) {
    return curl_easy_init();
}


static size_t curl_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
 
    if(userdata != NULL) { 
	json_error_t error;
        json_t * object = NULL;

        object = json_loads(ptr, 0, &error);

        *((gchar**) userdata) = (gchar *) g_strdup(json_string_value(json_object_get(object,"_id")));
       
        json_decref(object);
    }

    return size*nmemb;
}


gboolean curl_request(CURL *curl_handle,const gchar *url, const gchar *request, const gchar *requestType, gchar ** db_entry_id, gboolean ispost) {

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

    if(ispost == TRUE ) {
	curl_code = curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA,(void *) db_entry_id);
        if (CURLE_OK != curl_code) {
            retValue = FALSE;
        }
    }


    curl_code = curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
    if (CURLE_OK != curl_code) {
	retValue = FALSE;
    }



    json_t *object = json_object();
    json_object_set_new(object, "Source", json_string(request)); 
    gchar *request_str =  json_dumps(object, JSON_PRESERVE_ORDER);


    curl_code = curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS,request_str);
    if (CURLE_OK != curl_code) {
	retValue = FALSE;
    }


    curl_code = curl_easy_perform(curl_handle);
    if(CURLE_OK != curl_code) {
	retValue = FALSE;
    }

    json_decref(object);

    if (request_str != NULL) {
       g_free(request_str);	
    }   


    curl_slist_free_all(headers);
    return retValue;  
}


void curl_cleanup(CURL *curl_handle) {
  curl_easy_cleanup(curl_handle);   
}



