# Master XIAO ESP32-S3 - Lector de Ruido

Este es el código para el dispositivo **Maestro** (XIAO ESP32-S3) que consulta los datos del dispositivo Esclavo (ESP32-C3 Medidor de Ruido).

## Conexiones I2C
Conecta los siguientes pines entre el XIAO ESP32-S3 y el ESP32-C3.  
**IMPORTANTE:** Como ambos dispositivos funcionan a 3.3V, no necesitas conversores de nivel lógico. Sin embargo, recuerda que el bus I2C idealmente requiere resistencias pull-up de 4.7kΩ en las líneas SDA y SCL (aunque a veces las internas o las de los módulos funcionan para distancias cortas).

| Señal | XIAO ESP32-S3 (Master) | ESP32-C3 (Slave) |
| :--- | :--- | :--- |
| **SDA** | D4 (GPIO 5) | GPIO 8 |
| **SCL** | D5 (GPIO 6) | GPIO 9 |
| **GND** | GND | GND |
| **3V3/5V**| Vin/5V (Alimentación compartida si aplica) | 5V/3V3 |

*Nota: Asegúrate de conectar las tierras (GND) de ambos dispositivos.*

## Funcionamiento
1. El Maestro inicia el bus I2C.
2. Cada 1 segundo, solicita 4 bytes a la dirección `0x42`.
3. Reconstruye el número decimal (`float`) y lo muestra por el monitor serie.
