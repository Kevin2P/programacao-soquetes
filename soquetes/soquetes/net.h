/*
 * net.h: funcoes de rede que enviam e recebem mensagens completas.
 */
#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <stdint.h>

/* Le exatamente n bytes de fd.
 * Retorna: 1 = leu tudo, 0 = o outro lado fechou a conexao, -1 = erro. */
int read_full(int fd, void *buf, size_t n);

/* Escreve exatamente n bytes em fd. Retorna 0 = ok, -1 = erro. */
int write_full(int fd, const void *buf, size_t n);

/* Monta cabecalho + payload e envia como UMA mensagem. Retorna 0 ou -1. */
int send_msg(int fd, uint8_t tipo, const void *payload, uint32_t len);

/* Recebe uma mensagem completa.
 * cap = tamanho do buffer 'buf'. Preenche *tipo e *len.
 * Retorna: 1 = ok, 0 = conexao fechada, -1 = erro de rede,
 *          -2 = mensagem invalida (tamanho maior que o permitido). */
int recv_msg(int fd, uint8_t *tipo, void *buf, size_t cap, uint32_t *len);

/* Conecta a host:porta via TCP (IPv4 ou IPv6, com TCP_NODELAY). Retorna o fd ou -1. */
int net_connect(const char *host, const char *porta);

/* Liga o TCP keepalive no soquete (parametros em common.h). */
void net_keepalive(int fd);

/* Depuracao: imprime uma mensagem em hexadecimal (cabecalho + payload). */
void dump_msg(const char *prefixo, uint8_t tipo, const void *payload, uint32_t len);

#endif
