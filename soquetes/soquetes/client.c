/*
 * client.c: cliente interativo.
 *
 * Voce digita comandos de texto (CREATE x 10). O cliente converte cada um
 * para uma mensagem binaria, envia, e traduz a resposta de volta para texto.
 *
 * Valores: numeros inteiros (ex.: 42, -7) viajam como inteiro de 64 bits;
 * qualquer outro texto viaja como string. Para forcar string, use aspas:
 *   SET codigo "123"
 *
 * Uso: ./client <servidor> <porta> <id>   Ex.: ./client 127.0.0.1 5000 alice
 *      (servidor: IP ou nome da maquina onde o servidor roda)
 *      HEX=1 ./client ...              mostra as mensagens em hexadecimal
 * Para monitorar o servidor a cada segundo:
 *      while true; do echo STATS; sleep 1; done | ./client 127.0.0.1 5000 monitor
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"
#include "net.h"
#include "proto.h"

static int g_hex = 0;   /* HEX=1: imprime as mensagens em hexadecimal */

/* Tabela de comandos: nome digitado e tipo binario. */
static const struct { const char *nome; uint8_t tipo; } CMDS[] = {
    { "CREATE",  REQ_CREATE  },
    { "GET",     REQ_GET     },
    { "SET",     REQ_SET     },
    { "RESERVE", REQ_RESERVE },
    { "RELEASE", REQ_RELEASE },
    { "LIST",    REQ_LIST    },
    { "STATS",   REQ_STATS   },
    { "QUIT",    REQ_QUIT    },
};

/* Envia uma requisicao e le a resposta. Retorna 1 se a resposta chegou, 0 se
 * o servidor fechou a conexao. */
static int requisitar(int fd, uint8_t tipo, const uint8_t *pay, size_t plen,
                      uint8_t *st, uint8_t *resp, uint32_t *resp_len)
{
    if (g_hex)
        dump_msg("  enviei  :", tipo, pay, (uint32_t)plen);
    if (send_msg(fd, tipo, pay, (uint32_t)plen) < 0)
        return 0;
    if (recv_msg(fd, st, resp, MAX_PAYLOAD, resp_len) <= 0)
        return 0;
    if (g_hex)
        dump_msg("  recebi  :", *st, resp, *resp_len);
    return 1;
}

static const char *nome_tipo_valor(const Valor *v)
{
    return v->tipo == VAL_INT ? "inteiro" : "string";
}

/* LIST: [n: 2 bytes] e n x ([nome][valor][reservado: 1 byte][dono]). */
static void mostrar_lista(const uint8_t *resp, uint32_t len)
{
    size_t off = 0;
    uint16_t n;

    if (get_u16(resp, len, &off, &n) < 0) {
        printf("resposta LIST malformada\n");
        return;
    }
    printf("OK %u recurso(s)\n", n);
    for (uint16_t i = 0; i < n; i++) {
        char nome[NOME_MAX + 1], dono[ID_MAX + 1], texto[VALOR_MAX + 32];
        Valor v;
        uint8_t reservado;
        if (get_str(resp, len, &off, nome, sizeof nome) < 0 ||
            get_valor(resp, len, &off, &v) < 0 ||
            get_u8(resp, len, &off, &reservado) < 0 ||
            get_str(resp, len, &off, dono, sizeof dono) < 0) {
            printf("resposta LIST malformada\n");
            return;
        }
        valor_para_texto(&v, texto, sizeof texto);
        if (reservado)
            printf("  %-16s %-8s %-20s [RESERVADO por %s]\n", nome, nome_tipo_valor(&v), texto, dono);
        else
            printf("  %-16s %-8s %-20s [LIVRE]\n", nome, nome_tipo_valor(&v), texto);
    }
}

/* STATS: 8 contadores de 64 bits, [n: 1 byte] e n x ([tipo: 1 byte][contagem: 8 bytes]). */
static void mostrar_stats(const uint8_t *resp, uint32_t len)
{
    size_t off = 0;
    uint64_t c[8];
    uint8_t n;

    for (int i = 0; i < 8; i++)
        if (get_u64(resp, len, &off, &c[i]) < 0) {
            printf("resposta STATS malformada\n");
            return;
        }
    if (get_u8(resp, len, &off, &n) < 0) {
        printf("resposta STATS malformada\n");
        return;
    }
    printf("OK estatisticas do servidor\n");
    printf("  tempo ativo ........ %llu s\n", (unsigned long long)c[0]);
    printf("  conexoes ativas .... %llu (aceitas desde o inicio: %llu)\n",
           (unsigned long long)c[1], (unsigned long long)c[2]);
    printf("  clientes com HELLO . %llu\n", (unsigned long long)c[3]);
    printf("  recursos ........... %llu (reservados: %llu)\n",
           (unsigned long long)c[4], (unsigned long long)c[5]);
    printf("  requisicoes ........ %llu (respostas de erro: %llu)\n",
           (unsigned long long)c[6], (unsigned long long)c[7]);
    printf("  por comando ........");
    for (uint8_t i = 0; i < n; i++) {
        uint8_t tipo;
        uint64_t qtd;
        if (get_u8(resp, len, &off, &tipo) < 0 || get_u64(resp, len, &off, &qtd) < 0) {
            printf("\nresposta STATS malformada\n");
            return;
        }
        printf(" %s=%llu", nome_req(tipo), (unsigned long long)qtd);
    }
    printf("\n");
}

