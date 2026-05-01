# RemoteSwitchReceiverModule — Registro de Problemas y Soluciones

Proyecto: módulo Meshtastic custom que escucha paquetes `DETECTION_SENSOR_APP` y controla GPIOs con timeout configurable vía CLI nativa (`meshtastic --set moduleConfig.remote_switch.*`).

Hardware: Heltec V3 (ESP32-S3), firmware Meshtastic 2.7.x.
Entorno build: WSL Ubuntu-24.04 (Windows es incompatible con toolchain xtensa-esp32s3).

---

## PROBLEMA 1 — Toolchain ESP32-S3 incompatible con Windows

**Síntoma**: `CreateProcess: No such file or directory` al compilar con PlatformIO en Windows.

**Causa**: `xtensa-esp32s3-elf-gcc 8.4.0` no es compatible con rutas largas de Windows ni con ciertas versiones del runtime MSVC.

**Solución**: Mover TODO el flujo de compilación a WSL Ubuntu-24.04. Workflow definitivo:
1. Editar código en Windows (ruta `C:\Users\...\meshtastic-firmware\`)
2. Copiar archivos modificados a WSL con `cp /mnt/c/Users/.../meshtastic-firmware/src/... ~/meshtastic-firmware/src/...`
3. Compilar en WSL: `~/.local/bin/pio run -e heltec-v3`
4. Copiar binario a Windows: `cp ~/meshtastic-firmware/.pio/build/heltec-v3/firmware*.factory.bin /mnt/c/Temp/`
5. Flashear desde Windows: `python -m esptool --chip esp32s3 --port COM3 --baud 921600 write_flash 0x0 C:\Temp\firmware-heltec-v3.factory.bin`

---

## PROBLEMA 2 — `meshtastic_FromRadio_size` no declarado (error de compilación)

**Síntoma**: Error en tiempo de compilación al agregar `trigger_text` al proto.

**Causa**: Campos `string` en nanopb sin `max_size` en el `.options` generan `pb_callback_t` en vez de `char[]`. Esto rompe el union de tamaño `meshtastic_FromRadio_size` que nanopb usa internamente.

**Solución**: Agregar a `protobufs/meshtastic/module_config.options`:
```
*RemoteSwitchConfig.Rule.trigger_text max_size:32
```
Luego regenerar los `.pb.h`/`.pb.c` con nanopb.

---

## PROBLEMA 3 — `meshtastic_LocalModuleConfig has no member named 'has_remote_switch'`

**Síntoma**: Error de compilación en `AdminModule.cpp`.

**Causa**: En el firmware, `moduleConfig` es de tipo `meshtastic_LocalModuleConfig` (definido en `localonly.proto`), NO de tipo `ModuleConfig` (en `module_config.proto`). Son dos protos separados. Agregar el campo solo a `module_config.proto` no es suficiente.

**Solución**: Agregar también a `protobufs/meshtastic/localonly.proto`:
```proto
ModuleConfig.RemoteSwitchConfig remote_switch = 18;  // usar el próximo número libre
```
Y regenerar los `.pb.h`/`.pb.c`.

**Regla general**: Todo módulo configurable vía CLI necesita estar en AMBOS protos:
- `module_config.proto` → para el `AdminMessage.set_module_config` oneof (campo N)
- `localonly.proto` → para que el firmware pueda persistir la config en NVS

---

## PROBLEMA 4 — `meshtastic --get moduleConfig.remote_switch` → attribute not found

**Síntoma**: El CLI de Python rechaza el campo con "does not have attribute remote_switch".

**Causa**: `meshtastic-python` (la herramienta CLI) tiene sus propios archivos `*_pb2.py` generados desde sus propios `.proto`. Son completamente independientes del firmware. Agregar campos al firmware no los hace visibles en la CLI automáticamente.

**Solución**:
1. Clonar `meshtastic/python` localmente
2. Copiar los `.proto` modificados del firmware al repo de Python en `protobufs/meshtastic/`
3. Regenerar `*_pb2.py` con nanopb-0.4.8 (la versión del repo Python, distinta a la del firmware):
   ```bash
   ./nanopb-0.4.8/generator-bin/protoc -I=$TMPDIR/in --python_out "${OUTDIR}" $INDIR/*.proto
   ```
4. Arreglar imports relativos:
   ```bash
   sed -i -E 's/^from meshtastic.protobuf import/from . import/' meshtastic/protobuf/*pb2.py
   ```
5. Instalar en modo editable: `pip install -e .`

**Trampas**:
- nanopb-0.4.8 (Python repo) ≠ nanopb-0.4.9 (firmware). Usar el correcto para cada uno.
- Si mypy-protobuf no está instalado, omitir `--mypy_out`.

---

## PROBLEMA 5 — Sintaxis incorrecta del CLI (`moduleConfig.remote_switch` vs `remote_switch`)

**Síntoma**: `meshtastic --get moduleConfig.remote_switch` falla. `meshtastic --get remote_switch` funciona.

**Causa**: La CLI de meshtastic-python usa el NOMBRE DEL CAMPO dentro de `LocalConfig`/`LocalModuleConfig` como prefijo, no el tipo de mensaje. Para módulos, el prefijo es el nombre del campo (ej: `remote_switch`, `detection_sensor`), no `moduleConfig.`.

**Forma correcta**:
```
meshtastic --get remote_switch.enabled
meshtastic --set remote_switch.rule1.pin 48
```

---

## PROBLEMA 6 — `meshtastic --set remote_switch.*` acepta el comando pero no persiste

**Síntoma**: `--set remote_switch.enabled true` muestra "Writing remote_switch configuration to device", el dispositivo resetea (normal), pero `--get remote_switch.enabled` devuelve `False`.

**Causa raíz en `node.py`**: El método `writeConfig()` en `meshtastic-python` tiene un switch hardcodeado con todos los módulos conocidos. Faltaba el case para `remote_switch`. Sin este case, el método llama `our_exit("Error: No valid config with name remote_switch")`.

**Fix en `meshtastic-python/meshtastic/node.py`**:
```python
elif config_name == "remote_switch":
    p.set_module_config.remote_switch.CopyFrom(self.moduleConfig.remote_switch)
```
Agregar antes del `else: our_exit(...)`.

**Causa raíz en `NodeDB.cpp`** (firmware): La función `saveToDisk()` hardcodea `has_X = true` para todos los módulos conocidos antes de llamar a `saveProto()`. Sin esta línea para `remote_switch`, el campo no se serializa en el NVS aunque el handler lo haya configurado correctamente.

**Fix en `src/mesh/NodeDB.cpp`**:
```cpp
// Dentro del bloque if (saveWhat & SEGMENT_MODULECONFIG)
moduleConfig.has_remote_switch = true;
```
Agregar junto a las demás líneas `moduleConfig.has_X = true`.

---

## PROBLEMA 7 — AdminModule: handler para `set_module_config` no incluye `remote_switch`

**Causa**: `AdminModule.cpp` tiene un switch sobre `c.which_payload_variant` que lista cada módulo explícitamente. Los módulos nuevos deben agregarse manualmente.

**Fix en `src/modules/AdminModule.cpp`** — handler SET:
```cpp
case meshtastic_ModuleConfig_remote_switch_tag:
    LOG_INFO("Set module config: Remote Switch");
    moduleConfig.has_remote_switch = true;
    moduleConfig.remote_switch = c.payload_variant.remote_switch;
    break;
```

**Fix en `src/modules/AdminModule.cpp`** — handler GET:
```cpp
case meshtastic_AdminMessage_ModuleConfigType_REMOTESWITCH_CONFIG:
    res.get_module_config_response.which_payload_variant = meshtastic_ModuleConfig_remote_switch_tag;
    res.get_module_config_response.payload_variant.remote_switch = moduleConfig.remote_switch;
    break;
```

---

## PROBLEMA 8 — meshtastic-python en Windows vs WSL

**Síntoma**: Después de instalar el paquete en WSL, `meshtastic` desde CMD de Windows sigue usando la versión de PyPI (sin `remote_switch`).

**Causa**: WSL y Windows tienen instalaciones de Python completamente separadas.

**Solución**: Instalar el repo de Python en modo editable TAMBIÉN en Windows:
```
cd C:\Users\...\meshtastic-python
pip install -e .
```

---

## PROBLEMA 9 — `meshtastic --get remote_switch.enabled` retorna `False` aunque el NVS tiene `True`

**Causa 1 — `PhoneAPI.cpp` no envía `remote_switch` en el handshake**:
El firmware tiene un switch en `STATE_SEND_MODULECONFIG` que itera desde `_MIN+1=1` hasta `_MAX+1=17` (usando `_meshtastic_AdminMessage_ModuleConfigType_MAX+1`). Las cases usan `meshtastic_ModuleConfig_xxx_tag` (field numbers en `ModuleConfig` oneof). El stock firmware no tenía case para `remote_switch_tag = 17`. Cuando `config_state=17`, caía al `default` que logueaba "Unhandled" y enviaba un config vacío.

**Fix en `src/mesh/PhoneAPI.cpp`** (al final del switch, antes del `default`):
```cpp
case meshtastic_ModuleConfig_remote_switch_tag:
    LOG_DEBUG("Send module config: remote switch");
    fromRadioScratch.moduleConfig.which_payload_variant = meshtastic_ModuleConfig_remote_switch_tag;
    fromRadioScratch.moduleConfig.payload_variant.remote_switch = moduleConfig.remote_switch;
    break;
```

**Causa 2 — `mesh_interface.py` no procesa `remote_switch` en el handshake**:
`meshtastic-python/meshtastic/mesh_interface.py` tiene un `elif` chain que copia cada módulo de `fromRadio.moduleConfig` a `self.localNode.moduleConfig`. La cadena terminaba en `traffic_management` (sin case para `remote_switch`, `tak`, ni `statusmessage`). El paquete caía al `else: logger.debug("Unexpected FromRadio payload")` y el valor se descartaba.

**Fix en `meshtastic/mesh_interface.py`** (antes del `else:` al final del bloque):
```python
elif fromRadio.moduleConfig.HasField("remote_switch"):
    self.localNode.moduleConfig.remote_switch.CopyFrom(
        fromRadio.moduleConfig.remote_switch
    )
```

**Regla general para módulos futuros**: Hay que tocar CUATRO switches hardcodeados para que el GET funcione de punta a punta:
1. `AdminModule.cpp` — SET handler
2. `AdminModule.cpp` — GET handler
3. `NodeDB.cpp` — `has_X = true` en `saveToDisk()`
4. `PhoneAPI.cpp` — `STATE_SEND_MODULECONFIG` switch (envío en handshake)
5. `mesh_interface.py` — `elif` chain de recepción en Python

---

## PROBLEMA 10 — Módulo `RemoteSwitchModule` sobrante enviando "switch:1" cada 10s

**Síntoma**: Serial muestra `[RemoteSwitch] RemoteSwitch: sending "switch:1"` cada 10 segundos.

**Causa**: Se creó un módulo prototipo `RemoteSwitchModule.cpp` (diferente al `RemoteSwitchReceiverModule`) durante el desarrollo. Este módulo envía texto "switch:1" por LoRa cada 10 segundos en el portnum TEXT_MESSAGE_APP. No existía en el firmware stock (ambos archivos son `??` en `git status`).

**Fix**: Eliminar la línea `#include "modules/RemoteSwitchModule.h"` y la instanciación `remoteSwitchModule = new RemoteSwitchModule();` de `src/modules/Modules.cpp`.

---

## RESUMEN — Archivos a modificar para agregar un módulo configurable custom

Para cualquier módulo futuro, los archivos que hay que tocar son:

### Protos (regenerar después de cada cambio):
- `protobufs/meshtastic/module_config.proto` → agregar `RemoteSwitchConfig remote_switch = 17;` en `ModuleConfig.payload_variant`
- `protobufs/meshtastic/localonly.proto` → agregar `ModuleConfig.RemoteSwitchConfig remote_switch = 18;` en `LocalModuleConfig`
- `protobufs/meshtastic/admin.proto` → agregar `REMOTESWITCH_CONFIG = 16` en `ModuleConfigType` enum
- `protobufs/meshtastic/module_config.options` → agregar max_size para campos string

### Firmware C++:
- `src/modules/AdminModule.cpp` → agregar case en switch SET y case en switch GET
- `src/mesh/NodeDB.cpp` → agregar `moduleConfig.has_remote_switch = true;` en el bloque de save
- `src/mesh/PhoneAPI.cpp` → agregar case en `STATE_SEND_MODULECONFIG` switch (CRÍTICO: sin esto, el GET siempre devuelve el valor default)
- `src/modules/RemoteSwitchReceiverModule.h` → header del módulo
- `src/modules/RemoteSwitchReceiverModule.cpp` → implementación
- `src/modules/Modules.cpp` → instanciar el módulo (sin agregar módulos sobrantes de prototipo)

### meshtastic-python (CLI):
- `meshtastic/protobuf/module_config_pb2.py` → regenerar
- `meshtastic/protobuf/localonly_pb2.py` → regenerar
- `meshtastic/node.py` → agregar case en `writeConfig()`
- `meshtastic/mesh_interface.py` → agregar `elif` en el bloque de recepción de `moduleConfig` (CRÍTICO: sin esto, el GET siempre devuelve el valor default)
