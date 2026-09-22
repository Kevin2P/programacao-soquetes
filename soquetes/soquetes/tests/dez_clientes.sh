#!/bin/bash
# 10 clientes de verdade (10 processos ./client) disputam o mesmo recurso ao mesmo tempo.
# O servidor precisa estar rodando.   Uso: ./tests/dez_clientes.sh [servidor] [porta]
# No final, mostra as linhas do log do servidor sobre a disputa (arquivo server.log, o mesmo
# do './server -l server.log'; outro arquivo: LOG=outro.log ./tests/dez_clientes.sh).
cd "$(dirname "$0")/.." || exit 1
HOST=${1:-127.0.0.1}
PORTA=${2:-5000}
N=10
RES="disputa$$"
TMP=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$TMP"' EXIT

# cria o recurso que todos vao disputar
if ! printf 'CREATE %s 0\nQUIT\n' "$RES" | ./client "$HOST" "$PORTA" preparo > "$TMP/prep" 2>&1; then
    echo "nao foi possivel falar com o servidor em $HOST:$PORTA. Ele esta rodando?"
    exit 2
fi

echo "Conectando $N clientes (c1 a c$N) em $HOST:$PORTA..."
for i in $(seq 1 $N); do
    # cada cliente espera o arquivo "go" aparecer e so entao envia o RESERVE
    ( while [ ! -e "$TMP/go" ]; do sleep 0.01; done; echo "RESERVE $RES"; sleep 1; echo QUIT ) \
        | ./client "$HOST" "$PORTA" "c$i" > "$TMP/saida$i" 2>&1 &
done
sleep 1.5
echo "Todos conectados. Disparando RESERVE $RES em todos ao mesmo tempo..."
touch "$TMP/go"
wait
echo

OK=0; BUSY=0
for i in $(seq 1 $N); do
    hello=$(sed -n 1p "$TMP/saida$i")
    resp=$(sed -n 2p "$TMP/saida$i")
    [ "$hello" = "OK" ] || resp="HELLO falhou: $hello"
    printf '  c%-2s -> %s\n' "$i" "$resp"
    case "$resp" in "OK") OK=$((OK+1));; *BUSY*) BUSY=$((BUSY+1));; esac
done
echo
echo "Resultado: $OK OK e $BUSY BUSY"

# linhas do log do servidor sobre esta disputa (espera um instante para o servidor gravar a ultima)
LOG=${LOG:-server.log}
sleep 0.3
echo
if [ -f "$LOG" ]; then
    echo "Log do servidor ($LOG), linhas desta disputa:"
    grep "$RES" "$LOG" | sed 's/^/  /'
else
    echo "(log nao encontrado em $LOG: inicie o servidor com  ./server -l server.log  para ver o log aqui)"
fi
echo
[ $OK -eq 1 ] && [ $BUSY -eq $((N-1)) ] && echo "PASSOU: so um cliente conseguiu a reserva" && exit 0
echo "FALHOU: esperado 1 OK e $((N-1)) BUSY"
exit 1
