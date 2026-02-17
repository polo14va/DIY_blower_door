#ifndef HTTP_PAYLOAD_UTILS_H
#define HTTP_PAYLOAD_UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool json_escape_string(const char *input, char *output, size_t output_size);
bool json_extract_int_field(const char *json_body, const char *field_name,
                            int *out_value);
bool json_extract_float_field(const char *json_body, const char *field_name,
                              float *out_value);
bool json_extract_bool_field(const char *json_body, const char *field_name,
                             bool *out_value);
bool json_extract_uint32_field(const char *json_body, const char *field_name,
                               uint32_t *out_value);
bool json_extract_string_field(const char *json_body, const char *field_name,
                               char *out_value, size_t out_value_size);
bool base64_decode_payload(const char *input, uint8_t *output,
                           size_t output_capacity, size_t *out_output_length);

#endif
