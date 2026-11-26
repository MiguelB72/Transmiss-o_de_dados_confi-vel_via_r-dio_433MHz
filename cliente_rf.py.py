import io
import os
import sys
import time
import threading
import serial
import serial.tools.list_ports
from PIL import Image, ImageDraw

BAUD_RATE = 115200
ARQUIVO_SAIDA = "imagem_recebida.jpg"


def escolher_porta_serial():
    portas = list(serial.tools.list_ports.comports())
    if not portas:
        print("Nenhuma porta serial encontrada. Verifique o cabo USB do ESP32.")
        sys.exit(1)

    print("\nPortas seriais disponiveis:")
    for idx, p in enumerate(portas):
        print(f"  [{idx}] {p.device} - {p.description}")

    if len(portas) == 1:
        print(f"Selecionando automaticamente {portas[0].device}...")
        return portas[0].device

    escolha = int(input("Digite o numero da porta do ESP32: "))
    return portas[escolha].device


def preparar_imagem_compacta(caminho_arquivo=None, tamanho=(64, 64)):
    if caminho_arquivo and os.path.exists(caminho_arquivo):
        img = Image.open(caminho_arquivo).convert("RGB")
    else:
        img = Image.new("RGB", tamanho, "white")
        draw = ImageDraw.Draw(img)
        draw.rectangle([2, 2, 61, 61], outline="blue", width=3)
        draw.ellipse([14, 14, 50, 50], fill="gold", outline="black", width=2)
        draw.ellipse([23, 25, 27, 29], fill="black")
        draw.ellipse([37, 25, 41, 29], fill="black")
        draw.arc([22, 26, 42, 42], start=20, end=160, fill="black", width=2)

    img = img.resize(tamanho, Image.Resampling.LANCZOS)

    qualidade = 65
    while qualidade >= 20:
        buf = io.BytesIO()
        img.save(buf, format="JPEG", quality=qualidade, optimize=True)
        dados = buf.getvalue()
        if len(dados) <= 1200:
            return dados
        qualidade -= 10

    return dados


def escutar_esp32(ser):
    while True:
        try:
            linha = ser.readline().decode("utf-8", errors="replace").strip()
            if not linha:
                continue

            if linha.startswith("[IMG_HEX]:"):
                hex_dados = linha.split(":", 1)[1].strip()
                bytes_img = bytes.fromhex(hex_dados)

                with open(ARQUIVO_SAIDA, "wb") as f:
                    f.write(bytes_img)

                print(f"\n[PC_RECEPTOR] Imagem reconstruida com sucesso ({len(bytes_img)} bytes) -> '{ARQUIVO_SAIDA}'")
                img = Image.open(io.BytesIO(bytes_img))
                img.resize((256, 256), Image.Resampling.NEAREST).show()
            else:
                print(linha)
        except Exception as e:
            print(f"[ERRO_SERIAL] {e}")
            break


def main():
    porta = escolher_porta_serial()
    ser = serial.Serial(porta, BAUD_RATE, timeout=0.5)
    time.sleep(2)

    thread_rx = threading.Thread(target=escutar_esp32, args=(ser,), daemon=True)
    thread_rx.start()

    print("\n========================================================")
    print("  Cliente RF 433 MHz - Troca de Texto e Imagens")
    print("========================================================")
    print("Comandos:")
    print("  1 - Enviar mensagem de texto")
    print("  2 - Enviar mensagem de texto COM SIMULACAO DE ERRO")
    print("  3 - Enviar imagem (arquivo local ou imagem padrao de teste)")
    print("  4 - Enviar imagem COM SIMULACAO DE ERRO")
    print("  0 - Sair\n")

    while True:
        opcao = input("Escolha uma opcao: ").strip()

        if opcao == "1":
            msg = input("Digite o texto: ").strip()
            if msg:
                ser.write((msg + "\n").encode("utf-8"))

        elif opcao == "2":
            msg = input("Digite o texto para testar erro/retransmissao: ").strip()
            if msg:
                ser.write((f"!erro {msg}\n").encode("utf-8"))

        elif opcao in ("3", "4"):
            caminho = input("Caminho da imagem (ou pressione Enter para usar imagem de teste): ").strip()
            dados_img = preparar_imagem_compacta(caminho if caminho else None)
            hex_img = dados_img.hex()
            prefixo = "!img_erro " if opcao == "4" else "!img "

            print(f"[PC_TRANSMISSOR] Enviando {len(dados_img)} bytes ({(len(dados_img) + 15) // 16} quadros DATA)...")
            ser.write((prefixo + hex_img + "\n").encode("utf-8"))

        elif opcao == "0":
            ser.close()
            break


if __name__ == "__main__":
    main()