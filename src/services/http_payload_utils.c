#include "services/http_payload_utils.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool json_escape_string(const char *input, char *output, size_t output_size) {
  size_t write_index = 0u;
  size_t read_index = 0u;

  if (input == NULL || output == NULL || output_size == 0u) {
    return false;
  }

  while (input[read_index] != '\0') {
    char ch = input[read_index++];
    const char *replacement = NULL;
    char escaped[7];
    size_t replacement_length = 0u;

    switch (ch) {
    case '\\':
      replacement = "\\\\";
      break;
    case '"':
      replacement = "\\\"";
      break;
    case '\n':
      replacement = "\\n";
      break;
    case '\r':
      replacement = "\\r";
      break;
    case '\t':
      replacement = "\\t";
      break;
    default:
      if ((unsigned char)ch < 0x20u) {
        const int written = snprintf(escaped, sizeof(escaped), "\\u%04x",
                                     (unsigned char)ch);
        if (written <= 0 || (size_t)written >= sizeof(escaped)) {
          return false;
        }
        replacement = escaped;
      }
      break;
    }

    if (replacement != NULL) {
      replacement_length = strlen(replacement);
      if (write_index + replacement_length >= output_size) {
        return false;
      }
      memcpy(output + write_index, replacement, replacement_length);
      write_index += replacement_length;
    } else {
      if (write_index + 1u >= output_size) {
        return false;
      }
      output[write_index++] = ch;
    }
  }

  output[write_index] = '\0';
  return true;
}

bool json_extract_int_field(const char *json_body, const char *field_name,
                            int *out_value) {
  char token[32];
  const char *field = NULL;
  const char *colon = NULL;
  char *end_ptr = NULL;
  long parsed_value = 0;

  if (json_body == NULL || field_name == NULL || out_value == NULL) {
    return false;
  }

  if (snprintf(token, sizeof(token), "\"%s\"", field_name) <= 0) {
    return false;
  }

  field = strstr(json_body, token);
  if (field == NULL) {
    return false;
  }

  colon = strchr(field, ':');
  if (colon == NULL) {
    return false;
  }

  colon++;
  while (*colon != '\0' && isspace((unsigned char)*colon)) {
    colon++;
  }

  parsed_value = strtol(colon, &end_ptr, 10);
  if (end_ptr == colon) {
    return false;
  }

  *out_value = (int)parsed_value;
  return true;
}

bool json_extract_float_field(const char *json_body, const char *field_name,
                              float *out_value) {
  char token[32];
  const char *field = NULL;
  const char *colon = NULL;
  char *end_ptr = NULL;
  float parsed_value = 0.0f;

  if (json_body == NULL || field_name == NULL || out_value == NULL) {
    return false;
  }

  if (snprintf(token, sizeof(token), "\"%s\"", field_name) <= 0) {
    return false;
  }

  field = strstr(json_body, token);
  if (field == NULL) {
    return false;
  }

  colon = strchr(field, ':');
  if (colon == NULL) {
    return false;
  }

  colon++;
  while (*colon != '\0' && isspace((unsigned char)*colon)) {
    colon++;
  }

  parsed_value = strtof(colon, &end_ptr);
  if (end_ptr == colon) {
    return false;
  }

  *out_value = parsed_value;
  return true;
}

bool json_extract_bool_field(const char *json_body, const char *field_name,
                             bool *out_value) {
  char token[32];
  const char *field = NULL;
  const char *colon = NULL;

  if (json_body == NULL || field_name == NULL || out_value == NULL) {
    return false;
  }

  if (snprintf(token, sizeof(token), "\"%s\"", field_name) <= 0) {
    return false;
  }

  field = strstr(json_body, token);
  if (field == NULL) {
    return false;
  }

  colon = strchr(field, ':');
  if (colon == NULL) {
    return false;
  }

  colon++;
  while (*colon != '\0' && isspace((unsigned char)*colon)) {
    colon++;
  }

  if (strncmp(colon, "true", 4u) == 0) {
    *out_value = true;
    return true;
  }

  if (strncmp(colon, "false", 5u) == 0) {
    *out_value = false;
    return true;
  }

  if (*colon == '1') {
    *out_value = true;
    return true;
  }

  if (*colon == '0') {
    *out_value = false;
    return true;
  }

  return false;
}

