# Serviço de Coordenação Distribuída

Trabalho Prático de Soquetes (MAC 5910): servidor e clientes em C, TCP, com um protocolo binário próprio chamado **CSCBP** (Central Server & Custom Binary Protocol).
O servidor usa um processo por cliente (`fork`) e mantém o estado em memória compartilhada (`mmap`), protegida por um mutex robusto entre processos. Os clientes são distribuídos: podem rodar em outras máquinas e só falam com o servidor por mensagens TCP. Se a máquina de um cliente sumir sem fechar a conexão, o servidor percebe pelo TCP keepalive (cerca de 60 s) e libera as reservas dele.

Requisitos: Linux, `gcc` e `make` (testado em Ubuntu 24.04).

## Como reproduzir os resultados

Passo a passo para compilar, executar e reproduzir os resultados do relatório em um Linux novo (testado em Ubuntu 24.04). Cada passo indica se o comando é digitado no *shell* ou no prompt de um cliente (`alice>`, `bob>` ou `dave>`); um comando de *shell* digitado no prompt de um cliente não funciona.

### Requisitos e compilação

**Passo 1.** Instalar as ferramentas (Ubuntu ou Debian), no *shell*:

```
sudo apt update
sudo apt install build-essential git
```

**Passo 2.** Obter o código-fonte, **[endereço do repositório git]** (ou descompactar o arquivo entregue), e entrar na pasta `soquetes`:

```
git clone <URL_DO_REPOSITORIO>
cd <PASTA_CRIADA_PELO_CLONE>
```

**Passo 3.** Compilar:

```
make
```

Devem aparecer três linhas `gcc`, sem nenhum aviso, e são criados os programas `server`, `client` e `tester`.

### Execução manual (demonstração)

Abrir quatro terminais, todos na pasta do projeto: o terminal 1 para o servidor, os terminais 2 e 3 para os clientes e o terminal 4 para comandos de *shell*.

**Passo 4.** Terminal 1, iniciar o servidor com log em arquivo. A primeira linha impressa é `servidor escutando na porta 5000`, com data e hora:

```
./server -l server.log
```

**Passo 5.** Terminais 2 e 3, iniciar dois clientes. Cada um imprime `OK`, a resposta ao `HELLO`:

```
./client 127.0.0.1 5000 alice
./client 127.0.0.1 5000 bob
```

**Passo 6.** Múltiplos clientes: no terminal 4, ver o log do servidor, que mostra os dois clientes atendidos por processos filhos com pids diferentes:

```
tail -n 20 server.log
```

**Passo 7.** Criar, alterar e reservar, no prompt da `alice`:

```
CREATE contador 10
SET contador 11
GET contador
RESERVE contador
LIST
```

Respostas: `OK`, `OK`, `OK 11 (inteiro)`, `OK` e a lista com `contador` `[RESERVADO por alice]`.

**Passo 8.** Concorrência, no prompt do `bob`:

```
RESERVE contador
SET contador 5
RELEASE contador
```

Respostas: `ERRO 6 BUSY`, `ERRO 6 BUSY` e `ERRO 8 NOT_OWNER`.

**Passo 9.** Falha de cliente: no terminal da `alice`, apertar Ctrl+C, sem digitar `QUIT`. O log do servidor passa a mostrar `liberacao automatica: 'contador' (dono 'alice' saiu)`. No prompt do `bob`:

```
RESERVE contador
LIST
```

Respostas: `OK` e a lista com `contador` `[RESERVADO por bob]`.

**Passo 10.** Protocolo binário, no *shell* do terminal 4, iniciar um cliente que mostra as mensagens em hexadecimal:

```
HEX=1 ./client 127.0.0.1 5000 dave
```

No prompt do `dave`, digitar `CREATE k oi` e `CREATE n 42`. As mensagens enviadas aparecem assim:

```
  enviei  : tamanho=00 00 00 08 | tipo=02 | 00 01 6b 00 00 02 6f 69
  enviei  : tamanho=00 00 00 0c | tipo=02 | 00 01 6e 01 00 00 00 00 00 00 00 2a
```

**Passo 11.** Monitoramento: em qualquer cliente, digitar `STATS`. São mostrados o tempo ativo, as conexões, os recursos, as requisições, as respostas de erro e a contagem por comando. Para acompanhar a cada segundo, no *shell*:

```
while true; do echo STATS; sleep 1; done | ./client 127.0.0.1 5000 monitor
```

**Passo 12.** Encerrar: `QUIT` em cada cliente (ou Ctrl+D) e Ctrl+C no servidor.

### Execução em duas máquinas

Duas máquinas na mesma rede (ou uma máquina virtual em modo *bridge*). O servidor escuta em todas as interfaces, e o cliente aceita o IP ou o nome da máquina do servidor.

**Passo 13.** Máquina A, descobrir o IP e iniciar o servidor. Se houver *firewall*, liberar a porta:

