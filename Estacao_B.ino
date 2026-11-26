#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#define PINO_TX   32
#define PINO_RX   33
#define PINO_SDA  26
#define PINO_SCL  27

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
void processarQuadroRecebido(const Quadro &rx);
void exibirNoLCD(const String &linha1, const String &linha2 = "");

uint8_t bufferRecepcao[MAX_BUFFER_DADOS];
uint16_t tamanhoRecebido = 0;
LiquidCrystal_I2C* lcd = nullptr;
bool emSessao = false;
uint8_t seqEsperado = 0;
int16_t ultimoSeqEnd = -1;
uint32_t ultimaAtividadeSessao = 0;
uint32_t ultimoEndMs = 0;
uint8_t ultimoTipoEnd = 0;
Quadro ultimoData{};
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

void inicializarLCD() {
  Wire.begin(PINO_SDA, PINO_SCL);
  Wire.setClock(100000);
  uint8_t enderecos[] = {0x27, 0x3F};

  for (uint8_t addr : enderecos) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      lcd = new LiquidCrystal_I2C(addr, 16, 2);
      lcd->init();
      lcd->backlight();
      lcd->clear();
      lcd->setCursor(0, 0);
      lcd->print("RF 433MHz Pronto");
      lcd->setCursor(0, 1);
      lcd->print("Aguardando...");
      Serial.printf("[LCD] Display detectado no endereco 0x%02X\n", addr);
      return;
    }
  }
  Serial.println("[LCD] Nenhum display detectado nesta placa.");
}

void exibirNoLCD(const String &linha1, const String &linha2) {
  if (lcd == nullptr) return;

  lcd->clear();
  lcd->setCursor(0, 0);
  lcd->print(linha1.substring(0, min((int)linha1.length(), 16)));

  if (linha2.length() > 0) {
    lcd->setCursor(0, 1);
    lcd->print(linha2.substring(0, min((int)linha2.length(), 16)));
  }
}

void exibirTextoRecebidoLCD(const uint8_t* dados, uint16_t tamanho) {
  String texto = "";
  uint16_t limite = min((uint16_t)32, tamanho);
  for (uint16_t i = 0; i < limite; i++) {
    texto += (char)dados[i];
  }

  String l1 = texto.substring(0, min((int)texto.length(), 16));
  String l2 = (texto.length() > 16) ? texto.substring(16, min((int)texto.length(), 32)) : "";
  exibirNoLCD(l1, l2);
}

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

void enviarACK(uint8_t seqConfirmado) {
  delay(20);

  Quadro qAck;
  qAck.type = TIPO_ACK;
  qAck.seq  = seqConfirmado;
  qAck.len  = 0;

  enviarQuadroRF(qAck, false);
  Serial.printf("[TX_ACK] ACK enviado para SEQ=%d\n", seqConfirmado);
}

