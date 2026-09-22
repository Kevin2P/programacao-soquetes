/*
 * estado.c: memoria compartilhada + mutex entre processos + contadores.
 *
 * Regras para a estrutura compartilhada:
 *   - sem ponteiros (o endereco so vale dentro de um processo)
 *   - sem malloc: apenas arrays de tamanho fixo
 */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "common.h"
#include "estado.h"

typedef struct {
    int   usado;                     /* slot ocupado? */
    char  nome[NOME_MAX + 1];
    Valor valor;                     /* string ou inteiro                      */
    int   reservado;                 /* 0 = livre, 1 = reservado               */
    char  dono[ID_MAX + 1];          /* id do cliente que reservou             */
} Recurso;

/* Contadores atomicos: podem ser atualizados por qualquer processo sem mutex,
 * inclusive pelo tratador de SIGCHLD do pai. */
typedef struct {
    time_t inicio;
    _Atomic uint64_t conexoes_ativas;
    _Atomic uint64_t conexoes_total;
    _Atomic uint64_t ops_total;
    _Atomic uint64_t erros_total;
    _Atomic uint64_t ops[NUM_REQ];
} Contadores;

typedef struct {
    pthread_mutex_t mtx;             /* protege recursos[] e ids[] */
    Contadores cont;
    Recurso recursos[MAX_RECURSOS];
    char    ids[MAX_CLIENTES][ID_MAX + 1];   /* "" = slot livre */
} Estado;

static Estado *E;                    /* aponta para a regiao compartilhada */

