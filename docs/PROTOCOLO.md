# Protocolo BLE e formato dos dados

Nome anunciado: `ESP32_Estacao_Ambiental`

## Características GATT

Serviço: `7c9e0001-6e4f-4c3b-8b9a-123456789001`

| UUID | Propriedades | Conteúdo |
|---|---|---|
| `7c9e0002-6e4f-4c3b-8b9a-123456789002` | READ, NOTIFY | Temperatura em °C (texto, ex.: `25.43`) |
| `7c9e0003-6e4f-4c3b-8b9a-123456789003` | READ, NOTIFY | Umidade relativa em % (texto) |
| `7c9e0004-6e4f-4c3b-8b9a-123456789004` | READ, NOTIFY | Pressão em hPa (texto) |
| `7c9e0005-6e4f-4c3b-8b9a-123456789005` | READ | Status dos sensores, ex.: `AHT20=OK;BMP280=OK` |
| `7c9e0006-6e4f-4c3b-8b9a-123456789006` | READ, WRITE | Intervalo de aquisição em ms (texto ASCII, 500 a 60000) |
| `7c9e0007-6e4f-4c3b-8b9a-123456789007` | READ, WRITE | Hora unix em texto; escrever ajusta o relógio do ESP32 |
| `7c9e0008-6e4f-4c3b-8b9a-123456789008` | READ, WRITE | READ: resumo do registro. WRITE: `GET`, `STOP` ou `CLEAR` |
| `7c9e0009-6e4f-4c3b-8b9a-123456789009` | NOTIFY | Dados binários do registro durante o `GET` |

## Intervalo de aquisição

O celular escreve o intervalo em **milissegundos**, como texto ASCII (no nRF Connect,
escolha o formato *UTF-8/Text*). Exemplos: `500`, `2000`, `5000`. Valores fora da faixa
de 500 a 60000 são ignorados, e o intervalo anterior continua valendo.

Isso torna a comunicação bidirecional:

```text
Sensores -> ESP32 -> NOTIFY -> celular
              ^
              └──── WRITE ──── celular
```

## Resumo do registro (READ em 0008)

Texto no formato:

```text
registros=1234;bytes=19744;capacidade=81100;periodo=300;boot=7;hora=OK;memoria=OK
```

## Transferência do registro (`GET`)

Depois de ativar as notificações de `0009`, escreva `GET` em `0008`. Cada notificação tem
2 bytes de sequência (uint16, little-endian) seguidos dos dados:

| Sequência | Conteúdo |
|---|---|
| `0xFFFE` | Início: uint32 com o total de bytes |
| `0`, `1`, `2`, … | Dados do registro (a sequência volta a 0 depois de `0xFFEF`) |
| `0xFFFF` | Fim: uint32 com o total enviado |

`STOP` cancela a transferência, e `CLEAR` apaga todos os registros.

## Formato de cada registro (16 bytes, little-endian)

| Campo | Tipo | Significado |
|---|---|---|
| seg | uint32 | Segundos desde que o ESP32 ligou |
| sessao | uint16 | Número da sessão (aumenta a cada vez que liga) |
| tipo | uint8 | 0 = leitura, 1 = sincronização de hora |
| flags | uint8 | bit 0 = AHT ok, bit 1 = BMP ok |
| temp | int16 | Temperatura × 100 (°C) |
| umid | uint16 | Umidade × 100 (%) |
| valor | uint32 | Leitura: pressão × 100 (hPa). Sincronização: hora unix |

A hora de cada leitura é `hora_unix_da_sincronização − seg_da_sincronização + seg_da_leitura`,
usando a sincronização da mesma sessão.

## Comandos pela porta serial (115200 baud)

| Comando | Ação |
|---|---|
| `INFO` | Responde `#INFO` seguido do resumo do registro |
| `T<unix>` | Ajusta a hora (ex.: `T1795000000`) |
| `DUMP` | Envia `#DUMP <bytes>`, os dados binários e `#FIM` |
| `CLEAR` | Apaga os registros |
