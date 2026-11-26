#include <Arduino.h>

#define PINO_TX   32
#define PINO_RX   33

#define MEIO_BIT_US   500
#define PREAMBULO_BYTES 8
#define SYNC_WORD     0x2DD4

#define FLAG_INICIO       0x7E
#define MAX_PAYLOAD       16
#define MAX_BUFFER_DADOS  4096
#define TIMEOUT_ACK_MS    700
#define MAX_TENTATIVAS    10
#define TIMEOUT_SESSAO_MS 12000UL

enum TipoQuadro : uint8_t {
  TIPO_DATA = 0x01,
  TIPO_ACK  = 0x02,
  TIPO_END  = 0x03
};

enum TipoConteudo : uint8_t {
  CONTEUDO_TEXTO  = 0x01,
  CONTEUDO_IMAGEM = 0x02
};

struct Quadro {
  uint8_t flag;
  uint8_t type;
  uint8_t seq;
  uint8_t len;
  uint8_t data[MAX_PAYLOAD];
  uint8_t fcs;
};

void enviarQuadroRF(Quadro &q, bool simularErro = false);
bool receberQuadroRF(Quadro &q);
bool consumirMeioBit(bool nivel, Quadro &q);
uint8_t calcularFCS(const Quadro &q);
bool enviarQuadroConfiavel(Quadro &q, bool simularErroNaPrimeira = false);

uint8_t bufferTransmissao[MAX_BUFFER_DADOS];
bool aguardarRecuperacao = false;
uint32_t instanteFalha = 0;
int ultimoEstadoRX = LOW;
uint32_t ultimaBordaRX = 0;

uint32_t janelaSync = 0;
uint8_t meiosSync = 0;
bool sincronizadoRX = false;
bool temPrimeiroMeio = false;
bool primeiroMeio = false;
uint8_t byteRX = 0, bitsRX = 0, indiceRX = 0;
Quadro quadroParcial;

void limparDecodificador() {
  janelaSync = 0;
  meiosSync = 0;
  sincronizadoRX = false;
  temPrimeiroMeio = false;
  byteRX = bitsRX = indiceRX = 0;
}

void reiniciarRecepcaoRF() {
  limparDecodificador();
  ultimoEstadoRX = digitalRead(PINO_RX);
  ultimaBordaRX = micros();
}

uint32_t syncManchester() {
  uint32_t resultado = 0;
  for (int i = 15; i >= 0; --i) {
    resultado = (resultado << 2) | ((SYNC_WORD & (1U << i)) ? 2U : 1U);
  }
  return resultado;
}
const uint32_t SYNC_MANCHESTER = syncManchester();

uint8_t calcularFCS(const Quadro &q) {
  uint8_t soma = 0;
  soma += q.type;
  soma += q.seq;
  soma += q.len;

  for (uint8_t i = 0; i < q.len; i++) {
    soma += q.data[i];
  }

  return ~soma;
}

void enviarBitManchester(bool bitVal) {
  if (bitVal) {
    digitalWrite(PINO_TX, HIGH);
    delayMicroseconds(MEIO_BIT_US);
    digitalWrite(PINO_TX, LOW);
    delayMicroseconds(MEIO_BIT_US);
  } else {
    digitalWrite(PINO_TX, LOW);
    delayMicroseconds(MEIO_BIT_US);
    digitalWrite(PINO_TX, HIGH);
    delayMicroseconds(MEIO_BIT_US);
  }
}

void enviarByteRF(uint8_t b) {
  for (int8_t i = 7; i >= 0; i--) {
    enviarBitManchester((b >> i) & 0x01);
  }
}

void enviarQuadroRF(Quadro &q, bool simularErro) {
  q.flag = FLAG_INICIO;
  q.fcs = calcularFCS(q);

  uint8_t buffer[21];
  uint8_t idx = 0;
  buffer[idx++] = q.flag;
  buffer[idx++] = q.type;
  buffer[idx++] = q.seq;
  buffer[idx++] = q.len;

  for (uint8_t i = 0; i < q.len; i++) {
    buffer[idx++] = q.data[i];
  }
  buffer[idx++] = q.fcs;

  if (simularErro) {
    if (q.len > 0) {
      buffer[4] ^= 0x01;
    } else {
      buffer[2] ^= 0x01;
    }
    Serial.println("[SIMULACAO] Bit invertido propositalmente nesta tentativa!");
  }

  for (uint8_t i = 0; i < PREAMBULO_BYTES; i++) {
    enviarByteRF(0xAA);
  }

  enviarByteRF((SYNC_WORD >> 8) & 0xFF);
  enviarByteRF(SYNC_WORD & 0xFF);

  for (uint8_t i = 0; i < idx; i++) {
    enviarByteRF(buffer[i]);
  }

  enviarByteRF(0x00);
  digitalWrite(PINO_TX, LOW);
  reiniciarRecepcaoRF();
}

