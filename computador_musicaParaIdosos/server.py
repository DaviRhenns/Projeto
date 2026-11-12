#!/usr/bin/env python3
"""
Servidor local do Meu Instrumento (só biblioteca padrão, Python 3.8+).

    python3 server.py

- http://localhost:8000      -> tela de entrada (login, criar conta ou visitante)
- http://localhost:8000/app  -> o instrumento
- O ESP32 avisa cada botão apertado em  GET /api/botao?botao=N  (N de 0 a 24)

As contas ficam no arquivo usuarios.db (SQLite), criado ao lado deste script.
As senhas NUNCA são guardadas: só um hash scrypt com sal aleatório.
"""
import hashlib
import hmac
import json
import os
import queue
import re
import secrets
import socket
import sqlite3
import threading
import time
from contextlib import closing
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

PASTA = Path(__file__).parent
PORTA = int(os.environ.get("PORTA", 8000))
DB = PASTA / "usuarios.db"
SESSAO_DIAS = 30
MAX_TENTATIVAS, BLOQUEIO_S = 5, 60
RE_USUARIO = re.compile(r"^[\w.\-]{3,30}$")

clientes, trava = [], threading.Lock()   # fluxo de eventos (ESP32 -> página)
falhas = {}                              # (ip, usuario) -> (contagem, bloqueado_até)


# ---------- banco de dados ----------
def db():
    return closing(sqlite3.connect(DB, timeout=10))


def iniciar_db():
    with db() as c:
        c.executescript("""
        CREATE TABLE IF NOT EXISTS usuarios(
            id INTEGER PRIMARY KEY, nome TEXT NOT NULL, chave TEXT NOT NULL UNIQUE,
            senha_hash TEXT NOT NULL, criado INTEGER NOT NULL);
        CREATE TABLE IF NOT EXISTS sessoes(
            token_hash TEXT PRIMARY KEY, usuario_id INTEGER NOT NULL, expira INTEGER NOT NULL);
        """)
        c.commit()


# ---------- senhas ----------
def gerar_hash(senha):
    sal = secrets.token_bytes(16)
    h = hashlib.scrypt(senha.encode(), salt=sal, n=2**14, r=8, p=1, dklen=32)
    return f"scrypt$16384$8$1${sal.hex()}${h.hex()}"


def conferir_senha(senha, armazenado):
    try:
        _, n, r, p, sal, h = armazenado.split("$")
        novo = hashlib.scrypt(senha.encode(), salt=bytes.fromhex(sal),
                              n=int(n), r=int(r), p=int(p), dklen=32)
        return hmac.compare_digest(novo, bytes.fromhex(h))
    except (ValueError, TypeError):
        return False


HASH_FALSO = gerar_hash("senha-falsa")   # gasta o mesmo tempo quando o usuário não existe


# ---------- sessões ----------
def sha(txt):
    return hashlib.sha256(txt.encode()).hexdigest()


def criar_sessao(uid):
    token = secrets.token_urlsafe(32)
    with db() as c:
        c.execute("DELETE FROM sessoes WHERE expira < ?", (time.time(),))
        c.execute("INSERT INTO sessoes VALUES(?,?,?)",
                  (sha(token), uid, int(time.time()) + SESSAO_DIAS * 86400))
        c.commit()
    return token


def cookie_sessao(token):
    return ("Set-Cookie", f"sessao={token}; Path=/; HttpOnly; SameSite=Lax; Max-Age={SESSAO_DIAS * 86400}")


LIMPAR = [("Set-Cookie", "sessao=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0"),
          ("Set-Cookie", "visitante=; Path=/; SameSite=Lax; Max-Age=0")]


# ---------- eventos do ESP32 ----------
def transmitir(msg):
    with trava:
        for q in clientes:
            q.put(msg)


def ip_local():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"


