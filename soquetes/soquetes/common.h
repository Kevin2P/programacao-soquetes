/*
 * common.h: constantes compartilhadas por servidor, cliente e testes.
 *
 * Formato de toda mensagem (nos dois sentidos):
 *
 *   +-------------------+----------+----------------------+
 *   | tamanho (4 bytes) | tipo (1) | payload (tamanho B)  |
 *   +-------------------+----------+----------------------+
 *
 *   - tamanho: quantos bytes de payload vem depois do cabecalho de 5 bytes
 *   - tipo:    na requisicao e o comando, na resposta e o status
 *   - numeros de varios bytes viajam em "ordem de rede" (big-endian)
 *   - texto dentro do payload: [comprimento: 2 bytes][bytes], sem '\0'
 *   - valor de recurso: [tipo: 1 byte] e depois um texto (tipo 0) ou um
 *     inteiro de 64 bits com sinal (tipo 1)
 */
#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>

#define PORTA_PADRAO   5000
#define TAM_CABECALHO  5             /* 4 bytes de tamanho + 1 byte de tipo */
#define MAX_PAYLOAD    (128 * 1024)  /* maior payload aceito (128 KB)       */

/* Limites (o servidor valida todos eles) */
#define ID_MAX         32            /* tamanho maximo do id de um cliente  */
#define NOME_MAX       32            /* tamanho maximo do nome do recurso   */
#define VALOR_MAX      256           /* tamanho maximo de um valor texto    */
#define MAX_RECURSOS   256
#define MAX_CLIENTES   256

/* TCP keepalive: detecta a maquina do outro lado que sumiu sem fechar a conexao
 * (queda de energia, cabo ou wi-fi desligado). Depois de KEEPALIVE_IDLE segundos
 * de silencio, uma sonda a cada KEEPALIVE_INTVL segundos; sem resposta a
 * KEEPALIVE_CNT sondas seguidas, a conexao cai (cerca de 60 s no total). Dados
 * enviados e nunca confirmados tambem derrubam a conexao nesse mesmo prazo. */
#ifndef KEEPALIVE_IDLE
#define KEEPALIVE_IDLE   30
#endif
#ifndef KEEPALIVE_INTVL
#define KEEPALIVE_INTVL  10
#endif
#ifndef KEEPALIVE_CNT
#define KEEPALIVE_CNT    3
#endif

/* Tipos de requisicao (cliente -> servidor) */
enum {
    REQ_HELLO   = 1,
    REQ_CREATE  = 2,
    REQ_GET     = 3,
    REQ_SET     = 4,
    REQ_RESERVE = 5,
    REQ_RELEASE = 6,
    REQ_LIST    = 7,
    REQ_QUIT    = 8,
    REQ_STATS   = 9,   /* monitoramento do servidor (bonus) */
    NUM_REQ     = 10   /* tamanho de arrays indexados pelo tipo */
};

/* Status de resposta (servidor -> cliente) */
enum {
    ST_OK             = 0,
    ST_BAD_REQUEST    = 1,
    ST_NOT_IDENTIFIED = 2,
    ST_ID_IN_USE      = 3,
    ST_NOT_FOUND      = 4,
    ST_ALREADY_EXISTS = 5,
    ST_BUSY           = 6,
    ST_NOT_RESERVED   = 7,
    ST_NOT_OWNER      = 8,
    ST_FULL           = 9
};

/* Tipos de valor de um recurso */
enum {
    VAL_STRING = 0,
    VAL_INT    = 1
};

/* Filtro do LIST (1 byte opcional no payload; sem payload = todos) */
enum {
    LISTA_TODOS      = 0,
    LISTA_LIVRES     = 1,   /* somente recursos disponiveis (livres) */
    LISTA_RESERVADOS = 2
};

/* Valor de um recurso: string OU inteiro de 64 bits. */
typedef struct {
    uint8_t tipo;                    /* VAL_STRING ou VAL_INT            */
    int64_t inteiro;                 /* valido quando tipo == VAL_INT    */
    char    texto[VALOR_MAX + 1];    /* valido quando tipo == VAL_STRING */
} Valor;

#endif