/* Mostra a resposta de forma legivel. */
static void mostrar(uint8_t tipo_req, uint8_t st, const uint8_t *resp, uint32_t len)
{
    char texto[VALOR_MAX + 32];
    size_t off = 0;
    Valor v;

    if (st == ST_OK) {
        if (tipo_req == REQ_LIST) {
            mostrar_lista(resp, len);
        } else if (tipo_req == REQ_STATS) {
            mostrar_stats(resp, len);
        } else if (tipo_req == REQ_GET && get_valor(resp, len, &off, &v) == 0) {
            valor_para_texto(&v, texto, sizeof texto);
            printf("OK %s (%s)\n", texto, nome_tipo_valor(&v));
        } else {
            printf("OK\n");
        }
    } else {
        if (get_str(resp, len, &off, texto, sizeof texto) < 0)
            texto[0] = '\0';
        printf("ERRO %u %s: %s\n", st, nome_status(st), texto);
    }
}

int main(int argc, char **argv)
{
    static uint8_t resp[MAX_PAYLOAD];
    uint8_t pay[1024], st;
    uint32_t rlen;
    size_t off;

    if (argc != 4) {
        fprintf(stderr, "uso: %s <servidor> <porta> <id>\n       servidor: IP ou nome da maquina (ex.: 127.0.0.1)\n", argv[0]);
        return 2;
    }
    g_hex = getenv("HEX") != NULL;

    errno = 0;
    int fd = net_connect(argv[1], argv[2]);       /* aceita IP ou nome da maquina */
    if (fd < 0) {
        fprintf(stderr, "nao foi possivel conectar em %s:%s (%s)\n", argv[1], argv[2],
                errno ? strerror(errno) : "nome ou endereco desconhecido");
        return 1;
    }

    /* Identificacao obrigatoria. */
    off = 0;
    if (put_str(pay, sizeof pay, &off, argv[3]) < 0) {
        fprintf(stderr, "id grande demais\n");
        return 2;
    }
    if (requisitar(fd, REQ_HELLO, pay, off, &st, resp, &rlen) != 1) {
        fprintf(stderr, "falha no HELLO\n");
        return 1;
    }
    mostrar(REQ_HELLO, st, resp, rlen);
    if (st != ST_OK)
        return 1;

    int tty = isatty(STDIN_FILENO);
    if (tty)
        printf("Comandos: CREATE nome valor | GET nome | SET nome valor | RESERVE nome | RELEASE nome\n"
               "          LIST [FREE|RESERVED] | STATS | QUIT\n"
               "Valores: inteiros (42, -7) ou texto; use \"aspas\" para forcar texto.\n");

    char linha[1024];
    for (;;) {
        if (tty) {
            printf("%s> ", argv[3]);
            fflush(stdout);
        }
        if (!fgets(linha, sizeof linha, stdin))
            break;                                    /* Ctrl+D ou fim da entrada */
        linha[strcspn(linha, "\r\n")] = '\0';

        /* Separa: comando, primeiro argumento e o resto da linha. */
        char *p = linha;
        while (*p == ' ') p++;
        if (*p == '\0')
            continue;
        char *cmd = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
        while (*p == ' ') p++;
        char *arg1 = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
        while (*p == ' ') p++;
        char *arg2 = p;                               /* resto da linha = valor */

        int idx = -1;
        for (size_t i = 0; i < sizeof CMDS / sizeof CMDS[0]; i++)
            if (strcasecmp(cmd, CMDS[i].nome) == 0)
                idx = (int)i;
        if (idx < 0) {
            printf("comando desconhecido: %s\n", cmd);
            continue;
        }
        uint8_t tipo = CMDS[idx].tipo;

        /* Monta o payload de acordo com o comando. */
        off = 0;
        int ok = 1;
        Valor v;
        switch (tipo) {
        case REQ_CREATE:
        case REQ_SET:
            if (*arg1 == '\0' || *arg2 == '\0') {
                printf("uso: %s nome valor\n", CMDS[idx].nome);
                continue;
            }
            if (interpretar_valor(arg2, &v) < 0) {
                printf("valor maior que %d caracteres\n", VALOR_MAX);
                continue;
            }
            ok = put_str(pay, sizeof pay, &off, arg1) == 0 && put_valor(pay, sizeof pay, &off, &v) == 0;
            break;
        case REQ_GET:
        case REQ_RESERVE:
        case REQ_RELEASE:
            if (*arg1 == '\0') {
                printf("uso: %s nome\n", CMDS[idx].nome);
                continue;
            }
            ok = put_str(pay, sizeof pay, &off, arg1) == 0;
            break;
        case REQ_LIST:
            if (*arg1 == '\0') {
                /* sem argumento: payload vazio = todos */
            } else if (strcasecmp(arg1, "FREE") == 0 || strcasecmp(arg1, "LIVRES") == 0) {
                ok = put_u8(pay, sizeof pay, &off, LISTA_LIVRES) == 0;
            } else if (strcasecmp(arg1, "RESERVED") == 0 || strcasecmp(arg1, "RESERVADOS") == 0) {
                ok = put_u8(pay, sizeof pay, &off, LISTA_RESERVADOS) == 0;
            } else {
                printf("uso: LIST [FREE|RESERVED]\n");
                continue;
            }
            break;
        default:                                      /* STATS, QUIT: sem payload */
            break;
        }
        if (!ok) {
            printf("argumento grande demais\n");
            continue;
        }

        if (requisitar(fd, tipo, pay, off, &st, resp, &rlen) != 1) {
            fprintf(stderr, "conexao encerrada pelo servidor\n");
            break;
        }
        mostrar(tipo, st, resp, rlen);
        if (tipo == REQ_QUIT)
            break;
    }
    close(fd);
    return 0;
}
