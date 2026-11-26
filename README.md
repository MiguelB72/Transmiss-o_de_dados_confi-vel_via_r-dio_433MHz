# Transmissão de Dados Confiável via Rádio (RF 433 MHz)

Sistema de comunicação sem fio bidirecional desenvolvido entre duas placas **ESP32** conectadas a dois computadores, utilizando exclusivamente módulos de radiofrequência (**RF 433 MHz**) em transmissão bruta (sem uso de bibliotecas prontas de rádio ou interfaces nativas como Wi-Fi e Bluetooth).

O projeto divide o firmware embarcado em duas estações (**Estação A** transmissora e **Estação B** receptora) e permite o envio confiável de **mensagens de texto** (exibidas no Monitor Serial e em um display LCD I2C no destino) e **pequenas imagens** (fatiadas na origem e reconstruídas visualmente no computador receptor), implementando enquadramento customizado, detecção de erros via **Checksum** e recuperação de falhas via **Stop-and-Wait ARQ**.

---

## Hardware e Materiais Utilizados

* **2x** Placas de desenvolvimento ESP32
* **4x** Módulos RF 433 MHz ASK/OOK (2 transmissores FS1000A e 2 receptores MX-RM-5V)
* **1x** Display LCD 16x2 com módulo adaptador I2C (conectado ao ESP32 da Estação B)
* **2x** Computadores com Arduino IDE e Python 3 instalados
* **2x** Protoboards, cabos jumpers, resistores (divisor de tensão 5 V para 3,3 V), capacitores de desacoplamento e antenas de fio rígido (~17,3 cm)

### Pinagem do ESP32

Ambas as estações utilizam um par TX/RX para viabilizar o fluxo de dados (`Estação A -> Estação B`) e o retorno das confirmações `ACK` (`Estação B -> Estação A`):

| Componente | Pino do Módulo | GPIO do ESP32 | Estação | Observação Técnica |
| :--- | :--- | :--- | :--- | :--- |
| **Módulo RF Transmissor (TX)** | `DATA` | `GPIO 32` | A e B | Alimentação em `5V` (VIN) para maior alcance |
| **Módulo RF Receptor (RX)** | `DATA` | `GPIO 33` | A e B | Utilizar divisor resistivo (`5V -> 3,3V`) no pino de sinal |
| **Display LCD I2C** | `SDA` | `GPIO 26` | Apenas B | Endereços I2C suportados automaticamente: `0x27` ou `0x3F` |
| **Display LCD I2C** | `SCL` | `GPIO 27` | Apenas B | Linha de clock do barramento I2C (`100 kHz`) |

---

## Arquitetura do Protocolo

### 1. Camada Física (RF Bruto)
* **Codificação Manchester:** Transmissão digital direta em banda base com `500 µs` por meio-bit (taxa efetiva de **1.000 bps**), garantindo transição de nível no centro de cada bit (`1 = HIGH->LOW`, `0 = LOW->HIGH`).
* **Sincronismo e Encerramento:** Cada quadro é precedido por um preâmbulo de **8 bytes** (`0xAA`) para estabilização do ganho do receptor e pela palavra de sincronismo de **16 bits** (`0x2DD4`, avaliada como 32 meios-bits Manchester). Ao final do `FCS`, é enviado um byte *trailer* (`0x00`) para assegurar a última transição física do sinal.
* **Decodificador Não Bloqueante:** A recepção mede continuamente a duração entre bordas no pino `RX` (`250–750 µs` para 1 meio-bit e `750–1250 µs` para 2 meios-bits), reconstruindo os bits sem travar o loop principal do microcontrolador.

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
* **Leitura Guiada por `LEN`:** Após validar a `FLAG` inicial (`0x7E`), o receptor contabiliza exatamente os `LEN` bytes indicados no cabeçalho, evitando encerramento prematuro caso o byte `0x7E` ocorra naturalmente dentro dos bytes de uma imagem.

### 3. Detecção de Erros (FCS — Checksum)
O campo **FCS** ocupa 1 byte e é calculado sobre os campos **`TYPE`**, **`SEQ`**, **`LEN`** e **`DATA`** (excluindo explicitamente `FLAG` e `FCS`):

$$\text{FCS} = \neg \left( (\text{TYPE} + \text{SEQ} + \text{LEN} + \sum_{i=0}^{\text{LEN}-1} \text{DATA}[i]) \bmod 256 \right)$$

Ao receber um quadro completo, a Estação B recalcula o complemento de 1 da soma desses mesmos bytes. Se houver divergência em relação ao `FCS` recebido, o quadro corrompido é descartado imediatamente sem envio de `ACK`.

### 4. Controle de Fluxo e Recuperação (Stop-and-Wait ARQ)
* **Envio e Confirmação:** A Estação A fatia o buffer (até `4.096 bytes`) em blocos de até `16 bytes`, envia um quadro `DATA` por vez e aguarda o respectivo `ACK` da Estação B por até **`700 ms`** (`TIMEOUT_ACK_MS`).
* **Retransmissão Automática:** Caso o `ACK` não retorne dentro do tempo limite (por perda do quadro, descarte por erro de FCS ou perda do próprio `ACK`), a Estação A retransmite o quadro automaticamente até o limite de **`10` tentativas** (`MAX_TENTATIVAS`).
* **Controle de Duplicatas e Timeout de Sessão:** A Estação B rastreia o `SEQ` esperado e o último quadro recebido; se um `ACK` se perder na volta e a Estação A reenviar um quadro já processado, a Estação B ignora a carga útil repetida e reenvia apenas o `ACK`. Sessões incompletas são descartadas automaticamente após **`12 s`** (`TIMEOUT_SESSAO_MS`).

---

## Estrutura do Repositório

```text
├── Estacao_A.ino   # Firmware da estação transmissora (envia DATA/END e recebe ACK)
├── Estacao_B.ino   # Firmware da estação receptora (recebe DATA/END, envia ACK e controla o LCD)
├── cliente_rf.py   # Script Python para envio de texto/imagem e reconstrução visual no PC
└── README.md       # Documentação técnica e guia de reprodução
