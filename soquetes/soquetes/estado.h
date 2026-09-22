/*
 * estado.h: estado global compartilhado entre TODOS os processos do servidor.
 *
 * Cada cliente e atendido por um processo diferente (fork), e processos tem
 * memoria separada. Por isso o estado fica numa regiao criada com mmap
 * (MAP_SHARED) ANTES dos forks: pai e filhos enxergam a mesma memoria.
 *
 * Recursos e ids: protegidos por um mutex compartilhado. Todas as funcoes
 * abaixo travam o mutex, fazem o trabalho e destravam.
 * Contadores de monitoramento: variaveis atomicas (sem mutex).
 */
#ifndef ESTADO_H
#define ESTADO_H

#include <stdint.h>

#include "common.h"

/* Copia de um recurso para uso fora do lock (usada pelo LIST). */
typedef struct {
    char  nome[NOME_MAX + 1];
    Valor valor;
    int   reservado;
    char  dono[ID_MAX + 1];
} ItemLista;

/* Foto dos contadores do servidor (usada pelo STATS). */
typedef struct {
    uint64_t uptime_s;
    uint64_t conexoes_ativas;       /* processos filhos vivos            */
    uint64_t conexoes_total;        /* conexoes aceitas desde o inicio   */
    uint64_t clientes;              /* clientes identificados (HELLO)    */
    uint64_t recursos;
    uint64_t reservados;
    uint64_t ops_total;             /* requisicoes recebidas             */
    uint64_t erros_total;           /* respostas com status diferente de OK */
    uint64_t ops[NUM_REQ];          /* requisicoes por tipo              */
} Estatisticas;

/* Cria a memoria compartilhada e o mutex. Chamar UMA vez, antes do 1o fork. */
void est_init(void);

/* Ids de clientes conectados. registrar: ST_OK, ST_ID_IN_USE ou ST_FULL. */
int  est_registrar_id(const char *id);

/* Desconexao do cliente: libera TODAS as reservas dele e remove o id, na mesma
 * secao critica. Guarda em nomes[] os recursos liberados (ate max) e devolve
 * quantos foram. */
int  est_desconectar(const char *id, char nomes[][NOME_MAX + 1], int max);

/* Recursos. Todas devolvem um status (ST_OK ou codigo de erro). */
int est_criar(const char *nome, const Valor *v);            /* ST_ALREADY_EXISTS, ST_FULL */
int est_ler(const char *nome, Valor *out);                  /* ST_NOT_FOUND               */
int est_alterar(const char *nome, const Valor *v,
                const char *cliente);                       /* ST_NOT_FOUND, ST_BUSY      */
int est_reservar(const char *nome, const char *cliente);    /* ST_NOT_FOUND, ST_BUSY (reservar o que ja e seu = ST_OK) */
int est_liberar(const char *nome, const char *cliente);     /* ST_NOT_FOUND, ST_NOT_RESERVED, ST_NOT_OWNER */

/* Foto dos recursos, filtrada por LISTA_TODOS, LISTA_LIVRES ou LISTA_RESERVADOS.
 * Devolve quantos foram copiados. */
int est_listar(ItemLista *out, int max, int filtro);

/* Monitoramento */
void est_conexao_aberta(void);            /* pai, ANTES do fork                    */
void est_conexao_encerrada(void);         /* pai, ao recolher um filho (SIGCHLD)   */
void est_registrar_operacao(int tipo);    /* filho, para cada requisicao recebida  */
void est_registrar_erro(void);            /* filho, para cada resposta de erro     */
void est_estatisticas(Estatisticas *out);

#endif
