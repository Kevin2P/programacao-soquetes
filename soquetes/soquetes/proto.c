/*
 * proto.c: funcoes auxiliares do protocolo.
 *
 * REGRA DE OURO: tudo que vem da rede pode estar errado ou ser malicioso.
 * Todas as funcoes get_* conferem os limites antes de ler qualquer byte.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "proto.h"

int put_str(uint8_t *buf, size_t cap, size_t *off, const char *s)
{
    size_t n = strlen(s);
    if (n > 0xFFFF || *off + 2 + n > cap)
        return -1;
    uint16_t n_rede = htons((uint16_t)n);
    memcpy(buf + *off, &n_rede, 2);
    memcpy(buf + *off + 2, s, n);
    *off += 2 + n;
    return 0;
}

int get_str(const uint8_t *buf, size_t len, size_t *off, char *out, size_t outcap)
{
    if (*off + 2 > len)
        return -1;                          /* nem o comprimento cabe */
    uint16_t n_rede;
    memcpy(&n_rede, buf + *off, 2);
    size_t n = ntohs(n_rede);
    if (*off + 2 + n > len)
        return -1;                          /* o texto passa do fim do payload */
    if (n + 1 > outcap)
        return -1;                          /* nao cabe no destino */
    if (memchr(buf + *off + 2, '\0', n))
        return -1;                          /* '\0' embutido */
    memcpy(out, buf + *off + 2, n);
    out[n] = '\0';
    *off += 2 + n;
    return 0;
}

int put_u8(uint8_t *buf, size_t cap, size_t *off, uint8_t v)
{
    if (*off + 1 > cap)
        return -1;
    buf[(*off)++] = v;
    return 0;
}

int put_u16(uint8_t *buf, size_t cap, size_t *off, uint16_t v)
{
    if (*off + 2 > cap)
        return -1;
    buf[(*off)++] = (uint8_t)(v >> 8);
    buf[(*off)++] = (uint8_t)v;
    return 0;
}

int put_u64(uint8_t *buf, size_t cap, size_t *off, uint64_t v)
{
    if (*off + 8 > cap)
        return -1;
    for (int shift = 56; shift >= 0; shift -= 8)
        buf[(*off)++] = (uint8_t)(v >> shift);   /* byte mais significativo primeiro */
    return 0;
}

int put_i64(uint8_t *buf, size_t cap, size_t *off, int64_t v)
{
    return put_u64(buf, cap, off, (uint64_t)v);
}

int get_u8(const uint8_t *buf, size_t len, size_t *off, uint8_t *v)
{
    if (*off + 1 > len)
        return -1;
    *v = buf[(*off)++];
    return 0;
}

int get_u16(const uint8_t *buf, size_t len, size_t *off, uint16_t *v)
{
    if (*off + 2 > len)
        return -1;
    *v = (uint16_t)((buf[*off] << 8) | buf[*off + 1]);
    *off += 2;
    return 0;
}

int get_u64(const uint8_t *buf, size_t len, size_t *off, uint64_t *v)
{
    if (*off + 8 > len)
        return -1;
    uint64_t r = 0;
    for (int i = 0; i < 8; i++)
        r = (r << 8) | buf[*off + (size_t)i];
    *v = r;
    *off += 8;
    return 0;
}

int get_i64(const uint8_t *buf, size_t len, size_t *off, int64_t *v)
{
    uint64_t u;
    if (get_u64(buf, len, off, &u) < 0)
        return -1;
    *v = (int64_t)u;
    return 0;
}

int put_valor(uint8_t *buf, size_t cap, size_t *off, const Valor *v)
{
    if (put_u8(buf, cap, off, v->tipo) < 0)
        return -1;
    if (v->tipo == VAL_INT)
        return put_i64(buf, cap, off, v->inteiro);
    return put_str(buf, cap, off, v->texto);
}

int get_valor(const uint8_t *buf, size_t len, size_t *off, Valor *v)
{
    uint8_t t;
    memset(v, 0, sizeof *v);
    if (get_u8(buf, len, off, &t) < 0)
        return -1;
    if (t == VAL_INT) {
        v->tipo = VAL_INT;
        return get_i64(buf, len, off, &v->inteiro);
    }
    if (t == VAL_STRING) {
        v->tipo = VAL_STRING;
        return get_str(buf, len, off, v->texto, sizeof v->texto);
    }
    return -1;                               /* tipo de valor desconhecido */
}

