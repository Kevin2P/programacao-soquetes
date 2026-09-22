/*
 * server.c: servidor de coordenacao, um PROCESSO por cliente.
 *
 * Comandos: HELLO, CREATE, GET, SET, RESERVE, RELEASE, LIST, STATS, QUIT.
 * Ao desconectar (QUIT, queda ou erro), as reservas do cliente sao liberadas.
 *
 * Log: cada evento vira UMA linha com data e hora, gravada na saida padrao e,
 * com -l, tambem em arquivo. Como cada linha e escrita com um unico write() em
 * um arquivo aberto com O_APPEND, processos diferentes nao misturam as linhas.
 *
 * Uso: ./server [-l arquivo_de_log] [porta]      (porta padrao: 5000)
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "estado.h"
#include "net.h"
#include "proto.h"

/* ------------------------------------------------------------------ */
/* Log                                                                 */
/* ------------------------------------------------------------------ */

static int g_logfd = -1;                    /* arquivo de log (-1 = nao usa) */
static volatile sig_atomic_t g_stop = 0;    /* pedido de encerramento        */

static void escrever(int fd, const char *buf, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, buf, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return;                         /* log nao pode derrubar o servidor */
        }
        buf += w;
        n -= (size_t)w;
    }
}

/* Uma linha: "2026-09-19 22:15:03.412 [pid] NIVEL mensagem". */
static void log_msg(const char *nivel, const char *fmt, ...)
{
    struct timespec t;
    struct tm tm;
    char ts[32], msg[360], linha[480];
    va_list ap;

    clock_gettime(CLOCK_REALTIME, &t);
    localtime_r(&t.tv_sec, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    int n = snprintf(linha, sizeof linha, "%s.%03ld [%d] %-5s %s\n", ts,
                     t.tv_nsec / 1000000, (int)getpid(), nivel, msg);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof linha) {        /* truncada: garante o '\n' final */
        n = (int)sizeof linha - 1;
        linha[n - 1] = '\n';
    }
    escrever(STDOUT_FILENO, linha, (size_t)n);
    if (g_logfd >= 0)
        escrever(g_logfd, linha, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* Sessao de uma conexao (vive so dentro do processo filho)            */
/* ------------------------------------------------------------------ */

typedef struct {
    int  fd;
    int  identificado;
    char id[ID_MAX + 1];
    char origem[64];            /* "ip:porta" do cliente          */
    int  falha_envio;           /* send_msg falhou alguma vez?    */
} Sessao;

/* Registra uma operacao e seu resultado. */
static void log_op(const Sessao *s, const char *desc, int st)
{
    const char *quem = s->identificado ? s->id : s->origem;
    if (st == ST_OK)
        log_msg("INFO", "%s: %s -> OK", quem, desc);
    else
        log_msg(st == ST_FULL ? "ERROR" : "WARN", "%s: %s -> %s", quem, desc, nome_status(st));
}

/* ------------------------------------------------------------------ */
/* Respostas                                                           */
/* ------------------------------------------------------------------ */

static void enviar(Sessao *s, uint8_t status, const uint8_t *pay, size_t len)
{
    if (send_msg(s->fd, status, pay, (uint32_t)len) < 0)
        s->falha_envio = 1;
}

/* Responde com status e, se for erro, uma mensagem textual no payload. */
static void responder(Sessao *s, int st)
{
    if (st == ST_OK) {
        enviar(s, ST_OK, NULL, 0);
        return;
    }
    est_registrar_erro();
    uint8_t p[256];
    size_t off = 0;
    put_str(p, sizeof p, &off, mensagem_status(st));
    enviar(s, (uint8_t)st, p, off);
}

/* OK com um valor no payload (GET). */
static void responder_valor(Sessao *s, const Valor *v)
{
    uint8_t p[2 + VALOR_MAX + 16];
    size_t off = 0;
    put_valor(p, sizeof p, &off, v);
    enviar(s, ST_OK, p, off);
}

/* LIST: OK + [n: 2 bytes] e, para cada recurso:
 *   [nome: string][valor][reservado: 1 byte][dono: string, vazio se livre] */
static void responder_lista(Sessao *s, int filtro)
{
    static ItemLista itens[MAX_RECURSOS];
    static uint8_t pay[MAX_PAYLOAD];
    static const char *rotulo[] = { "todos", "livres", "reservados" };
    size_t off = 0;
    char desc[64];

    int n = est_listar(itens, MAX_RECURSOS, filtro);     /* "foto" tirada sob o lock */
    int ok = put_u16(pay, sizeof pay, &off, (uint16_t)n) == 0;
    for (int i = 0; i < n && ok; i++)
        ok = put_str(pay, sizeof pay, &off, itens[i].nome) == 0 &&
             put_valor(pay, sizeof pay, &off, &itens[i].valor) == 0 &&
             put_u8(pay, sizeof pay, &off, (uint8_t)itens[i].reservado) == 0 &&
             put_str(pay, sizeof pay, &off, itens[i].dono) == 0;
    if (!ok) {
        log_op(s, "LIST", ST_FULL);
        responder(s, ST_FULL);
        return;
    }
    snprintf(desc, sizeof desc, "LIST %s (%d recursos)", rotulo[filtro], n);
    log_op(s, desc, ST_OK);
    /* A rede so e usada DEPOIS de soltar o lock: um cliente lento nao trava os outros. */
    enviar(s, ST_OK, pay, off);
}

/* STATS: 8 contadores de 64 bits, depois [n: 1 byte] e n x ([tipo: 1 byte][contagem: 8 bytes]). */
static void responder_stats(Sessao *s)
{
    Estatisticas e;
    uint8_t pay[256];
    size_t off = 0;

    est_estatisticas(&e);
    int ok = put_u64(pay, sizeof pay, &off, e.uptime_s) == 0 &&
             put_u64(pay, sizeof pay, &off, e.conexoes_ativas) == 0 &&
             put_u64(pay, sizeof pay, &off, e.conexoes_total) == 0 &&
             put_u64(pay, sizeof pay, &off, e.clientes) == 0 &&
             put_u64(pay, sizeof pay, &off, e.recursos) == 0 &&
             put_u64(pay, sizeof pay, &off, e.reservados) == 0 &&
             put_u64(pay, sizeof pay, &off, e.ops_total) == 0 &&
             put_u64(pay, sizeof pay, &off, e.erros_total) == 0 &&
             put_u8(pay, sizeof pay, &off, REQ_STATS) == 0;   /* n = tipos 1..REQ_STATS */
    for (int t = 1; t <= REQ_STATS && ok; t++)
        ok = put_u8(pay, sizeof pay, &off, (uint8_t)t) == 0 &&
             put_u64(pay, sizeof pay, &off, e.ops[t]) == 0;
    if (!ok) {
        log_op(s, "STATS", ST_FULL);
        responder(s, ST_FULL);
        return;
    }
    log_op(s, "STATS", ST_OK);
    enviar(s, ST_OK, pay, off);
}

/* ------------------------------------------------------------------ */
/* Tratamento de uma requisicao                                        */
/* ------------------------------------------------------------------ */

/*
 * Trata UMA requisicao. Retorna 1 para continuar, 0 para encerrar a conexao.
 * 'p' e 'len' descrevem o payload recebido (nao confie no conteudo!).
 */
static int tratar(Sessao *s, uint8_t tipo, const uint8_t *p, uint32_t len)
{
    char nome[NOME_MAX + 1], desc[NOME_MAX + 48];
    Valor v;
    size_t off = 0;
    int st;

    est_registrar_operacao(tipo);

    if (tipo == REQ_QUIT) {
        log_op(s, "QUIT", ST_OK);
        responder(s, ST_OK);
        return 0;
    }

    if (tipo == REQ_HELLO) {
        char id[ID_MAX + 1];
        if (s->identificado)
            st = ST_BAD_REQUEST;
        else if (get_str(p, len, &off, id, sizeof id) < 0 || off != len || !nome_valido(id, ID_MAX))
            st = ST_BAD_REQUEST;
        else {
            st = est_registrar_id(id);
            if (st == ST_OK) {
                s->identificado = 1;
                snprintf(s->id, sizeof s->id, "%s", id);
            }
        }
        log_op(s, "HELLO", st);
        responder(s, st);
        return 1;
    }

    if (!s->identificado) {
        snprintf(desc, sizeof desc, "%s (antes do HELLO)", nome_req(tipo));
        log_op(s, desc, ST_NOT_IDENTIFIED);
        responder(s, ST_NOT_IDENTIFIED);
        return 1;
    }

    switch (tipo) {
    case REQ_CREATE:
    case REQ_SET:
        if (get_str(p, len, &off, nome, sizeof nome) < 0 ||
            get_valor(p, len, &off, &v) < 0 || off != len ||
            !nome_valido(nome, NOME_MAX) ||
            (v.tipo == VAL_STRING && v.texto[0] == '\0')) {
            st = ST_BAD_REQUEST;
            snprintf(desc, sizeof desc, "%s (malformado)", nome_req(tipo));
        } else {
            st = tipo == REQ_CREATE ? est_criar(nome, &v) : est_alterar(nome, &v, s->id);
            snprintf(desc, sizeof desc, "%s %s", nome_req(tipo), nome);
        }
        log_op(s, desc, st);
        responder(s, st);
        break;

    case REQ_GET:
        if (get_str(p, len, &off, nome, sizeof nome) < 0 || off != len || !nome_valido(nome, NOME_MAX)) {
            st = ST_BAD_REQUEST;
            snprintf(desc, sizeof desc, "GET (malformado)");
        } else {
            st = est_ler(nome, &v);
            snprintf(desc, sizeof desc, "GET %s", nome);
        }
        log_op(s, desc, st);
        if (st == ST_OK)
            responder_valor(s, &v);
        else
            responder(s, st);
        break;

    case REQ_RESERVE:
    case REQ_RELEASE:
        if (get_str(p, len, &off, nome, sizeof nome) < 0 || off != len || !nome_valido(nome, NOME_MAX)) {
            st = ST_BAD_REQUEST;
            snprintf(desc, sizeof desc, "%s (malformado)", nome_req(tipo));
        } else {
            st = tipo == REQ_RESERVE ? est_reservar(nome, s->id) : est_liberar(nome, s->id);
            snprintf(desc, sizeof desc, "%s %s", nome_req(tipo), nome);
        }
        log_op(s, desc, st);
        responder(s, st);
        break;

    case REQ_LIST: {
        uint8_t filtro = LISTA_TODOS;              /* sem payload = todos */
        if (len > 1 || (len == 1 && (get_u8(p, len, &off, &filtro) < 0 || filtro > LISTA_RESERVADOS))) {
            log_op(s, "LIST (malformado)", ST_BAD_REQUEST);
            responder(s, ST_BAD_REQUEST);
        } else {
            responder_lista(s, filtro);
        }
        break;
    }

    case REQ_STATS:
        if (len != 0) {
            log_op(s, "STATS (malformado)", ST_BAD_REQUEST);
            responder(s, ST_BAD_REQUEST);
        } else {
            responder_stats(s);
        }
        break;

    default:
        snprintf(desc, sizeof desc, "tipo %u desconhecido", tipo);
        log_op(s, desc, ST_BAD_REQUEST);
        responder(s, ST_BAD_REQUEST);
        break;
    }
    return 1;
}

/* Roda no processo filho: atende UM cliente ate ele desconectar. */
static void atender_cliente(int cfd, const char *origem)
{
    static uint8_t buf[MAX_PAYLOAD];
    Sessao s;
    uint8_t tipo;
    uint32_t len;
    char motivo[128] = "encerrado";

    memset(&s, 0, sizeof s);
    s.fd = cfd;
    snprintf(s.origem, sizeof s.origem, "%s", origem);

    log_msg("INFO", "conexao aceita de %s", origem);
    for (;;) {
        int r = recv_msg(cfd, &tipo, buf, sizeof buf, &len);
        if (r == 0) {
            snprintf(motivo, sizeof motivo, "cliente fechou a conexao sem QUIT");
            break;
        }
        if (r == -1) {
            if (errno == ETIMEDOUT)      /* o keepalive esgotou: a maquina do cliente sumiu */
                snprintf(motivo, sizeof motivo, "cliente inacessivel (sem resposta ao keepalive)");
            else
                snprintf(motivo, sizeof motivo, "erro de comunicacao: %s", strerror(errno));
            log_msg("ERROR", "%s: %s", s.identificado ? s.id : origem, motivo);
            break;
        }
        if (r == -2) {
            snprintf(motivo, sizeof motivo, "mensagem invalida (tamanho maior que o permitido)");
            log_msg("ERROR", "%s: %s", s.identificado ? s.id : origem, motivo);
            break;
        }
        if (!tratar(&s, tipo, buf, len)) {
            snprintf(motivo, sizeof motivo, "cliente enviou QUIT");
            break;
        }
        if (s.falha_envio) {
            snprintf(motivo, sizeof motivo, "erro ao enviar resposta");
            log_msg("ERROR", "%s: %s", s.identificado ? s.id : origem, motivo);
            break;
        }
    }

    /* Limpeza ao sair, por QUIT, queda ou erro: libera as reservas do cliente
     * e remove o id. Este e o tratamento de desconexao inesperada. */
    int n = 0;
    if (s.identificado) {
        char liberados[MAX_RECURSOS][NOME_MAX + 1];
        n = est_desconectar(s.id, liberados, MAX_RECURSOS);
        for (int i = 0; i < n && i < MAX_RECURSOS; i++)
            log_msg("INFO", "liberacao automatica: '%s' (dono '%s' saiu)", liberados[i], s.id);
    }
    /* A desconexao e SEMPRE registrada, com o motivo. */
    log_msg("INFO", "desconexao de %s@%s (%s); %d reserva(s) liberada(s)",
            s.identificado ? s.id : "?", origem, motivo, n);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

/* Recolhe filhos terminados (evita zumbis) e atualiza o contador de conexoes
 * ativas. Usa apenas operacoes seguras em tratador de sinal. */
static void ao_terminar_filho(int sig)
{
    (void)sig;
    int errno_salvo = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        est_conexao_encerrada();
    errno = errno_salvo;
}

static void ao_receber_sinal(int sig)
{
    (void)sig;
    g_stop = 1;
}

int main(int argc, char **argv)
{
    int porta = PORTA_PADRAO, opt;
    const char *logpath = NULL;

    while ((opt = getopt(argc, argv, "p:l:h")) != -1) {
        switch (opt) {
        case 'p': porta = atoi(optarg); break;
        case 'l': logpath = optarg; break;
        default:
            fprintf(stderr, "uso: %s [-l arquivo_de_log] [porta]\n", argv[0]);
            return opt == 'h' ? 0 : 2;
        }
    }
    if (optind < argc)
        porta = atoi(argv[optind]);
    if (porta <= 0 || porta > 65535) {
        fprintf(stderr, "porta invalida\n");
        return 2;
    }
    if (logpath) {
        /* O_APPEND: cada write() vai para o fim do arquivo, mesmo com varios processos. */
        g_logfd = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (g_logfd < 0) {
            perror("open log");
            return 1;
        }
    }

    /* Escrever o log em uma saida fechada (ex.: servidor | head) devolve EPIPE
     * em vez de matar o processo com SIGPIPE. */
    signal(SIGPIPE, SIG_IGN);

    /* A memoria compartilhada TEM que existir antes do primeiro fork. */
    est_init();

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = ao_terminar_filho;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = ao_receber_sinal;
    sa.sa_flags = 0;                     /* sem SA_RESTART: accept() retorna EINTR */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }
    int um = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &um, sizeof um);

    struct sockaddr_in end;
    memset(&end, 0, sizeof end);
    end.sin_family = AF_INET;
    end.sin_addr.s_addr = htonl(INADDR_ANY);
    end.sin_port = htons((uint16_t)porta);
    if (bind(lfd, (struct sockaddr *)&end, sizeof end) < 0) { perror("bind"); return 1; }
    if (listen(lfd, 128) < 0) { perror("listen"); return 1; }
    log_msg("INFO", "servidor escutando na porta %d", porta);

    while (!g_stop) {
        struct sockaddr_in cli;
        socklen_t tam = sizeof cli;
        int cfd = accept(lfd, (struct sockaddr *)&cli, &tam);
        if (cfd < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            log_msg("ERROR", "accept: %s", strerror(errno));
            usleep(100000);              /* ex.: EMFILE: evita laco quente */
            continue;
        }

        char ip[INET_ADDRSTRLEN], origem[64];
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof ip);
        snprintf(origem, sizeof origem, "%s:%d", ip, ntohs(cli.sin_port));

        net_keepalive(cfd);              /* detecta cliente cuja maquina sumiu sem fechar a conexao */
        est_conexao_aberta();            /* ANTES do fork: o SIGCHLD pode chegar a qualquer momento */
        pid_t pid = fork();
        if (pid < 0) {
            log_msg("ERROR", "fork: %s (conexao de %s recusada)", strerror(errno), origem);
            est_conexao_encerrada();
            close(cfd);
        } else if (pid == 0) {
            /* FILHO: nao precisa do socket de escuta; Ctrl+C volta ao padrao. */
            close(lfd);
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
            atender_cliente(cfd, origem);
            close(cfd);
            _exit(0);
        } else {
            /* PAI: nao usa o socket do cliente. Se nao fechar aqui, o socket
             * continua aberto no pai e o fim da conexao nunca e detectado. */
            close(cfd);
        }
    }

    log_msg("INFO", "encerrando servidor (sinal recebido)");
    close(lfd);
    if (g_logfd >= 0)
        close(g_logfd);
    return 0;
}
