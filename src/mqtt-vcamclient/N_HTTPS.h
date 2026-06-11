#ifndef N_HTTPS_H
#define N_HTTPS_H

#include <stddef.h>

int send_https_request(const char *endpoint, const char *method, const char *data, char *response, size_t response_size);

#endif 
