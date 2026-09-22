/*
 * tester.c: testes automaticos e medicao de desempenho.
 *
 * Fala o protocolo binario diretamente. Cada "cliente" de teste e uma thread
 * com a sua propria conexao TCP e o seu proprio id, exatamente como um cliente
 * de verdade.
 *
 * Modos (todos retornam 0 se passaram, 1 se falharam):
 *   func      <host> <porta>                 operacoes, tipos de valor, filtros do LIST e STATS
 *   race      <host> <porta> [n=10]          n clientes disputam o MESMO recurso: so 1 pode ganhar
 *   crash     <host> <porta>                 cliente reserva e cai abruptamente (RST): recurso deve liberar
 *   counter   <host> <porta> [n=10] [it=50]  n clientes incrementam um contador inteiro sob RESERVE
 *   malformed <host> <porta>                 mensagens invalidas nao derrubam o servidor
 *   limit     <host> <porta>                 o cliente numero MAX_CLIENTES+1 e recusado (FULL)
 *   load      <host> <porta> [n=10] [ciclos=100]  latencia e vazao com n clientes simultaneos
 *   keepalive <host> <porta>                 o servidor liga o TCP keepalive nas conexoes (Linux, servidor na mesma maquina)
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "net.h"
#include "proto.h"

static const char *g_host, *g_port;

typedef struct {
    int fd;
} Conn;

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

/* ---------------------- operacoes de alto nivel ---------------------- */

/* Envia uma requisicao e recebe a resposta. 0 = ok, -1 = erro de rede ou timeout. */
static int rpc(Conn *c, uint8_t tipo, const uint8_t *pay, size_t plen,
               uint8_t *st, uint8_t *resp, size_t cap, uint32_t *rlen)
{
    if (send_msg(c->fd, tipo, pay, (uint32_t)plen) < 0)
        return -1;
    return recv_msg(c->fd, st, resp, cap, rlen) == 1 ? 0 : -1;
}