bool consumirMeioBit(bool nivel, Quadro &q) {
  if (!sincronizadoRX) {
    janelaSync = (janelaSync << 1) | (nivel ? 1U : 0U);
    if (meiosSync < 32) ++meiosSync;
    if (meiosSync == 32 && janelaSync == SYNC_MANCHESTER) {
      sincronizadoRX = true;
      temPrimeiroMeio = false;
      byteRX = bitsRX = indiceRX = 0;
      quadroParcial = Quadro{};
    }
    return false;
  }

  if (!temPrimeiroMeio) {
    primeiroMeio = nivel;
    temPrimeiroMeio = true;
    return false;
  }
  temPrimeiroMeio = false;
  if (primeiroMeio == nivel) {
    limparDecodificador();
    return false;
  }
  byteRX = (byteRX << 1) | (primeiroMeio ? 1 : 0);
  if (++bitsRX < 8) return false;
  uint8_t valor = byteRX;
  bitsRX = byteRX = 0;

  switch (indiceRX) {
    case 0:
      if (valor != FLAG_INICIO) { limparDecodificador(); return false; }
      quadroParcial.flag = valor;
      break;
    case 1:
      if (valor != TIPO_DATA && valor != TIPO_ACK && valor != TIPO_END) {
        limparDecodificador(); return false;
      }
      quadroParcial.type = valor;
      break;
    case 2: quadroParcial.seq = valor; break;
    case 3:
      if (valor > MAX_PAYLOAD ||
          (quadroParcial.type == TIPO_ACK && valor != 0) ||
          (quadroParcial.type == TIPO_END && valor != 1) ||
          (quadroParcial.type == TIPO_DATA && valor == 0)) {
        limparDecodificador(); return false;
      }
      quadroParcial.len = valor;
      break;
    default:
      if (indiceRX < 4 + quadroParcial.len) {
        quadroParcial.data[indiceRX - 4] = valor;
      } else {
        quadroParcial.fcs = valor;
        q = quadroParcial;
        limparDecodificador();
        return true;
      }
  }
  ++indiceRX;
  return false;
}

bool receberQuadroRF(Quadro &q) {
  int estadoAtual = digitalRead(PINO_RX);
  uint32_t agora = micros();
  uint32_t duracao = agora - ultimaBordaRX;
  if (estadoAtual == ultimoEstadoRX) {
    if (duracao > 3 * MEIO_BIT_US && (meiosSync || sincronizadoRX)) {
      limparDecodificador();
    }
    return false;
  }
  bool nivelAnterior = (ultimoEstadoRX == HIGH);
  ultimoEstadoRX = estadoAtual;
  ultimaBordaRX = agora;

  uint8_t quantidade;
  if (duracao >= 250 && duracao < 750) quantidade = 1;
  else if (duracao >= 750 && duracao <= 1250) quantidade = 2;
  else { limparDecodificador(); return false; }

  for (uint8_t i = 0; i < quantidade; ++i) {
    if (consumirMeioBit(nivelAnterior, q)) return true;
  }
  return false;
}

bool enviarQuadroConfiavel(Quadro &q, bool simularErroNaPrimeira) {
  for (uint8_t tentativa = 1; tentativa <= MAX_TENTATIVAS; tentativa++) {
    bool injetarFalha = (simularErroNaPrimeira && tentativa == 1);

    if (tentativa == 1) {
      Serial.printf("[TX_%s] SEQ=%d | LEN=%d | Tentativa 1\n",
                    (q.type == TIPO_END) ? "END" : "DATA", q.seq, q.len);
    } else {
      Serial.printf("[RETRANSMIT] SEQ=%d | Tentativa %d de %d\n",
                    q.seq, tentativa, MAX_TENTATIVAS);
    }

    enviarQuadroRF(q, injetarFalha);

    uint32_t inicioEspera = millis();
    while ((millis() - inicioEspera) < TIMEOUT_ACK_MS) {
      Quadro resp;
      if (receberQuadroRF(resp)) {
        if (calcularFCS(resp) == resp.fcs &&
            resp.type == TIPO_ACK && resp.len == 0 &&
            resp.seq == q.seq) {
          Serial.printf("[ACK_OK] Confirmacao recebida para SEQ=%d\n", q.seq);
          delay(20);
          return true;
        }
      }
    }

    Serial.printf("[TIMEOUT] Sem ACK para SEQ=%d apos %d ms\n", q.seq, TIMEOUT_ACK_MS);
  }

  Serial.printf("[FALHA_CRITICA] Limite de tentativas atingido para SEQ=%d\n", q.seq);
  return false;
}

