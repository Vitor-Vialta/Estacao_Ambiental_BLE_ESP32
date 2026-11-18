# Estação Ambiental BLE com ESP32

Estação que mede **temperatura, umidade e pressão** com um ESP32 e envia os dados
por **Bluetooth Low Energy (BLE)** para o celular. Ela também guarda uma leitura a
cada 5 minutos na memória interna, por meses, para baixar depois em `.csv`.

**▶ [Abrir a interface no celular](https://vitor-vialta.github.io/Estacao_Ambiental_BLE_ESP32/)**
(Chrome no Android, ou app Bluefy no iPhone)

## Funcionalidades

- Leituras ao vivo por notificação BLE (temperatura, umidade, pressão e status dos sensores)
- Intervalo de aquisição ajustável pelo celular (500 a 60000 ms), por escrita BLE
- Interface web com gráficos, máximo/mínimo e altitude estimada pela pressão
- Registro de uma leitura a cada 5 min na flash (LittleFS), mesmo sem ninguém conectado
  (de ~140 a ~280 dias de dados)
- Download dos dados em `.csv` pelo celular (BLE) ou pelo computador (USB)

## Hardware

| Componente | Função |
|---|---|
| ESP32 DEVKIT V1 (ESP-WROOM-32, 30 pinos) | Microcontrolador com BLE |
| AHT10 (ou AHT20) | Temperatura e umidade (I²C, endereço 0x38) |
| BMP280 | Pressão (I²C, endereço 0x76 ou 0x77) |

### Ligações

Os dois sensores usam o mesmo barramento I²C.

| Sensor | Pino do sensor | ESP32 |
|---|---|---|
| AHT10 e BMP280 | VCC | 3V3 |
| AHT10 e BMP280 | GND | GND |
| AHT10 e BMP280 | SDA | GPIO 21 |
| AHT10 e BMP280 | SCL | GPIO 22 |
| BMP280 | CSB | 3V3 (seleciona o modo I²C) |
| BMP280 | SDO | GND (endereço 0x76) |

## Como usar

### 1. Gravar o firmware

1. Instale a [Arduino IDE 2](https://www.arduino.cc/en/software) e o pacote de placas
   **esp32 (Espressif)** pelo Gerenciador de Placas.
2. Instale as bibliotecas **Adafruit AHTX0** e **Adafruit BMP280 Library**
   (aceite instalar as dependências).
3. Abra `firmware/Estacao_Ambiental_BLE_ESP32/Estacao_Ambiental_BLE_ESP32.ino`.
4. Selecione a placa **DOIT ESP32 DEVKIT V1** e a porta, e grave.
5. No Monitor Serial (115200 baud) devem aparecer as leituras a cada 2 s.

### 2. Ver os dados no celular

Abra a [interface](https://vitor-vialta.github.io/Estacao_Ambiental_BLE_ESP32/) e toque em
**Conectar ao ESP32**. Ao conectar, a página acerta o relógio do ESP32. Pelo cartão
**Registro na memória** dá para baixar o `.csv` ou apagar os registros.

Também é possível testar com o app **nRF Connect**, usando os UUIDs do
[protocolo](docs/PROTOCOLO.md).

### 3. Baixar os dados pelo computador

Com o ESP32 no USB e o Monitor Serial fechado:

```bash
sudo apt install python3-serial      # uma vez
python3 ferramentas/baixar_dados.py          # salva estacao_AAAA-MM-DD_HHMM.csv
python3 ferramentas/baixar_dados.py --br     # formato do Excel brasileiro (; e vírgula decimal)
python3 ferramentas/baixar_dados.py --apagar # depois de salvar, apaga a memória (pede confirmação)
```

Colunas do CSV: `data_hora, unix, temperatura_C, umidade_pct, pressao_hPa, sessao, seg_desde_ligar`

## Estrutura do repositório

```text
├── firmware/
│   └── Estacao_Ambiental_BLE_ESP32/
│       └── Estacao_Ambiental_BLE_ESP32.ino   código do ESP32
├── docs/
│   ├── index.html                            interface web (publicada pelo GitHub Pages)
│   └── PROTOCOLO.md                          características BLE e formato dos dados
└── ferramentas/
    └── baixar_dados.py                       download do registro pelo USB
```

## Observações

- O ESP32 não tem relógio com bateria. A hora é informada pelo celular ou pelo PC, e as
  leituras daquela sessão recebem o horário correto, inclusive as anteriores. Se o ESP32
  for ligado e desligado sem receber a hora, as leituras dessa sessão ficam sem data no CSV.
- A altitude da interface é uma estimativa pela fórmula barométrica. Para mais precisão,
  use **Calibrar** informando a altitude real do local.
- Sensores muito perto da placa podem medir temperatura um pouco acima da ambiente,
  por causa do calor do próprio ESP32.
