# Medidor de Ruido Ambiental IoT (ESP32-C3 + MAX4466)

Este repositorio contiene una implementación profesional de un medidor de nivel sonoro (Sonómetro) basado en el microcontrolador **ESP32-C3** (Esclavo) y un sistema de lectura distribuida con **ESP32-S2** (Maestro).

## 🚀 Características

*   **Muestreo de Alta Velocidad**: ADC a **10kHz-20kHz** para capturar el espectro de audio completo.
*   **Procesamiento Digital DSP**: Cálculo de RMS y conversión a dB SPL mediante tareas FreeRTOS dedicadas.
*   **Arquitectura Maestro-Esclavo**: Comunicación mediante I2C redundante con auto-diagnóstico.
*   **Diagnóstico en Tiempo Real**: Detección automática de desconexión de micrófono (Bias Check).

## 🛠 Hardware y Conexiones

### 1. Nodo Sensor (I2C Slave - ESP32-C3)
| Componente | Pin ESP32-C3 | Notas |
| :--- | :--- | :--- |
| **MAX4466 OUT** | GPIO 4 | Entrada ADC |
| **I2C SDA** | GPIO 8 | Pin D2 en Lolin/SuperMini |
| **I2C SCL** | GPIO 10 | Pin D1 en Lolin/SuperMini |

### 2. Datalogger/Master (I2C Master - ESP32-S2)
| Componente | Pin ESP32-S2 | Notas |
| :--- | :--- | :--- |
| **I2C SDA** | GPIO 8 | Conectar a SDA de C3 |
| **I2C SCL** | GPIO 9 | Conectar a SCL de C3 |

> **IMPORTANTE**: Al usar ESP32-S2 y C3, recuerda que el bus I2C **necesita resistencias de pull-up** (4.7kΩ a 3.3V) si los cables son largos o si la comunicación es inestable.

## 📡 Protocolo I2C (Dirección 0x42)

El sensor responde a los siguientes comandos (1 byte):

| Comando | Valor | Respuesta |
| :--- | :--- | :--- |
| `GET_DB` | `0x10` | **4 Bytes** (Float) - Nivel en dB (LAeq,1s) |
| `GET_STATUS` | `0x20` | **1 Byte** (Uint8) - 1: OK, 0: Error Mic |
| `GET_RAW` | `0x30` | **4 Bytes** (Uint32) - Valor RMS en mV |

## ⚙️ Instalación (PlatformIO)

Cada carpeta es un proyecto independiente:
1.  **ruido**: Código para el ESP32-C3 (Esclavo).
2.  **ruido-max4466-maestro**: Código para el ESP32-S2 (Maestro).

Para flashear, abre la carpeta correspondiente en VSCode con PlatformIO y usa el botón `Upload`.

## 📐 Calibración

Ajusta la constante `CALIBRATION_DB` en el código del esclavo para que coincida con un sonómetro de referencia. El valor por defecto es un punto de partida basado en la sensibilidad estándar del MAX4466.

## 📜 Licencia

MIT License - Ver archivo [LICENSE](LICENSE).