bool json_extract_uint32_field(const char *json_body, const char *field_name,
                               uint32_t *out_value) {
  char token[32];
  const char *field = NULL;
  const char *colon = NULL;
  char *end_ptr = NULL;
  unsigned long parsed_value = 0u;

  if (json_body == NULL || field_name == NULL || out_value == NULL) {
    return false;
  }

  if (snprintf(token, sizeof(token), "\"%s\"", field_name) <= 0) {
    return false;
  }

  field = strstr(json_body, token);
  if (field == NULL) {
    return false;
  }

  colon = strchr(field, ':');
  if (colon == NULL) {
    return false;
  }

  colon++;
  while (*colon != '\0' && isspace((unsigned char)*colon)) {
    colon++;
  }

  if (*colon == '-') {
    return false;
  }

  parsed_value = strtoul(colon, &end_ptr, 10);
  if (end_ptr == colon || parsed_value > 0xfffffffful) {
    return false;
  }

  *out_value = (uint32_t)parsed_value;
  return true;
}

bool json_extract_string_field(const char *json_body, const char *field_name,
                               char *out_value, size_t out_value_size) {
  char token[32];
  const char *field = NULL;
  const char *colon = NULL;
  const char *cursor = NULL;
  size_t write_index = 0u;

  if (json_body == NULL || field_name == NULL || out_value == NULL ||
      out_value_size == 0u) {
    return false;
  }

  if (snprintf(token, sizeof(token), "\"%s\"", field_name) <= 0) {
    return false;
  }

  field = strstr(json_body, token);
  if (field == NULL) {
    return false;
  }

  colon = strchr(field, ':');
  if (colon == NULL) {
    return false;
  }

  cursor = colon + 1;
  while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
    cursor++;
  }

  if (*cursor != '"') {
    return false;
  }
  cursor++;

  while (*cursor != '\0' && *cursor != '"') {
    char ch = *cursor++;
    if (ch == '\\') {
      if (*cursor == '\0') {
        return false;
      }
      ch = *cursor++;
    }

    if (write_index + 1u >= out_value_size) {
      return false;
    }
    out_value[write_index++] = ch;
  }

  if (*cursor != '"') {
    return false;
  }

  out_value[write_index] = '\0';
  return true;
}

static int base64_decode_char(char value) {
  if (value >= 'A' && value <= 'Z') {
    return value - 'A';
  }
  if (value >= 'a' && value <= 'z') {
    return (value - 'a') + 26;
  }
  if (value >= '0' && value <= '9') {
    return (value - '0') + 52;
  }
  if (value == '+') {
    return 62;
  }
  if (value == '/') {
    return 63;
  }
  if (value == '=') {
    return -2;
  }
  return -1;
}

bool base64_decode_payload(const char *input, uint8_t *output,
                           size_t output_capacity, size_t *out_output_length) {
  int values[4];
  size_t output_length = 0u;
  size_t values_count = 0u;
  bool found_padding = false;
  const char *cursor = input;

  if (input == NULL || output == NULL || out_output_length == NULL) {
    return false;
  }

  while (*cursor != '\0') {
    const int decoded = base64_decode_char(*cursor++);
    if (decoded < -1) {
      found_padding = true;
    }
    if (decoded == -1) {
      if (!isspace((unsigned char)cursor[-1])) {
        return false;
      }
      continue;
    }
    if (found_padding && decoded >= 0) {
      return false;
    }

    values[values_count++] = decoded;
    if (values_count < 4u) {
      continue;
    }

    if (values[0] < 0 || values[1] < 0) {
      return false;
    }
    if (output_length + 1u > output_capacity) {
      return false;
    }
    output[output_length++] =
        (uint8_t)((values[0] << 2) | ((values[1] & 0x30) >> 4));

    if (values[2] == -2) {
      if (values[3] != -2) {
        return false;
      }
    } else {
      if (values[2] < 0) {
        return false;
      }
      if (output_length + 1u > output_capacity) {
        return false;
      }
      output[output_length++] =
          (uint8_t)(((values[1] & 0x0f) << 4) | ((values[2] & 0x3c) >> 2));

      if (values[3] != -2) {
        if (values[3] < 0) {
          return false;
        }
        if (output_length + 1u > output_capacity) {
          return false;
        }
        output[output_length++] =
            (uint8_t)(((values[2] & 0x03) << 6) | values[3]);
      }
    }

    values_count = 0u;
  }

  if (values_count != 0u) {
    return false;
  }

  *out_output_length = output_length;
  return true;
}