/* Inteiro na forma canonica: "0", "42", "-7". Rejeita "007", "-0", "+5", "1e3". */
static int inteiro_canonico(const char *s, int64_t *out)
{
    const char *p = s;
    int negativo = 0;

    if (*p == '-') {
        negativo = 1;
        p++;
    }
    if (!isdigit((unsigned char)*p))
        return 0;
    if (*p == '0' && (p[1] != '\0' || negativo))
        return 0;
    size_t digitos = strlen(p);
    if (digitos > 19)
        return 0;
    for (const char *q = p; *q; q++)
        if (!isdigit((unsigned char)*q))
            return 0;

    char *fim;
    errno = 0;
    long long v = strtoll(s, &fim, 10);
    if (errno == ERANGE || *fim != '\0')
        return 0;                            /* nao cabe em 64 bits */
    *out = (int64_t)v;
    return 1;
}

int interpretar_valor(const char *entrada, Valor *v)
{
    size_t n = strlen(entrada);
    memset(v, 0, sizeof *v);

    if (n >= 2 && entrada[0] == '"' && entrada[n - 1] == '"') {   /* "texto" forca string */
        if (n - 2 > VALOR_MAX)
            return -1;
        v->tipo = VAL_STRING;
        memcpy(v->texto, entrada + 1, n - 2);
        return 0;
    }
    if (inteiro_canonico(entrada, &v->inteiro)) {
        v->tipo = VAL_INT;
        return 0;
    }
    if (n > VALOR_MAX)
        return -1;
    v->tipo = VAL_STRING;
    memcpy(v->texto, entrada, n);
    return 0;
}

void valor_para_texto(const Valor *v, char *out, size_t sz)
{
    if (v->tipo == VAL_INT)
        snprintf(out, sz, "%lld", (long long)v->inteiro);
    else
        snprintf(out, sz, "%s", v->texto);
}

int nome_valido(const char *s, size_t max)
{
    size_t n = strlen(s);
    if (n == 0 || n > max)
        return 0;
    for (; *s; s++)
        if (!(isalnum((unsigned char)*s) || *s == '_' || *s == '-' || *s == '.'))
            return 0;
    return 1;
}

const char *nome_status(int st)
{
    switch (st) {
    case ST_OK:             return "OK";
    case ST_BAD_REQUEST:    return "BAD_REQUEST";
    case ST_NOT_IDENTIFIED: return "NOT_IDENTIFIED";
    case ST_ID_IN_USE:      return "ID_IN_USE";
    case ST_NOT_FOUND:      return "NOT_FOUND";
    case ST_ALREADY_EXISTS: return "ALREADY_EXISTS";
    case ST_BUSY:           return "BUSY";
    case ST_NOT_RESERVED:   return "NOT_RESERVED";
    case ST_NOT_OWNER:      return "NOT_OWNER";
    case ST_FULL:           return "FULL";
    default:                return "DESCONHECIDO";
    }
}

const char *mensagem_status(int st)
{
    switch (st) {
    case ST_OK:             return "ok";
    case ST_BAD_REQUEST:    return "requisicao invalida ou malformada";
    case ST_NOT_IDENTIFIED: return "envie HELLO <id> primeiro";
    case ST_ID_IN_USE:      return "ja existe um cliente conectado com esse id";
    case ST_NOT_FOUND:      return "recurso nao existe";
    case ST_ALREADY_EXISTS: return "recurso ja existe";
    case ST_BUSY:           return "recurso reservado por outro cliente";
    case ST_NOT_RESERVED:   return "recurso nao esta reservado";
    case ST_NOT_OWNER:      return "a reserva pertence a outro cliente";
    case ST_FULL:           return "limite do servidor atingido";
    default:                return "erro desconhecido";
    }
}

const char *nome_req(int tipo)
{
    switch (tipo) {
    case REQ_HELLO:   return "HELLO";
    case REQ_CREATE:  return "CREATE";
    case REQ_GET:     return "GET";
    case REQ_SET:     return "SET";
    case REQ_RESERVE: return "RESERVE";
    case REQ_RELEASE: return "RELEASE";
    case REQ_LIST:    return "LIST";
    case REQ_QUIT:    return "QUIT";
    case REQ_STATS:   return "STATS";
    default:          return "?";
    }
}