void processarQuadroRecebido(const Quadro &rx) {
  uint8_t fcsCalculado = calcularFCS(rx);

  if (fcsCalculado != rx.fcs) {
    Serial.printf("[RX_ERRO] Quadro corrompido descartado! FCS rx=0x%02X, esperado=0x%02X\n",
                  rx.fcs, fcsCalculado);
    return;
  }

  if (rx.type == TIPO_DATA) {
    if (!emSessao && rx.seq == 0) {
      emSessao = true;
      seqEsperado = 0;
      ultimoSeqEnd = -1;
      tamanhoRecebido = 0;
      ultimaAtividadeSessao = millis();
    }

    if (emSessao && rx.seq == seqEsperado) {
      if (rx.len == 0 || (tamanhoRecebido + rx.len) > MAX_BUFFER_DADOS) {
        Serial.println("[RX_ERRO] Tamanho invalido ou buffer cheio; sem ACK.");
        return;
      }
      memcpy(bufferRecepcao + tamanhoRecebido, rx.data, rx.len);
      tamanhoRecebido += rx.len;
      ultimaAtividadeSessao = millis();
      Serial.printf("[RX_DATA] SEQ=%d aceito (%d bytes | acumulado=%d)\n",
                    rx.seq, rx.len, tamanhoRecebido);
      ultimoData = rx;
      seqEsperado = (uint8_t)(seqEsperado + 1);
      enviarACK(rx.seq);
    } else if (emSessao && tamanhoRecebido > 0 &&
               rx.seq == (uint8_t)(seqEsperado - 1) &&
               rx.len == ultimoData.len &&
               memcmp(rx.data, ultimoData.data, rx.len) == 0) {
      ultimaAtividadeSessao = millis();
      Serial.printf("[RX_DUP] DATA duplicado SEQ=%d; reenviando ACK\n", rx.seq);
      enviarACK(rx.seq);
    } else {
      Serial.printf("[RX_ERRO] DATA fora de sequencia SEQ=%d; sem ACK\n", rx.seq);
    }
  }
  else if (rx.type == TIPO_END) {
    if (rx.len != 1 || (rx.data[0] != CONTEUDO_TEXTO && rx.data[0] != CONTEUDO_IMAGEM)) {
      Serial.println("[RX_ERRO] END invalido; sem ACK.");
      return;
    }
    if (emSessao && rx.seq == seqEsperado) {
      uint8_t tipoConteudo = rx.data[0];
      Serial.printf("[RX_END] Fim da transmissao (SEQ=%d | Tipo=0x%02X | Total=%d bytes)\n",
                    rx.seq, tipoConteudo, tamanhoRecebido);

      emSessao = false;
      ultimoSeqEnd = rx.seq;
      ultimoTipoEnd = tipoConteudo;
      ultimoEndMs = millis();
      seqEsperado = 0;
      enviarACK(rx.seq);

      if (tipoConteudo == CONTEUDO_TEXTO) {
        Serial.println("==========================================");
        Serial.print("MENSAGEM RECEBIDA: ");
        Serial.write(bufferRecepcao, tamanhoRecebido);
        Serial.println("\n==========================================\n");
        exibirTextoRecebidoLCD(bufferRecepcao, tamanhoRecebido);
      }
      else if (tipoConteudo == CONTEUDO_IMAGEM) {
        exibirNoLCD("Imagem recebida!", String(tamanhoRecebido) + " bytes OK");
        Serial.print("[IMG_HEX]:");
        const char hex[] = "0123456789ABCDEF";
        char bloco[64];
        for (uint16_t i = 0; i < tamanhoRecebido;) {
          size_t n = 0;
          while (i < tamanhoRecebido && n < sizeof(bloco)) {
            uint8_t b = bufferRecepcao[i++];
            bloco[n++] = hex[b >> 4];
            bloco[n++] = hex[b & 15];
          }
          Serial.write((const uint8_t*)bloco, n);
        }
        Serial.println();
      }
    } else if (!emSessao && rx.seq == ultimoSeqEnd && rx.data[0] == ultimoTipoEnd &&
               millis() - ultimoEndMs < TIMEOUT_SESSAO_MS) {
      Serial.printf("[RX_DUP] END duplicado SEQ=%d; reenviando ACK\n", rx.seq);
      enviarACK(rx.seq);
    }
  }
}

void setup() {
  pinMode(PINO_RX, INPUT);
  digitalWrite(PINO_TX, LOW);
  pinMode(PINO_TX, OUTPUT);
  Serial.begin(115200);
  inicializarLCD();
  reiniciarRecepcaoRF();
  Serial.println("=== ESTACAO B - RECEPTORA ===");
  Serial.println("Aguardando dados da estacao A...");
}

void loop() {
  if (emSessao && millis() - ultimaAtividadeSessao >= TIMEOUT_SESSAO_MS) {
    emSessao = false;
    seqEsperado = 0;
    tamanhoRecebido = 0;
    Serial.println("[RX_TIMEOUT] Sessao incompleta descartada.");
  }

  Quadro rxQuadro;
  if (receberQuadroRF(rxQuadro)) processarQuadroRecebido(rxQuadro);
}
