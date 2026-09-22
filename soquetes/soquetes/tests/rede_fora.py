#!/usr/bin/env python3
"""Cliente que some da rede sem fechar a conexao (queda de energia, cabo desligado).

A queda e simulada desligando a interface de loopback dentro de um espaco de rede
isolado: nenhum FIN nem RST e enviado. O servidor deve perceber pelo TCP keepalive
(cerca de 60 s com os valores padrao), registrar "cliente inacessivel" e liberar as
reservas do cliente que sumiu.

Uso (precisa de root; leva cerca de 70 s):

    sudo unshare -n python3 tests/rede_fora.py

O "unshare -n" e obrigatorio: ele cria uma rede isolada, para nao mexer na rede do
computador. O script se recusa a rodar fora dela.
"""
import fcntl
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

SIOCGIFFLAGS, SIOCSIFFLAGS, IFF_UP = 0x8913, 0x8914, 1
PORTA = 5600
LIMITE_S = 150                       # nenhum passo espera mais que isso
RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVIDOR = os.environ.get("COORD_SERVER", os.path.join(RAIZ, "server"))
STATUS = {0: "OK", 1: "BAD_REQUEST", 3: "ID_IN_USE", 4: "NOT_FOUND", 6: "BUSY"}


def loopback(ligada):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    flags = struct.unpack("16sh", fcntl.ioctl(s, SIOCGIFFLAGS, struct.pack("16sh", b"lo", 0)))[1]
    flags = (flags | IFF_UP) if ligada else (flags & ~IFF_UP)
    fcntl.ioctl(s, SIOCSIFFLAGS, struct.pack("16sh", b"lo", flags))
    s.close()


def rede_isolada():
    """True se a unica interface de rede deste processo e o loopback.
    (/proc/net/dev mostra a rede do processo; /sys/class/net nao serve, pois
    continua mostrando as interfaces do computador dentro do "unshare -n".)"""
    nomes = [l.split(":")[0].strip() for l in open("/proc/net/dev").read().split("\n")[2:] if ":" in l]
    return nomes == ["lo"]


def quadro(tipo, payload=b""):
    return struct.pack(">IB", len(payload), tipo) + payload


def texto(x):
    x = x.encode()
    return struct.pack(">H", len(x)) + x


def receber(c):
    cab = b""
    while len(cab) < 5:
        d = c.recv(5 - len(cab))
        if not d:
            return None
        cab += d
    n, tipo = struct.unpack(">IB", cab)
    corpo = b""
    while len(corpo) < n:
        corpo += c.recv(n - len(corpo))
    return tipo


def chamar(c, tipo, payload=b""):
    c.sendall(quadro(tipo, payload))
    r = receber(c)
    return STATUS.get(r, r)


def conectar(cliente_id):
    c = socket.create_connection(("127.0.0.1", PORTA))
    c.settimeout(LIMITE_S)
    return c, chamar(c, 1, texto(cliente_id))


def main():
    if os.geteuid() != 0:
        print("execute como root, dentro de uma rede isolada:  sudo unshare -n python3 tests/rede_fora.py")
        return 2
    if not rede_isolada():
        print("recusado: este script desliga o loopback e so pode rodar em uma rede isolada.")
        print("use:  sudo unshare -n python3 tests/rede_fora.py")
        return 2
    if not os.path.exists(SERVIDOR):
        print("servidor nao encontrado (%s); rode 'make' antes" % SERVIDOR)
        return 2

    log = tempfile.NamedTemporaryFile(prefix="rede_fora_", suffix=".log", delete=False).name
    loopback(True)                               # em uma rede nova, o loopback nasce desligado
    srv = subprocess.Popen([SERVIDOR, "-l", log, str(PORTA)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ok = False
    try:
        time.sleep(0.7)
        print("Cliente que some da rede sem fechar a conexao")
        alice, st = conectar("alice")
        print("  alice: HELLO -> %s" % st)
        print("  alice: CREATE lock1 -> %s" % chamar(alice, 2, texto("lock1") + b"\x00" + texto("x")))
        print("  alice: RESERVE lock1 -> %s" % chamar(alice, 5, texto("lock1")))
        bob, st = conectar("bob")
        print("  bob:   RESERVE lock1 -> %s (a alice tem a reserva)" % chamar(bob, 5, texto("lock1")))

        print("\nRede fora do ar, sem FIN nem RST. Aguardando o keepalive do servidor...")
        loopback(False)
        t0 = time.time()
        detectou = False
        while time.time() - t0 < LIMITE_S:
            time.sleep(1)
            if "alice: cliente inacessivel" in open(log).read():
                detectou = True
                break
        espera = time.time() - t0
        loopback(True)
        print("  reserva liberada automaticamente apos %.0f s" % espera if detectou else "  o servidor nao detectou a queda em %d s" % LIMITE_S)

        time.sleep(0.5)
        carol, st = conectar("carol")
        r = chamar(carol, 5, texto("lock1"))
        print("  carol: RESERVE lock1 -> %s" % r)
        print("\nLog do servidor:")
        for linha in open(log):
            if any(k in linha for k in ("inacessivel", "liberacao")):
                print("  " + linha.rstrip())
        ok = detectou and r == "OK"
    finally:
        try:
            loopback(True)
        except OSError:
            pass
        srv.terminate()
        try:
            srv.wait(timeout=5)
        except subprocess.TimeoutExpired:
            srv.kill()
        os.unlink(log)
    print("[%s] rede fora: cliente que some sem fechar a conexao e detectado e as reservas sao liberadas" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
