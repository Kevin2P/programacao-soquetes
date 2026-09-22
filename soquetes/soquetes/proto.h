/*
 * proto.h: codificacao e decodificacao dos campos do payload.
 *
 * Convencao: funcoes put_* escrevem em buf (que tem 'cap' bytes) a partir da
 * posicao *off, que avanca; funcoes get_* leem de buf (que tem 'len' bytes
 * validos). Todas devolvem 0 em sucesso e -1 se nao couber ou se a mensagem
 * estiver truncada ou malformada.
 */
#ifndef PROTO_H
#define PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "common.h"

/* Texto: [comprimento: 2 bytes][bytes], sem '\0'. get_str rejeita '\0' embutido. */
int put_str(uint8_t *buf, size_t cap, size_t *off, const char *s);
int get_str(const uint8_t *buf, size_t len, size_t *off, char *out, size_t outcap);

/* Inteiros em ordem de rede (big-endian). */
int put_u8(uint8_t *buf, size_t cap, size_t *off, uint8_t v);
int put_u16(uint8_t *buf, size_t cap, size_t *off, uint16_t v);
int put_u64(uint8_t *buf, size_t cap, size_t *off, uint64_t v);
int put_i64(uint8_t *buf, size_t cap, size_t *off, int64_t v);
int get_u8(const uint8_t *buf, size_t len, size_t *off, uint8_t *v);
int get_u16(const uint8_t *buf, size_t len, size_t *off, uint16_t *v);
int get_u64(const uint8_t *buf, size_t len, size_t *off, uint64_t *v);
int get_i64(const uint8_t *buf, size_t len, size_t *off, int64_t *v);

/* Valor de recurso: [tipo: 1 byte] + (texto | inteiro de 64 bits). */
int put_valor(uint8_t *buf, size_t cap, size_t *off, const Valor *v);
int get_valor(const uint8_t *buf, size_t len, size_t *off, Valor *v);

/* Converte o texto digitado pelo usuario em Valor:
 *   "entre aspas"        -> string (aspas removidas)
 *   -12, 0, 42           -> inteiro (somente forma canonica: sem zeros a esquerda)
 *   qualquer outra coisa -> string
 * Retorna 0, ou -1 se o texto for maior que VALOR_MAX. */
int interpretar_valor(const char *entrada, Valor *v);

/* Escreve o valor em forma legivel (inteiro em decimal, ou o proprio texto). */
void valor_para_texto(const Valor *v, char *out, size_t sz);

/* Nome ou id valido: de 1 a max caracteres entre [A-Za-z0-9_.-]. */
int nome_valido(const char *s, size_t max);

/* Nomes para exibicao e mensagens de erro para cada status. */
const char *nome_status(int st);
const char *mensagem_status(int st);
const char *nome_req(int tipo);

#endif
