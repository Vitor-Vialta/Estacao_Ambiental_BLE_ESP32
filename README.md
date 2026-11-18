
# Estação Ambiental BLE — ESP32 + AHT20 + BMP280 + WRITE

## Novidade desta versão

Foi adicionada uma característica BLE com propriedade **READ + WRITE** para alterar remotamente o intervalo de aquisição.

## Estrutura GATT

### Service
`7c9e0001-6e4f-4c3b-8b9a-123456789001`

### Temperatura
`7c9e0002-6e4f-4c3b-8b9a-123456789002`

- READ
- NOTIFY

### Umidade
`7c9e0003-6e4f-4c3b-8b9a-123456789003`

- READ
- NOTIFY

### Pressão
`7c9e0004-6e4f-4c3b-8b9a-123456789004`

- READ
- NOTIFY

### Status
`7c9e0005-6e4f-4c3b-8b9a-123456789005`

- READ

### Intervalo de aquisição
`7c9e0006-6e4f-4c3b-8b9a-123456789006`

- READ
- WRITE

## Formato do valor

O smartphone deve escrever o intervalo em **milissegundos**, como texto ASCII.

Exemplos:

- `500` -> 0,5 s
- `1000` -> 1 s
- `2000` -> 2 s
- `5000` -> 5 s
- `10000` -> 10 s

Limites aceitos:

- mínimo: 500 ms
- máximo: 60000 ms

Se for escrito um valor fora da faixa, o ESP32 ignora a alteração e mantém o intervalo anterior.

## Teste com nRF Connect

1. Faça Scan.
2. Conecte em `ESP32_Estacao_Ambiental`.
3. Localize o serviço:
   `7c9e0001-6e4f-4c3b-8b9a-123456789001`
4. Localize a characteristic de intervalo:
   `7c9e0006-6e4f-4c3b-8b9a-123456789006`
5. Faça READ para visualizar o valor atual.
6. Faça WRITE.
7. Selecione UTF-8/Text.
8. Digite, por exemplo:
   `5000`
9. Confirme.
10. Observe no Monitor Serial que o intervalo muda imediatamente para 5000 ms.

## Exemplo de interação

Valor inicial:
`2000`

Smartphone envia:
`5000`

Monitor Serial:
`Novo intervalo de aquisicao: 5000 ms`

As notificações passam então a ocorrer a cada 5 segundos.

## Conceito didático

A característica WRITE introduz comunicação BLE bidirecional:

```text
Sensores -> ESP32 -> NOTIFY -> smartphone
                    ^
                    |
                    WRITE
                    |
               smartphone
```

Isso permite transformar o smartphone em interface de configuração remota do sistema embarcado.

---

# Registro na memória (datalogger)

A partir desta versão o ESP32 grava **uma leitura a cada 5 minutos** na memória
flash interna (LittleFS), mesmo sem nenhum celular conectado. Os dados podem
ser baixados como `.csv` pelo celular (página web) ou pelo PC (USB).

## Capacidade

- Cada leitura ocupa 16 bytes.
- Com o esquema de partição padrão da placa (1,5 MB de dados), cabem entre
  ~140 e ~280 dias de leituras. Quando a memória enche, os registros mais
  antigos são descartados automaticamente.

## Data e hora

O ESP32 não tem relógio com bateria. Cada leitura guarda "sessão + segundos
desde que ligou", e a hora real é informada:

- automaticamente pela página web, ao conectar;
- automaticamente pelo script `baixar_dados.py`, ao baixar pelo USB.

Leituras feitas antes da sincronização (na mesma sessão) recebem a hora
correta retroativamente. Se o ESP32 for ligado e desligado sem nunca receber
a hora, as leituras daquela sessão ficam no CSV com `data_hora` vazia.

## Novas características GATT

| UUID | Propriedades | Conteúdo |
|---|---|---|
| `7c9e0007-6e4f-4c3b-8b9a-123456789007` | READ, WRITE | Hora unix em texto (escrever ajusta o relógio) |
| `7c9e0008-6e4f-4c3b-8b9a-123456789008` | READ, WRITE | READ: resumo do registro. WRITE: `GET`, `STOP`, `CLEAR` |
| `7c9e0009-6e4f-4c3b-8b9a-123456789009` | NOTIFY | Dados binários do registro durante o `GET` |

Formato de cada pacote do `GET`: 2 bytes de sequência (little-endian) + dados.
`0xFFFE` = início (uint32 com o total de bytes), `0xFFFF` = fim.

Registro binário (16 bytes, little-endian):
`seg (u32) | sessao (u16) | tipo (u8: 0 leitura, 1 hora) | flags (u8) | temp×100 (i16) | umid×100 (u16) | pressão×100 ou hora unix (u32)`

## Baixar pelo PC (USB)

1. Feche o Monitor Serial da IDE.
2. Instale o pyserial (uma vez): `sudo apt install python3-serial`
3. Rode na pasta do projeto:

```bash
python3 baixar_dados.py            # salva estacao_AAAA-MM-DD_HHMM.csv
python3 baixar_dados.py --br       # formato Excel brasileiro (; e vírgula decimal)
python3 baixar_dados.py --apagar   # depois de salvar, apaga a memória (pede confirmação)
```

Comandos aceitos pela serial (115200): `INFO`, `T<unix>`, `DUMP`, `CLEAR`.

## Colunas do CSV

`data_hora, unix, temperatura_C, umidade_pct, pressao_hPa, sessao, seg_desde_ligar`
