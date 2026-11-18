#!/usr/bin/env python3
"""
Baixa pelo USB os registros guardados na memória do ESP32 (Estação Ambiental)
e salva em um arquivo .csv.

Uso:
    python3 baixar_dados.py                  # detecta a porta e salva estacao_AAAA-MM-DD_HHMM.csv
    python3 baixar_dados.py --porta /dev/ttyUSB0 --saida dados.csv
    python3 baixar_dados.py --br             # CSV no formato do Excel brasileiro (; e vírgula decimal)
    python3 baixar_dados.py --apagar         # depois de salvar, apaga a memória do ESP32

Antes de rodar: feche o Monitor Serial da IDE do Arduino (a porta só pode ser usada por um programa).
Requer pyserial:  sudo apt install python3-serial   (ou  pip install pyserial)
"""
import argparse
import csv
import glob
import struct
import sys
import time
from datetime import datetime

try:
    import serial
except ImportError:
    sys.exit("Falta o pyserial. Instale com:  sudo apt install python3-serial")

FORMATO = "<IHBBhHI"          # mesmo layout do struct Registro do firmware (16 bytes)
TAM_REG = struct.calcsize(FORMATO)


def decodificar(dados):
    """Converte o binário do ESP32 em linhas (dicionários) com data/hora reconstruída."""
    regs = [struct.unpack_from(FORMATO, dados, i)
            for i in range(0, len(dados) - len(dados) % TAM_REG, TAM_REG)]

    # hora unix no instante em que cada sessão (boot) começou
    base = {}
    for seg, boot, tipo, flags, temp, umid, valor in regs:
        if tipo == 1:
            base[boot] = valor - seg

    linhas = []
    for seg, boot, tipo, flags, temp, umid, valor in regs:
        if tipo != 0:
            continue
        unix = base[boot] + seg if boot in base else None
        linhas.append({
            "unix": unix,
            "temperatura": temp / 100 if flags & 1 else None,
            "umidade": umid / 100 if flags & 1 else None,
            "pressao": valor / 100 if flags & 2 else None,
            "sessao": boot,
            "seg": seg,
        })
    return linhas


def salvar_csv(linhas, caminho, br=False):
    sep = ";" if br else ","

    def num(v, casas):
        if v is None:
            return ""
        s = f"{v:.{casas}f}"
        return s.replace(".", ",") if br else s

    with open(caminho, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, delimiter=sep)
        w.writerow(["data_hora", "unix", "temperatura_C", "umidade_pct", "pressao_hPa",
                    "sessao", "seg_desde_ligar"])
        for l in linhas:
            dh = datetime.fromtimestamp(l["unix"]).strftime("%Y-%m-%d %H:%M:%S") if l["unix"] else ""
            w.writerow([dh, l["unix"] or "", num(l["temperatura"], 2), num(l["umidade"], 2),
                        num(l["pressao"], 2), l["sessao"], l["seg"]])


def achar_porta():
    portas = sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))
    if not portas:
        sys.exit("Nenhuma porta /dev/ttyUSB* ou /dev/ttyACM* encontrada. O ESP32 está ligado no USB?")
    if len(portas) > 1:
        print(f"Várias portas encontradas {portas}; usando {portas[0]} (use --porta para escolher).")
    return portas[0]


def ler_ate(ser, marca, tempo):
    """Lê da serial até encontrar 'marca'; devolve o que veio depois dela."""
    buf = b""
    limite = time.time() + tempo
    while time.time() < limite:
        buf += ser.read(ser.in_waiting or 1)
        i = buf.find(marca)
        if i >= 0:
            return buf[i + len(marca):]
    return None


def comando(ser, texto):
    ser.write((texto + "\n").encode())
    ser.flush()


