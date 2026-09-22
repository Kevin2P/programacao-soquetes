#!/bin/bash
# Testes automaticos: sobe o servidor, roda todos os cenarios e mostra PASS/FAIL.
# Uso: ./tests/run_tests.sh   (ou: make test)
cd "$(dirname "$0")/.." || exit 1
PORT=${PORT:-5055}
HOST=127.0.0.1
LOG=tests/server_test.log
rm -f "$LOG"

./server -l "$LOG" "$PORT" > /dev/null &
SRV=$!
trap 'kill $SRV 2>/dev/null; wait $SRV 2>/dev/null' EXIT
sleep 0.5

FAIL=0
secao() { echo; echo "=== $1 ==="; }
rodar() { "$@" || FAIL=1; }

secao "Demonstracao: todos os comandos, tipos de valor e erros (cliente alice)"
printf '%s\n' \
  'CREATE contador 10' 'CREATE nome Maria da Silva' 'CREATE codigo "123"' \
  'GET contador' 'GET nome' 'GET codigo' 'SET contador 11' 'GET contador' \
  'CREATE contador 5' 'RESERVE contador' 'RESERVE contador' 'LIST' 'LIST FREE' 'LIST RESERVED' \
  'RELEASE contador' 'RELEASE contador' 'GET nao_existe' 'FOO' 'STATS' 'QUIT' \
  | ./client $HOST "$PORT" alice

secao "Teste funcional automatico"
rodar ./tester func $HOST "$PORT"

secao "Teste de concorrencia: 10 clientes disputam 1 recurso"
rodar ./tester race $HOST "$PORT" 10

secao "Teste de falha: cliente cai com reserva ativa"
rodar ./tester crash $HOST "$PORT"

secao "Teste de consistencia: contador compartilhado (10 clientes x 50 incrementos)"
rodar ./tester counter $HOST "$PORT" 10 50

secao "Teste de comunicacao: mensagens invalidas"
rodar ./tester malformed $HOST "$PORT"

secao "Teste de carga: 10 clientes simultaneos"
# a saida passa por um arquivo (e nao por um pipe) para que o status do teste nao se perca
SAIDA=$(mktemp)
./tester load $HOST "$PORT" 10 100 > "$SAIDA"; RC=$?
grep -v '^CSV,' "$SAIDA"
rm -f "$SAIDA"
[ $RC -eq 0 ] || FAIL=1

secao "Teste de limite: cliente numero $((256 + 1)) e recusado"
rodar ./tester limit $HOST "$PORT"

secao "Teste de keepalive: deteccao de cliente que some sem fechar a conexao"
rodar ./tester keepalive $HOST "$PORT"

secao "Monitoramento do servidor apos os testes (STATS)"
printf 'STATS\nQUIT\n' | ./client $HOST "$PORT" monitor | tail -n +2

secao "Ultimas linhas do log do servidor ($LOG)"
tail -n 10 "$LOG"
echo
echo "Linhas de log: $(wc -l < "$LOG"); ERROR: $(grep -c ' ERROR ' "$LOG"); WARN: $(grep -c ' WARN ' "$LOG")"
echo
[ $FAIL -eq 0 ] && echo "TODOS OS TESTES PASSARAM" || echo "HA TESTES FALHANDO"
exit $FAIL