void est_init(void)
{
    /* MAP_ANONYMOUS: memoria nova, zerada, sem arquivo.
     * MAP_SHARED: continua compartilhada com os filhos apos o fork. */
    E = mmap(NULL, sizeof *E, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (E == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    E->cont.inicio = time(NULL);

    /* Mutex que funciona entre processos e sobrevive a morte do dono. */
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&a, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(&E->mtx, &a);
    pthread_mutexattr_destroy(&a);
}

static void travar(void)
{
    int r = pthread_mutex_lock(&E->mtx);
    /* Mutex "robusto": se um processo morreu segurando o lock, o proximo a
     * travar recebe EOWNERDEAD. Marcamos como consistente e seguimos, em vez
     * de deixar o servidor inteiro travado para sempre. */
    if (r == EOWNERDEAD)
        pthread_mutex_consistent(&E->mtx);
}

static void destravar(void)
{
    pthread_mutex_unlock(&E->mtx);
}

/* Procura um recurso pelo nome. Chamar com o mutex travado. */
static Recurso *buscar(const char *nome)
{
    for (int i = 0; i < MAX_RECURSOS; i++)
        if (E->recursos[i].usado && strcmp(E->recursos[i].nome, nome) == 0)
            return &E->recursos[i];
    return NULL;
}

int est_registrar_id(const char *id)
{
    int livre = -1, st;
    travar();
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (E->ids[i][0] == '\0') {
            if (livre < 0)
                livre = i;
        } else if (strcmp(E->ids[i], id) == 0) {
            destravar();
            return ST_ID_IN_USE;
        }
    }
    if (livre < 0) {
        st = ST_FULL;
    } else {
        snprintf(E->ids[livre], sizeof E->ids[livre], "%s", id);
        st = ST_OK;
    }
    destravar();
    return st;
}

int est_desconectar(const char *id, char nomes[][NOME_MAX + 1], int max)
{
    int liberados = 0;
    travar();
    for (int i = 0; i < MAX_RECURSOS; i++) {
        Recurso *r = &E->recursos[i];
        if (r->usado && r->reservado && strcmp(r->dono, id) == 0) {
            r->reservado = 0;
            r->dono[0] = '\0';
            if (liberados < max)
                snprintf(nomes[liberados], NOME_MAX + 1, "%s", r->nome);
            liberados++;
        }
    }
    /* Remover o id na MESMA secao critica: assim ninguem consegue reutilizar
     * o id e "herdar" reservas que ainda nao tinham sido liberadas. */
    for (int i = 0; i < MAX_CLIENTES; i++)
        if (strcmp(E->ids[i], id) == 0)
            E->ids[i][0] = '\0';
    destravar();
    return liberados;
}

int est_criar(const char *nome, const Valor *v)
{
    int st = ST_OK;
    travar();
    if (buscar(nome)) {
        st = ST_ALREADY_EXISTS;
    } else {
        Recurso *slot = NULL;
        for (int i = 0; i < MAX_RECURSOS && !slot; i++)
            if (!E->recursos[i].usado)
                slot = &E->recursos[i];
        if (!slot) {
            st = ST_FULL;
        } else {
            slot->usado = 1;
            slot->reservado = 0;
            slot->dono[0] = '\0';
            snprintf(slot->nome, sizeof slot->nome, "%s", nome);
            slot->valor = *v;
        }
    }
    destravar();
    return st;
}

int est_ler(const char *nome, Valor *out)
{
    int st = ST_OK;
    travar();
    Recurso *r = buscar(nome);
    if (!r)
        st = ST_NOT_FOUND;
    else
        *out = r->valor;                 /* copia ainda com o lock */
    destravar();
    return st;
}

int est_alterar(const char *nome, const Valor *v, const char *cliente)
{
    int st = ST_OK;
    travar();
    Recurso *r = buscar(nome);
    if (!r)
        st = ST_NOT_FOUND;
    else if (r->reservado && strcmp(r->dono, cliente) != 0)
        st = ST_BUSY;             /* reservado por outro: nao pode alterar */
    else
        r->valor = *v;            /* o tipo (string ou inteiro) tambem pode mudar */
    destravar();
    return st;
}

int est_reservar(const char *nome, const char *cliente)
{
    int st = ST_OK;
    travar();
    Recurso *r = buscar(nome);
    if (!r) {
        st = ST_NOT_FOUND;
    } else if (r->reservado && strcmp(r->dono, cliente) != 0) {
        st = ST_BUSY;
    } else {
        /* Testar e atribuir dentro da MESMA secao critica: dois clientes
         * nunca conseguem reservar o mesmo recurso ao mesmo tempo. */
        r->reservado = 1;
        snprintf(r->dono, sizeof r->dono, "%s", cliente);
    }
    destravar();
    return st;
}

int est_liberar(const char *nome, const char *cliente)
{
    int st = ST_OK;
    travar();
    Recurso *r = buscar(nome);
    if (!r)
        st = ST_NOT_FOUND;
    else if (!r->reservado)
        st = ST_NOT_RESERVED;
    else if (strcmp(r->dono, cliente) != 0)
        st = ST_NOT_OWNER;
    else {
        r->reservado = 0;
        r->dono[0] = '\0';
    }
    destravar();
    return st;
}

int est_listar(ItemLista *out, int max, int filtro)
{
    int n = 0;
    travar();
    for (int i = 0; i < MAX_RECURSOS && n < max; i++) {
        Recurso *r = &E->recursos[i];
        if (!r->usado)
            continue;
        if (filtro == LISTA_LIVRES && r->reservado)
            continue;
        if (filtro == LISTA_RESERVADOS && !r->reservado)
            continue;
        snprintf(out[n].nome, sizeof out[n].nome, "%s", r->nome);
        out[n].valor = r->valor;
        out[n].reservado = r->reservado;
        snprintf(out[n].dono, sizeof out[n].dono, "%s", r->reservado ? r->dono : "");
        n++;
    }
    destravar();
    return n;
}

/* ---------------------------- monitoramento ---------------------------- */

void est_conexao_aberta(void)
{
    atomic_fetch_add(&E->cont.conexoes_ativas, 1);
    atomic_fetch_add(&E->cont.conexoes_total, 1);
}

void est_conexao_encerrada(void)
{
    atomic_fetch_sub(&E->cont.conexoes_ativas, 1);
}

void est_registrar_operacao(int tipo)
{
    atomic_fetch_add(&E->cont.ops_total, 1);
    if (tipo >= 0 && tipo < NUM_REQ)
        atomic_fetch_add(&E->cont.ops[tipo], 1);
}

void est_registrar_erro(void)
{
    atomic_fetch_add(&E->cont.erros_total, 1);
}

void est_estatisticas(Estatisticas *out)
{
    memset(out, 0, sizeof *out);
    out->uptime_s        = (uint64_t)(time(NULL) - E->cont.inicio);
    out->conexoes_ativas = atomic_load(&E->cont.conexoes_ativas);
    out->conexoes_total  = atomic_load(&E->cont.conexoes_total);
    out->ops_total       = atomic_load(&E->cont.ops_total);
    out->erros_total     = atomic_load(&E->cont.erros_total);
    for (int i = 0; i < NUM_REQ; i++)
        out->ops[i] = atomic_load(&E->cont.ops[i]);

    travar();
    for (int i = 0; i < MAX_RECURSOS; i++) {
        if (E->recursos[i].usado) {
            out->recursos++;
            if (E->recursos[i].reservado)
                out->reservados++;
        }
    }
    for (int i = 0; i < MAX_CLIENTES; i++)
        if (E->ids[i][0] != '\0')
            out->clientes++;
    destravar();
}