class Handler(BaseHTTPRequestHandler):
    # ----- utilidades -----
    def responder(self, codigo, corpo, tipo="text/plain; charset=utf-8", extra=()):
        if isinstance(corpo, str):
            corpo = corpo.encode()
        self.send_response(codigo)
        self.send_header("Content-Type", tipo)
        self.send_header("Content-Length", str(len(corpo)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(corpo)

    def json_(self, codigo, obj, extra=()):
        self.responder(codigo, json.dumps(obj), "application/json", extra)

    def pagina(self, nome):
        self.responder(200, (PASTA / nome).read_bytes(), "text/html; charset=utf-8")

    def redirecionar(self, destino):
        self.responder(302, "", extra=[("Location", destino)])

    def cookies(self):
        c = SimpleCookie()
        c.load(self.headers.get("Cookie", ""))
        return c

    def usuario_logado(self):
        c = self.cookies().get("sessao")
        if not c:
            return None
        with db() as d:
            linha = d.execute(
                "SELECT u.nome FROM sessoes s JOIN usuarios u ON u.id = s.usuario_id "
                "WHERE s.token_hash = ? AND s.expira > ?", (sha(c.value), time.time())).fetchone()
        return linha[0] if linha else None

    def ler_json(self):
        try:
            if not self.headers.get("Content-Type", "").startswith("application/json"):
                return None
            tam = int(self.headers.get("Content-Length", 0))
            if not 0 < tam <= 4096:
                return None
            dados = json.loads(self.rfile.read(tam))
            return dados if isinstance(dados, dict) else None
        except (ValueError, TypeError):
            return None

    # ----- GET -----
    def do_GET(self):
        url = urlparse(self.path)
        rota = url.path
        if rota == "/":
            if self.usuario_logado():
                self.redirecionar("/app")
            else:
                self.pagina("login.html")
        elif rota in ("/app", "/index.html"):
            if self.usuario_logado() or "visitante" in self.cookies():
                self.pagina("index.html")
            else:
                self.redirecionar("/")
        elif rota == "/api/eu":
            self.json_(200, {"usuario": self.usuario_logado()})
        elif rota == "/api/botao":
            try:
                botao = int(parse_qs(url.query)["botao"][0])
                if not 0 <= botao < 25:
                    raise ValueError
            except (KeyError, ValueError):
                return self.responder(400, "botao invalido")
            print(f"ESP32: botão {botao + 1}")
            transmitir(botao)
            self.responder(200, "ok")
        elif rota == "/eventos":
            self.fluxo_eventos()
        else:
            self.responder(404, "nao encontrado")

    def fluxo_eventos(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        q = queue.Queue()
        with trava:
            clientes.append(q)
        try:
            while True:
                try:
                    self.wfile.write(f"data: {q.get(timeout=15)}\n\n".encode())
                except queue.Empty:
                    self.wfile.write(b": ping\n\n")
                self.wfile.flush()
        except OSError:
            pass
        finally:
            with trava:
                clientes.remove(q)

    # ----- POST -----
    def do_POST(self):
        dados = self.ler_json()
        if dados is None:
            return self.json_(400, {"erro": "Pedido inválido."})
        rota = self.path
        if rota == "/api/cadastro":
            self.cadastro(dados)
        elif rota == "/api/login":
            self.login(dados)
        elif rota == "/api/visitante":
            self.json_(200, {"ok": True}, [("Set-Cookie", "visitante=1; Path=/; SameSite=Lax")])
        elif rota == "/api/logout":
            c = self.cookies().get("sessao")
            if c:
                with db() as d:
                    d.execute("DELETE FROM sessoes WHERE token_hash = ?", (sha(c.value),))
                    d.commit()
            self.json_(200, {"ok": True}, LIMPAR)
        else:
            self.json_(404, {"erro": "Não encontrado."})

    def cadastro(self, d):
        nome, senha = str(d.get("usuario", "")).strip(), str(d.get("senha", ""))
        if not RE_USUARIO.match(nome):
            return self.json_(400, {"erro": "O nome de usuário deve ter de 3 a 30 letras ou números, sem espaços."})
        if not 6 <= len(senha) <= 128:
            return self.json_(400, {"erro": "A senha deve ter pelo menos 6 caracteres."})
        try:
            with db() as c:
                cur = c.execute("INSERT INTO usuarios(nome, chave, senha_hash, criado) VALUES(?,?,?,?)",
                                (nome, nome.lower(), gerar_hash(senha), int(time.time())))
                c.commit()
                uid = cur.lastrowid
        except sqlite3.IntegrityError:
            return self.json_(409, {"erro": "Esse nome de usuário já existe. Escolha outro."})
        print(f"conta criada: {nome}")
        self.json_(200, {"ok": True}, [cookie_sessao(criar_sessao(uid)),
                                       ("Set-Cookie", "visitante=; Path=/; Max-Age=0")])

    def login(self, d):
        nome, senha = str(d.get("usuario", "")).strip(), str(d.get("senha", ""))
        chave = (self.client_address[0], nome.lower())
        cont, ate = falhas.get(chave, (0, 0))
        if ate > time.time():
            return self.json_(429, {"erro": "Muitas tentativas. Espere 1 minuto e tente de novo."})
        with db() as c:
            linha = c.execute("SELECT id, senha_hash FROM usuarios WHERE chave = ?", (nome.lower(),)).fetchone()
        ok = conferir_senha(senha, linha[1] if linha else HASH_FALSO)
        if not (linha and ok):
            cont += 1
            falhas[chave] = (0, time.time() + BLOQUEIO_S) if cont >= MAX_TENTATIVAS else (cont, 0)
            return self.json_(401, {"erro": "Usuário ou senha incorretos."})
        falhas.pop(chave, None)
        self.json_(200, {"ok": True}, [cookie_sessao(criar_sessao(linha[0])),
                                       ("Set-Cookie", "visitante=; Path=/; Max-Age=0")])

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    iniciar_db()
    print("Meu Instrumento rodando!")
    print(f"  no computador        : http://localhost:{PORTA}")
    print(f"  endereço p/ o ESP32  : {ip_local()}  (porta {PORTA})")
    print("Ctrl+C para parar.\n")
    try:
        ThreadingHTTPServer(("0.0.0.0", PORTA), Handler).serve_forever()
    except KeyboardInterrupt:
        print("\nEncerrado.")
