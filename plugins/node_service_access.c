#include "node_service_access.h"

CURL *curl_init(void)
{
    return curl_easy_init();
}

gchar *create_json(const gchar *url)
{
    json_t *object = json_object();


    json_object_set_new(object, "Source", json_string(url)); 


    return json_dumps(object,JSON_PRESERVE_ORDER);

}

gboolean curl_request(CURL *curl_handle,const gchar *url, const gchar *request)
{
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


    curl_code = curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10L); 
    if (CURLE_OK != curl_code) {
	retValue = FALSE;
    }


    curl_code = curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST,"POST");
    if(CURLE_OK != curl_code){
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


void curl_cleanup(CURL *curl_handle)
{
  curl_easy_cleanup(curl_handle);   
}