```
hostname -I
./server -l server.log
sudo ufw allow 5000/tcp
```

**Passo 14.** Máquina B, compilar o projeto e conectar no IP da máquina A:

```
make
./client IP_DA_MAQUINA_A 5000 alice
```

Repetir os passos 7 a 9 com clientes nas duas máquinas. O log da máquina A mostra `conexao aceita de` seguido do IP da máquina B.

### Testes automáticos e desempenho

**Passo 15.** Executar todos os testes. O comando sobe um servidor próprio, na porta 5055, e leva poucos segundos; termina com `TODOS OS TESTES PASSARAM`:

```
make test
```

**Passo 16.** Medir o desempenho com 1, 10, 50, 100 e 200 clientes (cerca de dez segundos). Imprime a tabela de latência e vazão e grava `tests/bench_resultado.csv`:

```
make bench
```

**Passo 17.** Executar um teste isolado, com o servidor iniciado no passo 4 em execução na porta 5000:

```
./tester func      127.0.0.1 5000
./tester race      127.0.0.1 5000 10
./tester crash     127.0.0.1 5000
./tester counter   127.0.0.1 5000 10 50
./tester malformed 127.0.0.1 5000
./tester limit     127.0.0.1 5000
./tester load      127.0.0.1 5000 10 100
./tester keepalive 127.0.0.1 5000
```

O teste `limit` exige que não haja nenhum outro cliente conectado, porque ele ocupa os 256 IDs; para os demais isso não importa. Para remover os programas compilados, `make clean`.

**Passo 18.** Dez clientes de verdade: o script abre 10 processos `./client` (`c1` a `c10`) que pedem o `RESERVE` do mesmo recurso ao mesmo tempo. Com o servidor do passo 4 em execução, em outro terminal:

```
./tests/dez_clientes.sh
```

O script mostra a resposta de cada cliente e as linhas do log do servidor (`server.log`) sobre a disputa, e termina com `PASSOU` quando exatamente um cliente recebe `OK` e os outros nove recebem `BUSY`.

**Passo 19.** Cliente que some da rede, simulado (opcional; precisa de `sudo` e leva cerca de 70 s). O script desliga o loopback dentro de uma rede isolada e se recusa a rodar fora dela; termina com `[PASS]`:

```
sudo unshare -n python3 tests/rede_fora.py
```

### Problemas comuns

- `Address already in use`: já há um servidor na porta 5000. Descobrir o processo com `ss -ltnp | grep :5000` e encerrá-lo com `kill`, ou usar outra porta (`./server 5001` e `./client 127.0.0.1 5001 alice`). Para `make test` e `make bench`, a porta se troca com a variável `PORT`, por exemplo `PORT=6000 make test`.
- `Arquivo ou diretório inexistente` ao executar `./client`: o terminal não está na pasta do projeto, ou o `make` ainda não foi executado.
- `comando desconhecido` no prompt de um cliente: o comando digitado é de *shell*; deve ser digitado em outro terminal.
- `ERRO 3 ID_IN_USE`: já existe um cliente conectado com esse ID; escolher outro.
- O cliente aceita o IP ou o nome da máquina do servidor (por exemplo, `127.0.0.1` ou `localhost`).
- `nao foi possivel conectar` a um servidor de outra máquina: conferir o IP (`hostname -I` na máquina do servidor), se o servidor está em execução e se o *firewall* libera a porta (`sudo ufw allow 5000/tcp`).

## Referência rápida

Comandos do cliente:

    CREATE nome valor     cria um recurso (valor: inteiro como 42, ou texto)
    GET nome              lê o valor
    SET nome valor        altera o valor (recurso livre ou reservado por você)
    RESERVE nome          reserva o recurso
    RELEASE nome          libera uma reserva sua
    LIST [FREE|RESERVED]  lista todos, só os livres ou só os reservados
    STATS                 estatísticas do servidor (monitoramento)
    QUIT                  encerra e libera suas reservas

Valores: inteiros em forma canônica (`42`, `-7`) viajam como inteiro de 64 bits; qualquer outro texto viaja como string. Aspas forçam string: `SET codigo "123"`.

## Arquivos

| Arquivo | Conteúdo |
|---|---|
| `common.h` | Constantes, tipos de requisição, status e o tipo `Valor` |
| `net.c/h` | Envio e recebimento de mensagens completas sobre TCP; conexão por IP ou nome; TCP keepalive |
| `proto.c/h` | Codificação de campos (textos, inteiros, valores) com verificação de limites |
| `estado.c/h` | Estado compartilhado, mutex e contadores atômicos |
| `server.c` | Servidor: `fork` por cliente, comandos, log com data e hora |
| `client.c` | Cliente interativo |
| `tester.c` | Testes automáticos e medição de desempenho |
| `tests/` | `run_tests.sh`, `bench.sh`, `dez_clientes.sh` e `rede_fora.py` |
