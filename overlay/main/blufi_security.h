#pragma once

#include <stdbool.h>
#include <stdint.h>

void blufi_security_negotiate(uint8_t *data, int len, uint8_t **output_data,
                              int *output_len, bool *need_free);
int blufi_security_encrypt(uint8_t iv8, uint8_t *data, int len);
int blufi_security_decrypt(uint8_t iv8, uint8_t *data, int len);
uint16_t blufi_security_checksum(uint8_t iv8, uint8_t *data, int len);
int blufi_security_init(void);
void blufi_security_deinit(void);
