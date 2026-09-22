#!/bin/bash
# Medicao de desempenho: latencia e vazao variando o numero de clientes simultaneos.
# Cada rodada usa um servidor NOVO (a tabela de recursos e limitada a 256 e nao ha DELETE).
# Uso: ./tests/bench.sh [ciclos_por_cliente]   (ou: make bench)
cd "$(dirname "$0")/.." || exit 1
PORT=${PORT:-5056}
HOST=127.0.0.1
CICLOS=${1:-100}
OUT=tests/bench_resultado.csv

echo "clientes,ciclos,requisicoes,tempo_ms,req_por_s,media_ms,p50_ms,p95_ms,p99_ms,max_ms,erros" > "$OUT"
for N in 1 10 50 100 200; do
    ./server "$PORT" > /dev/null &
    SRV=$!
    sleep 0.5
    ./tester load $HOST "$PORT" $N "$CICLOS" | grep '^CSV,' | sed 's/^CSV,//' >> "$OUT"
    kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
    sleep 0.5
done

echo "Desempenho (cada cliente faz $CICLOS ciclos de SET, GET, RESERVE e RELEASE; latencia por requisicao):"
echo
awk -F, 'NR==1 { printf "%8s %7s %11s %10s %10s %9s %8s %8s %8s %8s %6s\n", "clientes","ciclos","requisicoes","tempo_ms","req/s","media_ms","p50_ms","p95_ms","p99_ms","max_ms","erros"; next }
     { printf "%8s %7s %11s %10s %10s %9s %8s %8s %8s %8s %6s\n", $1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11 }' "$OUT"
echo
echo "Arquivo CSV: $OUT"