void transmitirBuffer(const uint8_t* dados, uint16_t totalBytes, uint8_t tipoConteudo, bool simularErro) {
  if (aguardarRecuperacao && millis() - instanteFalha < TIMEOUT_SESSAO_MS) {
    Serial.println("[OCUPADO] Aguarde 12 s apos a falha antes de iniciar outra mensagem.");
    return;
  }
  aguardarRecuperacao = false;
  uint16_t offset = 0;
  uint8_t seq = 0;
  uint16_t totalQuadros = (totalBytes + MAX_PAYLOAD - 1) / MAX_PAYLOAD;

  Serial.printf("\n--- Iniciando transmissao (%s): %d bytes em %d quadros ---\n",
                (tipoConteudo == CONTEUDO_IMAGEM) ? "IMAGEM" : "TEXTO",
                totalBytes, totalQuadros);

  while (offset < totalBytes) {
    uint8_t tamanhoBloco = min((uint16_t)MAX_PAYLOAD, (uint16_t)(totalBytes - offset));

    Quadro qData;
    qData.type = TIPO_DATA;
    qData.seq  = seq;
    qData.len  = tamanhoBloco;
    memcpy(qData.data, dados + offset, tamanhoBloco);

    bool erroNesteQuadro = (simularErro && seq == 0);

    if (!enviarQuadroConfiavel(qData, erroNesteQuadro)) {
      Serial.println("--- Transmissao abortada por falha de entrega ---\n");
      aguardarRecuperacao = true;
      instanteFalha = millis();
      return;
    }

    offset += tamanhoBloco;
    seq = (seq + 1) % 256;
  }

  Quadro qEnd;
  qEnd.type = TIPO_END;
  qEnd.seq  = seq;
  qEnd.len  = 1;
  qEnd.data[0] = tipoConteudo;

  if (enviarQuadroConfiavel(qEnd, false)) {
    Serial.println("--- Transmissao concluida com sucesso! ---\n");
    delay(1000);
  } else {
    Serial.println("--- Falha ao confirmar quadro END ---\n");
    aguardarRecuperacao = true;
    instanteFalha = millis();
  }
}

int charParaNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

uint16_t decodificarHexParaBuffer(const String &hexStr, uint8_t* destino, uint16_t maxBytes) {
  if (hexStr.length() == 0 || hexStr.length() % 2 != 0 || hexStr.length() / 2 > maxBytes) {
    Serial.println("[ERRO] HEX vazio, impar ou maior que 4096 bytes.");
    return 0;
  }
  uint16_t numBytes = hexStr.length() / 2;
  for (uint16_t i = 0; i < numBytes; i++) {
    int hi = charParaNibble(hexStr[i * 2]);
    int lo = charParaNibble(hexStr[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      Serial.println("[ERRO] Caractere hexadecimal invalido.");
      return 0;
    }
    destino[i] = (hi << 4) | lo;
  }
  return numBytes;
}

String linhaSerial;
bool linhaExcedida = false;
const size_t MAX_LINHA_SERIAL = 2 * MAX_BUFFER_DADOS + 10;

void setup() {
  pinMode(PINO_RX, INPUT);
  digitalWrite(PINO_TX, LOW);
  pinMode(PINO_TX, OUTPUT);
  Serial.setRxBufferSize(8448);
  Serial.begin(115200);
  linhaSerial.reserve(MAX_LINHA_SERIAL + 1);
  reiniciarRecepcaoRF();
  Serial.println("=== ESTACAO A - TRANSMISSORA ===");
  Serial.println("Comandos: texto | !erro texto | !img HEX | !img_erro HEX");
}

void processarEntradaSerial(String &entrada) {
  entrada.trim();
  if (entrada.length() == 0) return;
  if (entrada.startsWith("!img_erro ") || entrada.startsWith("!img ")) {
    bool simularErro = entrada.startsWith("!img_erro ");
    String hexDados = entrada.substring(simularErro ? 10 : 5);
    uint16_t qtdBytes = decodificarHexParaBuffer(hexDados, bufferTransmissao, MAX_BUFFER_DADOS);
    if (qtdBytes > 0) transmitirBuffer(bufferTransmissao, qtdBytes, CONTEUDO_IMAGEM, simularErro);
  } else {
    bool simularErro = entrada.startsWith("!erro ");
    if (simularErro) entrada.remove(0, 6);
    if (entrada.length() > MAX_BUFFER_DADOS) {
      Serial.println("[ERRO] Texto maior que 4096 bytes.");
      return;
    }
    uint16_t qtdBytes = entrada.length();
    if (qtdBytes > 0) {
      memcpy(bufferTransmissao, entrada.c_str(), qtdBytes);
      transmitirBuffer(bufferTransmissao, qtdBytes, CONTEUDO_TEXTO, simularErro);
    }
  }
}

void loop() {
  if (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') {
      if (linhaExcedida) Serial.println("[ERRO] Linha serial muito longa.");
      else processarEntradaSerial(linhaSerial);
      linhaSerial = "";
      linhaExcedida = false;
    } else if (c != '\r') {
      if (linhaSerial.length() < MAX_LINHA_SERIAL && !linhaExcedida) linhaSerial += c;
      else linhaExcedida = true;
    }
  }
}