def main():
    ap = argparse.ArgumentParser(description="Baixa os registros da Estação Ambiental pelo USB.")
    ap.add_argument("--porta", help="porta serial (padrão: detecta /dev/ttyUSB* ou /dev/ttyACM*)")
    ap.add_argument("--saida", help="arquivo .csv de saída")
    ap.add_argument("--br", action="store_true", help="formato Excel pt-BR (; como separador e vírgula decimal)")
    ap.add_argument("--apagar", action="store_true", help="apaga a memória do ESP32 depois de salvar")
    ap.add_argument("--sem-hora", action="store_true", help="não ajusta a hora do ESP32 com a do PC")
    a = ap.parse_args()

    porta = a.porta or achar_porta()
    saida = a.saida or datetime.now().strftime("estacao_%Y-%m-%d_%H%M.csv")

    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = porta, 115200, 0.2
    ser.dtr = False   # evita reiniciar o ESP32 ao abrir a porta
    ser.rts = False
    try:
        ser.open()
    except serial.SerialException as e:
        sys.exit(f"Não consegui abrir {porta}: {e}\nO Monitor Serial da IDE está aberto? Feche e tente de novo.")

    print(f"Conectado em {porta}. Aguardando o ESP32...")
    time.sleep(1.5)
    ser.reset_input_buffer()

    # confirma que o firmware com registro está respondendo (o ESP32 pode ter reiniciado)
    info = None
    for _ in range(8):
        comando(ser, "INFO")
        resto = ler_ate(ser, b"#INFO ", 1.5)
        if resto is not None:
            if b"\n" not in resto:
                resto += ser.readline()
            info = resto.split(b"\n", 1)[0].decode(errors="replace").strip()
            break
    if info is None:
        sys.exit("O ESP32 não respondeu. Confira se o firmware novo (com registro) foi gravado.")
    print("ESP32:", info)

    if not a.sem_hora:
        comando(ser, f"T{int(time.time())}")
        time.sleep(0.3)
        print("Hora do ESP32 ajustada com a do PC.")

    ser.reset_input_buffer()
    comando(ser, "DUMP")
    resto = ler_ate(ser, b"#DUMP ", 5)
    if resto is None:
        sys.exit("O ESP32 não começou a enviar os dados.")
    while b"\n" not in resto:
        resto += ser.read(1)
    cab, resto = resto.split(b"\n", 1)
    total = int(cab.strip())
    print(f"Recebendo {total} bytes ({total // TAM_REG} registros)...")

    dados = bytearray(resto[:total])
    inicio = time.time()
    ser.timeout = 5
    while len(dados) < total:
        pedaco = ser.read(min(4096, total - len(dados)))
        if not pedaco:
            sys.exit(f"Transferência interrompida ({len(dados)} de {total} bytes).")
        dados += pedaco
        pct = 100 * len(dados) // max(total, 1)
        print(f"\r  {pct:3d}%  {len(dados)}/{total} bytes", end="", flush=True)
    print(f"\n  concluído em {time.time() - inicio:.0f} s")

    linhas = decodificar(bytes(dados))
    salvar_csv(linhas, saida, br=a.br)
    sem_hora = sum(1 for l in linhas if l["unix"] is None)
    print(f"Salvo: {saida}  ({len(linhas)} leituras)")
    if linhas and linhas[0]["unix"] and linhas[-1]["unix"]:
        d0 = datetime.fromtimestamp(linhas[0]["unix"]).strftime("%d/%m/%Y %H:%M")
        d1 = datetime.fromtimestamp(linhas[-1]["unix"]).strftime("%d/%m/%Y %H:%M")
        print(f"Período: {d0}  até  {d1}")
    if sem_hora:
        print(f"Atenção: {sem_hora} leituras ficaram sem data/hora (o ESP32 foi ligado e desligado "
              "sem nunca receber a hora nessa sessão). Elas estão no CSV com a coluna data_hora vazia.")

    if a.apagar:
        r = input("Apagar todos os registros da memória do ESP32? Digite SIM para confirmar: ")
        if r.strip().upper() == "SIM":
            comando(ser, "CLEAR")
            time.sleep(0.5)
            print("Memória do ESP32 apagada.")
        else:
            print("Nada foi apagado.")
    ser.close()


if __name__ == "__main__":
    main()
