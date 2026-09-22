/*
 * net.c: leitura e escrita de mensagens sobre TCP.
 *
 * Ideia central: TCP e um fluxo de bytes, sem marcar onde uma mensagem acaba.
 * Um read() pode devolver MENOS bytes do que voce pediu. Por isso precisamos
 * de um laco que repete ate completar (read_full / write_full).
 */
#include <arpa/inet.h>   /* htonl, ntohl */
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"
#include "net.h"

int read_full(int fd, void *buf, size_t n)
{
    size_t lidos = 0;
    while (lidos < n) {
        ssize_t r = read(fd, (char *)buf + lidos, n - lidos);
        if (r == 0)
            return 0;                 /* EOF: o outro lado fechou */
        if (r < 0) {
            if (errno == EINTR)
                continue;             /* interrompido por sinal: tenta de novo */
            return -1;
        }
        lidos += (size_t)r;
    }
    return 1;
}

int write_full(int fd, const void *buf, size_t n)
{
    size_t enviados = 0;
    while (enviados < n) {
        /* MSG_NOSIGNAL: se o outro lado ja fechou, send() devolve erro em vez
         * de matar o processo com SIGPIPE. */
        ssize_t w = send(fd, (const char *)buf + enviados, n - enviados, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        enviados += (size_t)w;
    }
    return 0;
}

int send_msg(int fd, uint8_t tipo, const void *payload, uint32_t len)
{
    if (len > MAX_PAYLOAD)
        return -1;

    /* Monta a mensagem inteira num unico buffer e envia de uma vez.
     * (Duas escritas pequenas seguidas podem sofrer atraso de ~40 ms por
     * causa do algoritmo de Nagle.) */
    static __thread uint8_t quadro[TAM_CABECALHO + MAX_PAYLOAD];

    uint32_t len_rede = htonl(len);   /* host -> ordem de rede */
    memcpy(quadro, &len_rede, 4);
    quadro[4] = tipo;
    if (len > 0)
        memcpy(quadro + TAM_CABECALHO, payload, len);

    return write_full(fd, quadro, TAM_CABECALHO + len);
}

int recv_msg(int fd, uint8_t *tipo, void *buf, size_t cap, uint32_t *len)
{
    uint8_t cab[TAM_CABECALHO];

    int r = read_full(fd, cab, TAM_CABECALHO);   /* 1) le o cabecalho */
    if (r <= 0)
        return r;

    uint32_t len_rede;
    memcpy(&len_rede, cab, 4);
    uint32_t n = ntohl(len_rede);                /* ordem de rede -> host */

    /* 2) valida ANTES de ler o payload: nunca confie no tamanho vindo da rede */
    if (n > MAX_PAYLOAD || n > cap)
        return -2;

    *tipo = cab[4];
    *len = n;
    if (n > 0) {
        r = read_full(fd, buf, n);               /* 3) le o payload */
        if (r <= 0)
            return r;
    }
    return 1;
}

void dump_msg(const char *prefixo, uint8_t tipo, const void *payload, uint32_t len)
{
    uint32_t len_rede = htonl(len);
    const uint8_t *h = (const uint8_t *)&len_rede;
    const uint8_t *p = payload;

    printf("%s tamanho=%02x %02x %02x %02x | tipo=%02x |", prefixo, h[0], h[1], h[2], h[3], tipo);
    for (uint32_t i = 0; i < len; i++)
        printf(" %02x", p[i]);
    printf("\n");
}

int net_connect(const char *host, const char *porta)
{
    struct addrinfo dicas, *res, *rp;
    int fd = -1;

    memset(&dicas, 0, sizeof dicas);
    dicas.ai_family   = AF_UNSPEC;
    dicas.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, porta, &dicas, &res) != 0)
        return -1;

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd >= 0) {
        int um = 1;   /* mensagens pequenas: nao esperar o algoritmo de Nagle */
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &um, sizeof um);
        net_keepalive(fd);
    }
    return fd;
}

void net_keepalive(int fd)
{
    int um = 1, idle = KEEPALIVE_IDLE, intvl = KEEPALIVE_INTVL, cnt = KEEPALIVE_CNT;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &um, sizeof um);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
#ifdef TCP_USER_TIMEOUT
    /* O keepalive so vale para conexao parada. Se a maquina sumir logo depois de
     * enviarmos dados, eles ficam sem confirmacao e o TCP so desistiria depois de
     * ~15 minutos. Este limite (ms) fecha a conexao no mesmo prazo do keepalive. */
    unsigned int limite_ms = (unsigned int)(KEEPALIVE_IDLE + KEEPALIVE_INTVL * KEEPALIVE_CNT) * 1000u;
    setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &limite_ms, sizeof limite_ms);
#endif
}