static void definir_timeout(int fd, int segundos)
{
    struct timeval tv = { .tv_sec = segundos, .tv_usec = 0 };   /* nenhum teste fica preso para sempre */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

/* Conecta e faz HELLO. Devolve 0 se o servidor respondeu OK; *status recebe a resposta.
 * Se o servidor recusar o HELLO, o fd continua aberto (quem chamou fecha). */
static int conn_open_st(Conn *c, const char *id, int *status)
{
    uint8_t pay[64], resp[512], st;
    uint32_t rl;
    size_t off = 0;

    *status = -1;
    c->fd = net_connect(g_host, g_port);
    if (c->fd < 0)
        return -1;
    definir_timeout(c->fd, 10);
    put_str(pay, sizeof pay, &off, id);
    if (rpc(c, REQ_HELLO, pay, off, &st, resp, sizeof resp, &rl) < 0) {
        close(c->fd);
        c->fd = -1;
        return -1;
    }
    *status = st;
    return st == ST_OK ? 0 : -1;
}

static int conn_open(Conn *c, const char *id)
{
    int st;
    int r = conn_open_st(c, id, &st);
    if (r < 0 && c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    return r;
}

/* RESERVE, RELEASE ou GET (sem ler o valor): so o nome. Devolve o status, ou -1. */
static int op_nome(Conn *c, uint8_t tipo, const char *nome)
{
    uint8_t pay[64], resp[512], st;
    uint32_t rl;
    size_t off = 0;
    put_str(pay, sizeof pay, &off, nome);
    if (rpc(c, tipo, pay, off, &st, resp, sizeof resp, &rl) < 0)
        return -1;
    return st;
}

/* CREATE ou SET com um Valor. Devolve o status, ou -1. */
static int op_valor(Conn *c, uint8_t tipo, const char *nome, const Valor *v)
{
    uint8_t pay[512], resp[512], st;
    uint32_t rl;
    size_t off = 0;
    put_str(pay, sizeof pay, &off, nome);
    put_valor(pay, sizeof pay, &off, v);
    if (rpc(c, tipo, pay, off, &st, resp, sizeof resp, &rl) < 0)
        return -1;
    return st;
}

/* GET: devolve o status e, se OK, o valor em *out. */
static int op_get(Conn *c, const char *nome, Valor *out)
{
    uint8_t pay[64], resp[512], st;
    uint32_t rl;
    size_t off = 0;
    put_str(pay, sizeof pay, &off, nome);
    if (rpc(c, REQ_GET, pay, off, &st, resp, sizeof resp, &rl) < 0)
        return -1;
    if (st == ST_OK) {
        off = 0;
        if (get_valor(resp, rl, &off, out) < 0)
            return -1;
    }
    return st;
}

static Valor vint(int64_t x)
{
    Valor v;
    memset(&v, 0, sizeof v);
    v.tipo = VAL_INT;
    v.inteiro = x;
    return v;
}

static Valor vstr(const char *s)
{
    Valor v;
    memset(&v, 0, sizeof v);
    v.tipo = VAL_STRING;
    snprintf(v.texto, sizeof v.texto, "%s", s);
    return v;
}

static int valor_igual(const Valor *a, const Valor *b)
{
    if (a->tipo != b->tipo)
        return 0;
    return a->tipo == VAL_INT ? a->inteiro == b->inteiro : strcmp(a->texto, b->texto) == 0;
}

static void verdict(const char *name, int pass)
{
    printf("[%s] %s\n", pass ? "PASS" : "FAIL", name);
}

/* Uma verificacao dentro de um teste: imprime e acumula. */
static void check(int *ok, const char *descricao, int condicao)
{
    printf("  %s %s\n", condicao ? "ok  " : "ERRO", descricao);
    if (!condicao)
        *ok = 0;
}

/* LIST com filtro: devolve quantos recursos vieram, e diz se 'procurado' esta entre eles
 * (e quem e o dono). Devolve -1 em caso de erro. */
static int op_list(Conn *c, uint8_t filtro, const char *procurado, int *achou, char *dono_out)
{
    static uint8_t resp[MAX_PAYLOAD];
    uint8_t pay[1], st;
    uint32_t rl;
    size_t off = 0, poff = 0;
    uint16_t n;

    if (filtro != LISTA_TODOS)
        put_u8(pay, sizeof pay, &poff, filtro);
    if (rpc(c, REQ_LIST, pay, poff, &st, resp, sizeof resp, &rl) < 0 || st != ST_OK)
        return -1;
    if (get_u16(resp, rl, &off, &n) < 0)
        return -1;
    *achou = 0;
    if (dono_out)
        dono_out[0] = '\0';
    for (uint16_t i = 0; i < n; i++) {
        char nome[NOME_MAX + 1], dono[ID_MAX + 1];
        Valor v;
        uint8_t res;
        if (get_str(resp, rl, &off, nome, sizeof nome) < 0 || get_valor(resp, rl, &off, &v) < 0 ||
            get_u8(resp, rl, &off, &res) < 0 || get_str(resp, rl, &off, dono, sizeof dono) < 0)
            return -1;
        if (strcmp(nome, procurado) == 0) {
            *achou = 1;
            if (dono_out)
                snprintf(dono_out, ID_MAX + 1, "%s", dono);
        }
    }
    return n;
}

/* STATS: preenche os 8 contadores. Devolve 0 ou -1. */
static int op_stats(Conn *c, uint64_t ctr[8])
{
    uint8_t resp[512], st;
    uint32_t rl;
    size_t off = 0;
    if (rpc(c, REQ_STATS, NULL, 0, &st, resp, sizeof resp, &rl) < 0 || st != ST_OK)
        return -1;
    for (int i = 0; i < 8; i++)
        if (get_u64(resp, rl, &off, &ctr[i]) < 0)
            return -1;
    return 0;
}

/* ---------------------------- func ---------------------------- */

static int test_func(void)
{
    Conn a, b, d;
    char n_int[48], n_max[48], dono[ID_MAX + 1];
    Valor v, v42 = vint(42), vneg = vint(-7), vmax = vint(INT64_MAX);
    Valor vtxt = vstr("ola mundo com espacos");
    int ok = 1, achou, st_dup = 0;
    uint64_t s0[8], s1[8];

    snprintf(n_int, sizeof n_int, "f_int_%d", getpid());
    snprintf(n_max, sizeof n_max, "f_max_%d", getpid());

    if (conn_open(&a, "func_a") < 0 || conn_open(&b, "func_b") < 0) {
        fprintf(stderr, "conexao falhou\n");
        return 1;
    }

    printf("tipos de valor\n");
    check(&ok, "CREATE com inteiro 42", op_valor(&a, REQ_CREATE, n_int, &v42) == ST_OK);
    check(&ok, "GET (outro cliente) devolve o inteiro 42",
          op_get(&b, n_int, &v) == ST_OK && valor_igual(&v, &v42));
    check(&ok, "SET altera para o inteiro negativo -7", op_valor(&a, REQ_SET, n_int, &vneg) == ST_OK);
    check(&ok, "GET devolve -7", op_get(&b, n_int, &v) == ST_OK && valor_igual(&v, &vneg));
    check(&ok, "CREATE com INT64_MAX", op_valor(&a, REQ_CREATE, n_max, &vmax) == ST_OK);
    check(&ok, "GET devolve INT64_MAX sem perda", op_get(&b, n_max, &v) == ST_OK && valor_igual(&v, &vmax));
    check(&ok, "SET troca o tipo do recurso para string", op_valor(&a, REQ_SET, n_max, &vtxt) == ST_OK);
    check(&ok, "GET devolve a string com espacos", op_get(&b, n_max, &v) == ST_OK && valor_igual(&v, &vtxt));

    printf("filtros do LIST\n");
    check(&ok, "func_a reserva o recurso inteiro", op_nome(&a, REQ_RESERVE, n_int) == ST_OK);
    check(&ok, "LIST todos contem o recurso reservado, com o dono certo",
          op_list(&b, LISTA_TODOS, n_int, &achou, dono) >= 0 && achou && strcmp(dono, "func_a") == 0);
    check(&ok, "LIST livres NAO contem o recurso reservado",
          op_list(&b, LISTA_LIVRES, n_int, &achou, NULL) >= 0 && !achou);
    check(&ok, "LIST livres contem um recurso livre",
          op_list(&b, LISTA_LIVRES, n_max, &achou, NULL) >= 0 && achou);
    check(&ok, "LIST reservados contem o recurso reservado, com o dono certo",
          op_list(&b, LISTA_RESERVADOS, n_int, &achou, dono) >= 0 && achou && strcmp(dono, "func_a") == 0);
    check(&ok, "LIST reservados NAO contem um recurso livre",
          op_list(&b, LISTA_RESERVADOS, n_max, &achou, NULL) >= 0 && !achou);

    printf("regras de erro\n");
    check(&ok, "GET de recurso inexistente = NOT_FOUND", op_nome(&b, REQ_GET, "nao_existe_xyz") == ST_NOT_FOUND);
    check(&ok, "CREATE repetido = ALREADY_EXISTS", op_valor(&b, REQ_CREATE, n_int, &vtxt) == ST_ALREADY_EXISTS);
    check(&ok, "RESERVE por outro cliente = BUSY", op_nome(&b, REQ_RESERVE, n_int) == ST_BUSY);
    check(&ok, "SET por outro cliente = BUSY", op_valor(&b, REQ_SET, n_int, &vtxt) == ST_BUSY);
    check(&ok, "RELEASE por outro cliente = NOT_OWNER", op_nome(&b, REQ_RELEASE, n_int) == ST_NOT_OWNER);
    check(&ok, "RELEASE pelo dono = OK", op_nome(&a, REQ_RELEASE, n_int) == ST_OK);
    check(&ok, "RELEASE de recurso livre = NOT_RESERVED", op_nome(&a, REQ_RELEASE, n_int) == ST_NOT_RESERVED);
    conn_open_st(&d, "func_a", &st_dup);
    check(&ok, "HELLO com id repetido = ID_IN_USE", st_dup == ST_ID_IN_USE);
    if (d.fd >= 0)
        close(d.fd);

    printf("monitoramento (STATS)\n");
    check(&ok, "STATS responde", op_stats(&a, s0) == 0);
    op_nome(&a, REQ_GET, "nao_existe_xyz");                 /* erro proposital */
    check(&ok, "STATS responde de novo", op_stats(&a, s1) == 0);
    check(&ok, "conexoes ativas >= 2 (func_a e func_b)", s1[1] >= 2);
    check(&ok, "recursos >= 2", s1[4] >= 2);
    check(&ok, "requisicoes aumentaram entre as duas leituras", s1[6] > s0[6]);
    check(&ok, "respostas de erro aumentaram entre as duas leituras", s1[7] > s0[7]);

    close(a.fd);
    close(b.fd);
    verdict("funcional: operacoes, tipos de valor, filtros do LIST e STATS", ok);
    return !ok;
}

/* ---------------------------- race ---------------------------- */

static pthread_barrier_t g_bar1, g_bar2;
static atomic_int g_ok, g_busy, g_other;
static char g_resname[64];

static void *race_thread(void *arg)
{
    long i = (long)arg;
    char id[32];
    Conn c;
    snprintf(id, sizeof id, "racer%ld", i);
    int opened = conn_open(&c, id) == 0;

    pthread_barrier_wait(&g_bar1);       /* todos largam juntos */
    int st = opened ? op_nome(&c, REQ_RESERVE, g_resname) : -1;
    if (st == ST_OK)
        atomic_fetch_add(&g_ok, 1);
    else if (st == ST_BUSY)
        atomic_fetch_add(&g_busy, 1);
    else
        atomic_fetch_add(&g_other, 1);
    pthread_barrier_wait(&g_bar2);       /* so desconecta depois que todos tentaram */

    if (opened)
        close(c.fd);
    return NULL;
}

static int test_race(int n)
{
    Conn s;
    snprintf(g_resname, sizeof g_resname, "race_%d", getpid());
    Valor v = vstr("x");
    if (conn_open(&s, "race_setup") < 0 || op_valor(&s, REQ_CREATE, g_resname, &v) != ST_OK) {
        fprintf(stderr, "setup falhou\n");
        return 1;
    }
    pthread_barrier_init(&g_bar1, NULL, (unsigned)n);
    pthread_barrier_init(&g_bar2, NULL, (unsigned)n);
    pthread_t *th = malloc(sizeof(pthread_t) * (size_t)n);
    for (long i = 0; i < n; i++)
        pthread_create(&th[i], NULL, race_thread, (void *)i);
    for (int i = 0; i < n; i++)
        pthread_join(th[i], NULL);
    free(th);
    close(s.fd);

    printf("race: %d clientes; OK=%d, BUSY=%d, outros=%d\n", n, g_ok, g_busy, g_other);
    int pass = g_ok == 1 && g_busy == n - 1 && g_other == 0;
    verdict("concorrencia: exatamente 1 cliente reserva o mesmo recurso", pass);
    return !pass;
}

/* ---------------------------- crash ---------------------------- */

static int test_crash(void)
{
    Conn a, b;
    char name[64];
    int ok = 1;
    snprintf(name, sizeof name, "crash_%d", getpid());
    Valor v = vstr("v");

    if (conn_open(&a, "crashA") < 0 || conn_open(&b, "crashB") < 0) {
        fprintf(stderr, "conexao falhou\n");
        return 1;
    }
    check(&ok, "A cria o recurso", op_valor(&a, REQ_CREATE, name, &v) == ST_OK);
    check(&ok, "A reserva", op_nome(&a, REQ_RESERVE, name) == ST_OK);
    check(&ok, "B tenta reservar com A vivo: BUSY", op_nome(&b, REQ_RESERVE, name) == ST_BUSY);

    /* A cai abruptamente: SO_LINGER com timeout 0 faz close() enviar RST (sem QUIT). */
    struct linger lg = { .l_onoff = 1, .l_linger = 0 };
    setsockopt(a.fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    close(a.fd);
    usleep(300 * 1000);                 /* tempo para o servidor detectar */

    check(&ok, "B reserva depois da queda de A: OK (reserva liberada automaticamente)",
          op_nome(&b, REQ_RESERVE, name) == ST_OK);
    close(b.fd);

    verdict("falha: reserva liberada automaticamente apos queda abrupta do cliente", ok);
    return !ok;
}

/* ---------------------------- counter ---------------------------- */

static int g_iters;
static char g_ctrname[64];
static atomic_int g_counter_err;

static void *counter_thread(void *arg)
{
    long i = (long)arg;
    char id[32];
    Conn c;
    snprintf(id, sizeof id, "cnt%ld", i);
    if (conn_open(&c, id) < 0) {
        atomic_fetch_add(&g_counter_err, 1);
        return NULL;
    }
    for (int k = 0; k < g_iters; k++) {
        /* Adquire o "lock" distribuido: repete enquanto estiver ocupado. */
        for (;;) {
            int st = op_nome(&c, REQ_RESERVE, g_ctrname);
            if (st == ST_OK)
                break;
            if (st != ST_BUSY) {
                atomic_fetch_add(&g_counter_err, 1);
                close(c.fd);
                return NULL;
            }
            usleep(200);
        }
        /* Secao critica: le, incrementa (inteiro de 64 bits), escreve. */
        Valor v;
        if (op_get(&c, g_ctrname, &v) != ST_OK || v.tipo != VAL_INT) {
            atomic_fetch_add(&g_counter_err, 1);
        } else {
            Valor novo = vint(v.inteiro + 1);
            if (op_valor(&c, REQ_SET, g_ctrname, &novo) != ST_OK)
                atomic_fetch_add(&g_counter_err, 1);
        }
        if (op_nome(&c, REQ_RELEASE, g_ctrname) != ST_OK)
            atomic_fetch_add(&g_counter_err, 1);
    }
    close(c.fd);
    return NULL;
}

static int test_counter(int n, int iters)
{
    Conn s;
    g_iters = iters;
    snprintf(g_ctrname, sizeof g_ctrname, "counter_%d", getpid());
    Valor zero = vint(0);
    if (conn_open(&s, "cnt_setup") < 0 || op_valor(&s, REQ_CREATE, g_ctrname, &zero) != ST_OK) {
        fprintf(stderr, "setup falhou\n");
        return 1;
    }
    double t0 = now_ms();
    pthread_t *th = malloc(sizeof(pthread_t) * (size_t)n);
    for (long i = 0; i < n; i++)
        pthread_create(&th[i], NULL, counter_thread, (void *)i);
    for (int i = 0; i < n; i++)
        pthread_join(th[i], NULL);
    free(th);
    double dt = now_ms() - t0;

    Valor v;
    long long final = -1;
    if (op_get(&s, g_ctrname, &v) == ST_OK && v.tipo == VAL_INT)
        final = (long long)v.inteiro;
    close(s.fd);

    long long expected = (long long)n * iters;
    printf("counter: %d clientes x %d incrementos; esperado=%lld, obtido=%lld, erros=%d, tempo=%.0f ms\n",
           n, iters, expected, final, g_counter_err, dt);
    int pass = final == expected && g_counter_err == 0;
    verdict("consistencia: contador inteiro compartilhado protegido por RESERVE/RELEASE", pass);
    return !pass;
}

/* ---------------------------- malformed ---------------------------- */

/* Espera uma resposta e devolve o status (ou -1 se a conexao fechou ou deu timeout). */
static int esperar_status(Conn *c)
{
    uint8_t st, resp[512];
    uint32_t rl;
    return recv_msg(c->fd, &st, resp, sizeof resp, &rl) == 1 ? st : -1;
}

static int test_malformed(void)
{
    Conn c, d, e;
    int ok = 1;
    uint8_t pay[64], buf[16];
    size_t off;

    /* 1) comando antes do HELLO */
    d.fd = net_connect(g_host, g_port);
    definir_timeout(d.fd, 5);
    off = 0;
    put_str(pay, sizeof pay, &off, "x");
    send_msg(d.fd, REQ_GET, pay, (uint32_t)off);
    check(&ok, "GET antes do HELLO = NOT_IDENTIFIED", esperar_status(&d) == ST_NOT_IDENTIFIED);
    close(d.fd);

    if (conn_open(&c, "malf") < 0) {
        fprintf(stderr, "conexao falhou\n");
        return 1;
    }

    /* 2) tipo de comando desconhecido */
    send_msg(c.fd, 99, NULL, 0);
    check(&ok, "tipo de comando desconhecido = BAD_REQUEST", esperar_status(&c) == ST_BAD_REQUEST);

    /* 3) string com comprimento maior que o payload (diz 60000 bytes, tem 3) */
    buf[0] = 0xEA; buf[1] = 0x60; buf[2] = 'a'; buf[3] = 'b'; buf[4] = 'c';
    send_msg(c.fd, REQ_GET, buf, 5);
    check(&ok, "string com comprimento mentiroso = BAD_REQUEST", esperar_status(&c) == ST_BAD_REQUEST);

    /* 4) bytes sobrando depois dos campos */
    off = 0;
    put_str(pay, sizeof pay, &off, "abc");
    pay[off++] = 0xFF;
    send_msg(c.fd, REQ_GET, pay, (uint32_t)off);
    check(&ok, "bytes sobrando no payload = BAD_REQUEST", esperar_status(&c) == ST_BAD_REQUEST);

    /* 5) '\0' dentro de uma string */
    buf[0] = 0; buf[1] = 3; buf[2] = 'a'; buf[3] = 0; buf[4] = 'b';
    send_msg(c.fd, REQ_GET, buf, 5);
    check(&ok, "byte NUL dentro de string = BAD_REQUEST", esperar_status(&c) == ST_BAD_REQUEST);

    /* 6) tipo de valor invalido em CREATE */
    off = 0;
    put_str(pay, sizeof pay, &off, "malf_v");
    pay[off++] = 7;                                    /* tipo de valor 7 nao existe */
    send_msg(c.fd, REQ_CREATE, pay, (uint32_t)off);
    check(&ok, "tipo de valor invalido = BAD_REQUEST", esperar_status(&c) == ST_BAD_REQUEST);

    /* 7) filtro invalido no LIST */
    buf[0] = 9;
    send_msg(c.fd, REQ_LIST, buf, 1);
    check(&ok, "filtro de LIST invalido = BAD_REQUEST", esperar_status(&c) == ST_BAD_REQUEST);

    /* 7b) nome invalido (com quebra de linha) em GET: nao pode chegar ao log do servidor */
    buf[0] = 0; buf[1] = 3; buf[2] = 'a'; buf[3] = '\n'; buf[4] = 'b';
    send_msg(c.fd, REQ_GET, buf, 5);
    check(&ok, "nome invalido em GET (quebra de linha) = BAD_REQUEST", esperar_status(&c) == ST_BAD_REQUEST);

    /* 8) a mesma conexao segue funcionando depois de todos esses erros */
    Valor v = vstr("ainda vivo");
    char nome[48];
    snprintf(nome, sizeof nome, "malf_%d", getpid());
    check(&ok, "a mesma conexao segue funcionando apos os erros", op_valor(&c, REQ_CREATE, nome, &v) == ST_OK);
    close(c.fd);

    /* 9) cabecalho anunciando 4 GB: o servidor deve fechar a conexao sem alocar nada */
    {
        static const uint8_t gigante[5] = { 0xFF, 0xFF, 0xFF, 0xFF, REQ_GET };
        uint8_t st, resp[16];
        uint32_t rl;
        d.fd = net_connect(g_host, g_port);
        definir_timeout(d.fd, 5);
        write_full(d.fd, gigante, sizeof gigante);
        int r = recv_msg(d.fd, &st, resp, sizeof resp, &rl);
        check(&ok, "tamanho de 4 GB no cabecalho: o servidor fecha a conexao", r == 0 || r == -1);
        close(d.fd);
    }

    /* 10) mensagem truncada: promete 100 bytes, manda 4 e o cliente some */
    {
        static const uint8_t truncada[9] = { 0, 0, 0, 100, REQ_CREATE, 'a', 'b', 'c', 'd' };
        d.fd = net_connect(g_host, g_port);
        write_full(d.fd, truncada, sizeof truncada);
        close(d.fd);
    }
    usleep(200 * 1000);

    /* 11) depois de tudo isso o servidor ainda aceita clientes normais */
    check(&ok, "o servidor continua aceitando clientes depois das mensagens invalidas",
          conn_open(&e, "malf_depois") == 0);
    if (e.fd >= 0)
        close(e.fd);

    verdict("erros de comunicacao: mensagens invalidas nao derrubam o servidor", ok);
    return !ok;
}

/* ---------------------------- limit ---------------------------- */

static int test_limit(void)
{
    Conn *cs = calloc(MAX_CLIENTES + 1, sizeof *cs);
    int ok = 1, abertos = 0, st = 0;
    char id[32];

    for (int i = 0; i <= MAX_CLIENTES; i++)
        cs[i].fd = -1;
    for (int i = 0; i < MAX_CLIENTES; i++) {
        snprintf(id, sizeof id, "lim%d", i);
        if (conn_open(&cs[i], id) < 0)
            break;
        abertos++;
    }
    printf("limit: %d clientes conectados com HELLO OK (limite do servidor: %d)\n", abertos, MAX_CLIENTES);
    check(&ok, "todos os clientes ate o limite foram aceitos", abertos == MAX_CLIENTES);

    int r = conn_open_st(&cs[MAX_CLIENTES], "lim_extra", &st);
    check(&ok, "o cliente seguinte e recusado com FULL", r < 0 && st == ST_FULL);

    for (int i = 0; i <= MAX_CLIENTES; i++)
        if (cs[i].fd >= 0)
            close(cs[i].fd);
    free(cs);
    verdict("limite: cliente alem de MAX_CLIENTES recebe FULL", ok);
    return !ok;
}

/* ---------------------------- load ---------------------------- */

typedef struct {
    long    idx;
    int     cycles;
    double *lat;        /* latencias (ms) de cada requisicao */
    int     nlat;
    int     violations; /* GET que nao devolveu o que o proprio cliente gravou */
    int     errors;
} LoadArg;

static pthread_barrier_t g_loadbar;

/* Executa 'chamada' medindo o tempo e guardando a latencia. */
#define TIMED(a, chamada) ({ double t0_ = now_ms(); int r_ = (chamada); (a)->lat[(a)->nlat++] = now_ms() - t0_; r_; })

static void *load_thread(void *arg)
{
    LoadArg *a = arg;
    char id[48], name[64], txt[32];
    Conn c;
    snprintf(id, sizeof id, "ld%d_%ld", getpid(), a->idx);   /* id unico por execucao */
    snprintf(name, sizeof name, "load%d_%ld", getpid(), a->idx);
    Valor inicial = vint(0);
    int opened = conn_open(&c, id) == 0 && op_valor(&c, REQ_CREATE, name, &inicial) == ST_OK;

    pthread_barrier_wait(&g_loadbar);
    if (!opened) {
        a->errors++;
        return NULL;
    }
    for (int k = 0; k < a->cycles; k++) {
        /* alterna inteiro e string para exercitar os dois tipos */
        Valor esperado, lido;
        if (k % 2 == 0) {
            esperado = vint(k);
        } else {
            snprintf(txt, sizeof txt, "v%d", k);
            esperado = vstr(txt);
        }
        if (TIMED(a, op_valor(&c, REQ_SET, name, &esperado)) != ST_OK) { a->errors++; break; }
        if (TIMED(a, op_get(&c, name, &lido)) != ST_OK) { a->errors++; break; }
        if (!valor_igual(&lido, &esperado))
            a->violations++;
        if (TIMED(a, op_nome(&c, REQ_RESERVE, name)) != ST_OK) { a->errors++; break; }
        if (TIMED(a, op_nome(&c, REQ_RELEASE, name)) != ST_OK) { a->errors++; break; }
    }
    close(c.fd);
    return NULL;
}

static int cmp_double(const void *x, const void *y)
{
    double a = *(const double *)x, b = *(const double *)y;
    return (a > b) - (a < b);
}

static int test_load(int n, int cycles)
{
    LoadArg *args = calloc((size_t)n, sizeof *args);
    pthread_t *th = malloc(sizeof(pthread_t) * (size_t)n);
    pthread_barrier_init(&g_loadbar, NULL, (unsigned)n + 1);

    for (int i = 0; i < n; i++) {
        args[i].idx = i;
        args[i].cycles = cycles;
        args[i].lat = malloc(sizeof(double) * (size_t)cycles * 4);
        pthread_create(&th[i], NULL, load_thread, &args[i]);
    }
    pthread_barrier_wait(&g_loadbar);   /* todos conectados e prontos */
    double t0 = now_ms();
    for (int i = 0; i < n; i++)
        pthread_join(th[i], NULL);
    double dt = now_ms() - t0;

    size_t total = 0;
    int viol = 0, errs = 0;
    for (int i = 0; i < n; i++) {
        total += (size_t)args[i].nlat;
        viol += args[i].violations;
        errs += args[i].errors;
    }
    double *all = malloc(sizeof(double) * (total ? total : 1)), sum = 0;
    size_t k = 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < args[i].nlat; j++) {
            all[k++] = args[i].lat[j];
            sum += args[i].lat[j];
        }
    qsort(all, total, sizeof(double), cmp_double);

    double avg = total ? sum / (double)total : 0, p50 = 0, p95 = 0, p99 = 0, max = 0;
    if (total) {
        p50 = all[total / 2];
        p95 = all[(size_t)((double)total * 0.95)];
        p99 = all[(size_t)((double)total * 0.99)];
        max = all[total - 1];
    }
    double vazao = dt > 0 ? (double)total / (dt / 1000.0) : 0;

    printf("load: %d clientes simultaneos, %d ciclos cada (4 requisicoes por ciclo)\n", n, cycles);
    printf("  requisicoes: %zu em %.1f ms  ->  %.0f req/s\n", total, dt, vazao);
    printf("  latencia (ms): media=%.3f  p50=%.3f  p95=%.3f  p99=%.3f  max=%.3f\n", avg, p50, p95, p99, max);
    printf("  violacoes de consistencia=%d, erros=%d\n", viol, errs);
    /* linha legivel por maquina, usada por tests/bench.sh */
    printf("CSV,%d,%d,%zu,%.1f,%.0f,%.3f,%.3f,%.3f,%.3f,%.3f,%d\n", n, cycles, total, dt, vazao,
           avg, p50, p95, p99, max, viol + errs);
    int pass = viol == 0 && errs == 0;
    for (int i = 0; i < n; i++)
        free(args[i].lat);
    free(args);
    free(th);
    free(all);
    verdict("carga: clientes simultaneos sem erros e com estado consistente", pass);
    return !pass;
}

/* ---------------------------- main ---------------------------- */

/* ------------------------------ keepalive ------------------------------ */

/* Confere se o servidor ligou o TCP keepalive no soquete desta conexao. Le
 * /proc/net/tcp (Linux), entao so funciona com o servidor na mesma maquina;
 * se nao achar o soquete, o teste e ignorado (SKIP). Na linha do soquete do
 * servidor, o campo "tr" vale 2 quando o timer ativo e o do keepalive. */
static int test_keepalive(void)
{
    Conn c;
    int ok = 1;
    struct sockaddr_in loc;
    socklen_t tam = sizeof loc;

    printf("keepalive: o servidor liga o TCP keepalive nas conexoes (%d s de silencio, sonda a cada %d s, %d sondas)\n",
           KEEPALIVE_IDLE, KEEPALIVE_INTVL, KEEPALIVE_CNT);
    if (conn_open(&c, "ka_teste") < 0 || getsockname(c.fd, (struct sockaddr *)&loc, &tam) < 0) {
        verdict("keepalive: nao foi possivel conectar", 0);
        return 1;
    }
    unsigned porta_cli = ntohs(loc.sin_port), porta_srv = (unsigned)atoi(g_port);
    usleep(200000);                       /* deixa o servidor terminar de configurar o soquete */

    int achou = 0;
    unsigned timer = 0;
    FILE *f = fopen("/proc/net/tcp", "r");
    if (f) {
        char linha[512], la[16], ra[16];
        while (fgets(linha, sizeof linha, f)) {
            unsigned lp, rp, st, tr;
            /* "  0: 0100007F:1388 0100007F:C3A2 01 00000000:00000000 02:0000A1B2 ..." */
            if (sscanf(linha, " %*d: %8[0-9A-Fa-f]:%x %8[0-9A-Fa-f]:%x %x %*x:%*x %x:", la, &lp, ra, &rp, &st, &tr) == 6 &&
                lp == porta_srv && rp == porta_cli && st == 1) {
                achou = 1;
                timer = tr;
                break;
            }
        }
        fclose(f);
    }
    close(c.fd);
    if (!achou) {
        printf("[SKIP] keepalive: soquete do servidor nao encontrado (servidor em outra maquina ou sem /proc/net/tcp)\n");
        return 0;
    }
    check(&ok, "o soquete da conexao, no servidor, tem o timer de keepalive ativo", timer == 2);
    verdict("keepalive: ativo em cada conexao (detecta cliente que some sem fechar a conexao)", ok);
    return !ok;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr,
                "uso: %s func      <host> <porta>\n"
                "     %s race      <host> <porta> [n=10]\n"
                "     %s crash     <host> <porta>\n"
                "     %s counter   <host> <porta> [n=10] [iter=50]\n"
                "     %s malformed <host> <porta>\n"
                "     %s limit     <host> <porta>\n"
                "     %s load      <host> <porta> [n=10] [ciclos=100]\n"
                "     %s keepalive <host> <porta>\n",
                argv[0], argv[0], argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }
    const char *mode = argv[1];
    g_host = argv[2];
    g_port = argv[3];
    int a4 = argc > 4 ? atoi(argv[4]) : 0;
    int a5 = argc > 5 ? atoi(argv[5]) : 0;

    if (strcmp(mode, "func") == 0)      return test_func();
    if (strcmp(mode, "race") == 0)      return test_race(a4 > 0 ? a4 : 10);
    if (strcmp(mode, "crash") == 0)     return test_crash();
    if (strcmp(mode, "counter") == 0)   return test_counter(a4 > 0 ? a4 : 10, a5 > 0 ? a5 : 50);
    if (strcmp(mode, "malformed") == 0) return test_malformed();
    if (strcmp(mode, "limit") == 0)     return test_limit();
    if (strcmp(mode, "load") == 0)      return test_load(a4 > 0 ? a4 : 10, a5 > 0 ? a5 : 100);
    if (strcmp(mode, "keepalive") == 0) return test_keepalive();
    fprintf(stderr, "modo desconhecido: %s\n", mode);
    return 2;
}
