# Transmissão de Dados Confiável via Rádio (RF 433 MHz)

Sistema de comunicação sem fio bidirecional desenvolvido entre duas placas **ESP32** conectadas a dois computadores, utilizando exclusivamente módulos de radiofrequência (**RF 433 MHz**) em transmissão bruta (sem uso de bibliotecas prontas de rádio ou interfaces nativas como Wi-Fi e Bluetooth).

O sistema permite o envio confiável de **mensagens de texto** (exibidas no Monitor Serial e em um display LCD I2C no lado receptor) e **pequenas imagens** (fatiadas na origem e reconstruídas visualmente no computador de destino), implementando enquadramento customizado, detecção de erros via **Checksum** e recuperação de falhas via **Stop-and-Wait ARQ**.

---

## Hardware e Materiais Utilizados

* **2x** Placas de desenvolvimento ESP32
* **4x** Módulos RF 433 MHz ASK/OOK (2 transmissores FS1000A e 2 receptores MX-RM-5V)
* **1x** Display LCD 16x2 com módulo adaptador I2C (conectado ao ESP32 receptor)
* **2x** Computadores com Arduino IDE e Python 3 instalados
* **2x** Protoboards, cabos jumpers, resistores (divisor de tensão 5 V para 3,3 V), capacitores de desacoplamento e antenas de fio rígido (~17,3 cm)

### Pinagem do ESP32 (Idêntica nas Duas Placas)

Como ambas as placas operam com um par TX/RX para viabilizar o envio de dados e o retorno de confirmações (`ACK`), o mesmo firmware é gravado nos dois microcontroladores:

| Componente | Pino do Módulo | GPIO do ESP32 | Observação Técnica |
| :--- | :--- | :--- | :--- |
| **Módulo RF Transmissor (TX)** | `DATA` | `GPIO 33` | Alimentação em `5V` (VIN) para maior alcance |
| **Módulo RF Receptor (RX)** | `DATA` | `GPIO 32` | Utilizar divisor resistivo (`5V -> 3,3V`) no pino de sinal |
| **Display LCD I2C** | `SDA` | `GPIO 26` | Conectado apenas no ESP32 receptor (endereços `0x27` ou `0x3F`) |
| **Display LCD I2C** | `SCL` | `GPIO 27` | Linha de clock do barramento I2C |

---

## Arquitetura do Protocolo

### 1. Camada Física (RF Bruto)
* **Modulação e Codificação:** Transmissão digital direta em banda base utilizando **codificação Manchester** (`500 µs` por meio-bit, resultando em taxa efetiva de **1.000 bps**), permitindo ressincronização de clock a cada bit recebido.
* **Sincronismo:** Cada transmissão é precedida por um preâmbulo de **4 bytes** (`0xAA`) para estabilização do controle automático de ganho (AGC) do receptor, seguido pela palavra de sincronismo de **16 bits** (`0x2DD4`).
* **Operação Half-Duplex:** Durante qualquer envio, o microcontrolador mantém o pino `TX` em nível lógico baixo ao finalizar e sincroniza o estado ocioso do pino `RX` local para evitar capturar o próprio sinal.

### 2. Camada de Enlace (Estrutura do Quadro)
Todos os pacotes seguem o formato estrutural de até **21 bytes**, aceitando exclusivamente três tipos de quadro (`DATA`, `ACK` e `END`):

| Campo | Tamanho | Descrição |
| :--- | :--- | :--- |
| **FLAG** | 1 byte | Delimitador de início do quadro (`0x7E`) |
| **TYPE** | 1 byte | `0x01` (`DATA`), `0x02` (`ACK`) ou `0x03` (`END`) |
| **SEQ** | 1 byte | Número de sequência cíclico (`0` a `255`) |
| **LEN** | 1 byte | Quantidade de bytes úteis transportados em `DATA` (`0` a `16`) |
| **DATA** | 0 a 16 bytes | Carga útil da mensagem ou fragmento binário da imagem |
| **FCS** | 1 byte | *Frame Check Sequence* para verificação de integridade |

* **Diferenciação entre Texto e Imagem:** Para respeitar o limite de três tipos de quadro, o tipo de conteúdo viaja no campo `DATA` do quadro `END` (`LEN = 1`), utilizando `0x01` para texto e `0x02` para imagem.
* **Leitura Guiada por `LEN`:** Após detectar a `FLAG` inicial (`0x7E`), o receptor contabiliza exatamente os `LEN` bytes indicados no cabeçalho, evitando encerramento prematuro caso o byte `0x7E` ocorra naturalmente nos dados binários de uma imagem.

### 3. Detecção de Erros (FCS — Checksum)
O campo **FCS** ocupa 1 byte e é calculado pelo transmissor sobre os campos **`TYPE`**, **`SEQ`**, **`LEN`** e **`DATA`** (excluindo explicitamente `FLAG` e `FCS`):

$$\text{FCS} = \neg \left( (\text{TYPE} + \text{SEQ} + \text{LEN} + \sum_{i=0}^{\text{LEN}-1} \text{DATA}[i]) \bmod 256 \right)$$

Ao receber um quadro, o destino recalcula o complemento de 1 da soma desses mesmos bytes. Caso o valor obtido divirja do byte `FCS` recebido, o quadro é considerado corrompido e descartado imediatamente.

### 4. Controle de Fluxo e Recuperação (Stop-and-Wait ARQ)
* **Envio e Confirmação:** O transmissor fatia o buffer (até `4.096 bytes`) em blocos de até `16 bytes`, envia um quadro `DATA` por vez e aguarda o respectivo `ACK` por até **`700 ms`** (`TIMEOUT_ACK_MS`).
* **Retransmissão Automática:** Se o `ACK` não chegar dentro do tempo limite (por perda do quadro de dados, descarte por erro de FCS ou perda do próprio `ACK`), o transmissor reenvia o quadro automaticamente até o limite de **`10` tentativas** (`MAX_TENTATIVAS`).
* **Filtro de Duplicatas:** O receptor rastreia o `SEQ` esperado (`seqEsperado`) e o último `END` confirmado (`ultimoSeqEnd`). Se um `ACK` se perder na volta e o transmissor reenviar um quadro já processado, o receptor descarta a carga útil repetida e reenvia apenas o `ACK`.

---

## Estrutura do Repositório

```text
├── firmware_esp32.ino   # Código embarcado para ambas as placas ESP32 (Arduino IDE)
├── cliente_rf.py        # Script Python para envio de texto/imagem e reconstrução visual
└── README.md            # Documentação técnica e guia de reprodução
