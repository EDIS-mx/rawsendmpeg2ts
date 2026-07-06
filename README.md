# rawsendmpeg2ts

Reproduce una vez un archivo MPEG-TS CBR sobre UDP, sin remux ni modificación del payload. Es un test mínimo para alimentar un IRD de hardware con datagramas uniformemente espaciados. El programa está escrito en C11 y usa únicamente POSIX y sockets IPv4. No pretende ser un sender de producción ni una herramienta MPEG-TS general.

<img width="543" height="461" alt="Captura de pantalla_20260703_183618" src="https://github.com/user-attachments/assets/38ed49d3-7b1d-4b70-b5a7-baf8347c697f" />


## Build

```bash
cmake -B build
cmake --build build

# El binario queda en `build/rawsendmpeg2ts`.
$ rawsendmpeg2ts <file.ts|--stdin> <ip:port>
```

Ejemplo:

```bash
./build/rawsendmpeg2ts /path/to/golden.ts 239.1.74.20:9009
```

Un TS CBR null-stuffed también puede llegar por stdin. El sender conserva en memoria el tramo de
aproximadamente un segundo usado para derivar la tasa y después continúa leyendo la tubería:

```bash
cat /path/to/golden.ts |
  ./build/rawsendmpeg2ts --stdin 239.1.74.20:9009
```

Para convertir la salida TS non-stuffed de MoQ en CBR antes del pacing UDP:

```bash
/home/ariel/projects/moq-dev.main/target/release/moq \
    --client-connect https://192.168.99.6 \
    --client-tls-disable-verify \
    --broadcast test/bbb \
    export ts |
  ffmpeg -hide_banner -loglevel warning \
    -i pipe:0 -map 0:v:0 -map 0:a:0 -c copy \
    -muxrate 11M -pcr_period 20 -f mpegts pipe:1 |
  ./build/rawsendmpeg2ts --stdin 239.1.74.20:9009
```

FFmpeg realiza el remux y agrega null stuffing. `rawsendmpeg2ts` deriva el muxrate resultante y corrige
la cadencia UDP. No usar `-re` en esta tubería: el sender raw es el único dueño del pacing final.

El destino debe ser IPv4. Para multicast se configura TTL 4 y se desactiva multicast loopback. La
interfaz de egreso la decide la ruta del sistema operativo:

```bash
ip route get 239.1.74.20
```

## Qué hace

1. Verifica que el archivo sea una secuencia completa de paquetes TS de 188 bytes.
2. Encuentra el primer PID con PCR.
3. Mide aproximadamente un segundo de separación PCR y deriva la tasa a partir de bytes/PCR.
4. Rebobina el archivo o reproduce el prefijo guardado cuando la entrada es stdin.
5. Envía siete paquetes TS por datagrama UDP, 1316 bytes, contra deadlines absolutos de
   `CLOCK_MONOTONIC`.
6. Termina al llegar a EOF. No hace loop.

El socket es bloqueante y usa `SO_SNDBUF=32768`. Los deadlines permanecen anclados al inicio para no
reducir la tasa de largo plazo después de un atraso temporal.

El programa reporta cada cinco segundos los wakes que llegaron más de dos intervalos de datagrama
tarde. Son telemetría, no una condición de aborto:

```text
[t+ 500.0s] slips this 5s: 3 (worst 1.9 ms at t+496.6s), total 303
```

Un número pequeño y acotado de slips es normal bajo el scheduler de Linux. Lo preocupante es atraso
creciente, backpressure sostenido o una correlación visible con freezes del IRD.

## Qué no hace

- No remuxa, rellena, elimina ni reescribe paquetes TS.
- No corrige PCR, PTS, DTS, continuity counters ni T-STD.
- No soporta VBR como caso general. La tasa se deriva suponiendo una relación CBR entre PCR y posición
  de byte.
- No selecciona interfaz multicast explícitamente.
- No soporta IPv6, reproducción en loop ni múltiples programas configurables.
- No configura `SCHED_FIFO`, afinidad de CPU ni `mlockall`.

## Cama de pruebas necesaria

Se validó con un enlace directo (los switches pueden tener storm control multicast e invalidan la prueba, hacen _pacing_ falso y parece que pasa, o detienen el _storm_ y parece que falla).

**Pare de sufrir, conecte un cable directo al IRD**:

```text
PC/NIC -> cable Ethernet -> Sencore
```
## ¿Audio? ¡Si!
Es importante probar con audio, clics y micro silencios evidencian rápidamente problemas que sólo video oculta.

**Pare de sufrir, conecte una bocina**:

## Switches / red

No debe haber switch entre el sender y el IRD. El PC y el Sencore necesitan direcciones estáticas en
la misma subred. Multicast funciona sobre el enlace directo y la ruta debe apuntar a la NIC conectada
al Sencore.

Antes de cada prueba:

```bash
ip route get 239.1.74.20
ethtool <interfaz> | grep -E 'Speed|Duplex|Link detected'
ethtool -S <interfaz> | grep -E 'rx_flow_control_xon|rx_flow_control_xoff'
```

Durante la corrida, `rx_flow_control_xoff` debe permanecer estable. El resultado se evalúa durante el
archivo completo, no solamente durante un smoke test: reproducción A/V limpia, terminación normal,
sin atraso creciente y sin backpressure sostenido.

### Por qué un switch invalida la prueba

Un switch puede aplicar storm-control, multicast policing, rate limiting, shaping o PAUSE 802.3x. Eso
puede cambiar la tasa y la cadencia aunque el puerto negocie a 100 Mb/s o 1 Gb/s. El sender entonces
mide comportamiento de la ruta y no solamente del archivo, su scheduler y el IRD.

En este banco, la ruta a través de un switch produjo el siguiente falso diagnóstico:

- El TS declaraba 11 Mb/s, pero FFmpeg tardó 689.866 s en transportar 596.459 s de media.
- El throughput UDP real fue 9.511 Mb/s, casi exactamente un límite Ethernet de 10 Mb/s después de
  overhead.
- `rx_flow_control_xoff` crecía unas 65 veces por segundo.
- `rawsendmpeg2ts` acumulaba atraso y el IRD presentaba micro-freezes.
- Con cable directo, el mismo TS corrió completo a 11 Mb/s, sin XOFF y con A/V perfecto.
- El enlace directo también reprodujo correctamente un TS broadcast de 20 Mb/s.

Por eso una prueba detrás de un switch es diagnóstica, no un Gate. Si resulta distinta al cable
directo, hay que revisar storm-control, policing multicast, flow control y configuración de puertos.

## Demostración local sin IRD

`udp_sink.py` recibe datagramas, mide throughput e inter-arrival y puede guardar un prefijo para probar
fidelidad de bytes.

```bash
python3 udp_sink.py \
  --port 5004 \
  --seconds 9 \
  --save rx.bin \
  --stats stats.json &

./build/rawsendmpeg2ts /path/to/golden.ts 127.0.0.1:5004

cmp -n 1000000 rx.bin /path/to/golden.ts
cat stats.json
```

Loopback sirve como smoke test. Para medir pacing real se necesita una captura en el lado receptor.

## Goldens CBR stuffed de Big Buck Bunny

Todos los goldens usados en el banco son H.264, 1920x1080i59.94 top-field-first, AAC 5.1 a 48 kHz,
PCR cada 20 ms y MPEG-TS CBR con null stuffing.

Fuente común:

```text
/home/ariel/projects/moq-dev/notes/captures/big_buck_bunny_1080p_h264.mov
```

El `.mov` original se descargó del archivo oficial de películas de Big Buck Bunny de Blender:

```text
https://download.blender.org/peach/bigbuckbunny_movies/
```

Los archivos `.ts` están fuera del repositorio. Las recetas requieren FFmpeg con `libx264`.


```bash
### Generar 4 Mb/s
ffmpeg -hide_banner -y \
  -i /home/ariel/projects/moq-dev/notes/captures/big_buck_bunny_1080p_h264.mov \
  -map 0:v:0 -map 0:a:0 \
  -vf "fps=30000/1001" \
  -c:v libx264 -preset veryfast -profile:v main -level:v 4.1 -pix_fmt yuv420p \
  -flags +ilme+ildct \
  -x264opts "interlaced=1:tff=1:nal-hrd=cbr:keyint=30" \
  -b:v 3M -minrate 3M -maxrate 3M -bufsize 3M \
  -c:a aac -b:a 160k \
  -muxrate 4M -pcr_period 20 \
  -f mpegts \
  /home/ariel/projects/moq-dev/notes/captures/golden-bbb-1080i5994-4m.ts

### Generar 8 Mb/s
ffmpeg -hide_banner -y \
  -i /home/ariel/projects/moq-dev/notes/captures/big_buck_bunny_1080p_h264.mov \
  -map 0:v:0 -map 0:a:0 \
  -vf "fps=30000/1001" \
  -c:v libx264 -preset veryfast -pix_fmt yuv420p \
  -flags +ilme+ildct \
  -x264opts "interlaced=1:tff=1:nal-hrd=cbr:keyint=30" \
  -b:v 6M -minrate 6M -maxrate 6M -bufsize 6M \
  -c:a copy \
  -muxrate 8M -pcr_period 20 \
  -f mpegts \
  /home/ariel/projects/moq-dev/notes/captures/golden-bbb-1080i5994-8m.ts

### Generar 11 Mb/s
ffmpeg -hide_banner -y \
  -i /home/ariel/projects/moq-dev/notes/captures/big_buck_bunny_1080p_h264.mov \
  -map 0:v:0 -map 0:a:0 \
  -vf "fps=30000/1001" \
  -c:v libx264 -preset veryfast -pix_fmt yuv420p \
  -flags +ilme+ildct \
  -x264opts "interlaced=1:tff=1:nal-hrd=cbr:keyint=30" \
  -b:v 8M -minrate 8M -maxrate 8M -bufsize 8M \
  -c:a copy \
  -muxrate 11M -pcr_period 20 \
  -f mpegts \
  /home/ariel/projects/moq-dev/notes/captures/golden-bbb-1080i-clean.ts

### Generar 20 Mb/s
ffmpeg -hide_banner -y \
  -i /home/ariel/projects/moq-dev/notes/captures/big_buck_bunny_1080p_h264.mov \
  -map 0:v:0 -map 0:a:0 \
  -vf "fps=30000/1001" \
  -c:v libx264 -preset veryfast -pix_fmt yuv420p \
  -flags +ilme+ildct \
  -x264opts "interlaced=1:tff=1:nal-hrd=cbr:keyint=30" \
  -b:v 17M -minrate 17M -maxrate 17M -bufsize 17M \
  -c:a copy \
  -muxrate 20M -pcr_period 20 \
  -f mpegts \
  /home/ariel/projects/moq-dev/notes/captures/golden-bbb-1080i5994-20m.ts
```

## Emitir los golden specimens vis rawsendmpeg2ts

```bash
### 4 Mb/s
/home/ariel/projects/rawsendmpeg2ts/build/rawsendmpeg2ts \
  /home/ariel/projects/moq-dev/notes/captures/golden-bbb-1080i5994-4m.ts \
  239.1.74.20:9009

### 8 Mb/s
/home/ariel/projects/rawsendmpeg2ts/build/rawsendmpeg2ts \
  /home/ariel/projects/moq-dev/notes/captures/golden-bbb-1080i5994-8m.ts \
  239.1.74.20:9009

### 11 Mb/s
/home/ariel/projects/rawsendmpeg2ts/build/rawsendmpeg2ts \
  /home/ariel/projects/moq-dev/notes/captures/golden-bbb-1080i-clean.ts \
  239.1.74.20:9009

### 20 Mb/s
/home/ariel/projects/rawsendmpeg2ts/build/rawsendmpeg2ts \
  /home/ariel/projects/moq-dev/notes/captures/golden-bbb-1080i5994-20m.ts \
  239.1.74.20:9009
```

## Emitir con TSDuck

Los cuatro casos también produjeron A/V correcto en el IRD con TSDuck nativo por cable directo. La dirección local fija el egreso a la NIC `10.6.6.1` y evita que multicast use otra interfaz.

```bash
### 4 Mb/s
tsp \
  -I file /home/ariel/projects/moq-dev/notes/captures/golden-bbb-1080i5994-4m.ts \
  -P regulate --bitrate 4000000 --packet-burst 7 \
  -O ip --local-address 10.6.6.1 --ttl 4 --packet-burst 7 \
  239.1.74.20:9009

### 8 Mb/s
tsp \
  -I file /home/ariel/projects/moq-dev/notes/captures/golden-bbb-1080i5994-8m.ts \
  -P regulate --bitrate 8000000 --packet-burst 7 \
  -O ip --local-address 10.6.6.1 --ttl 4 --packet-burst 7 \
  239.1.74.20:9009

### 11 Mb/s
tsp \
  -I file /home/ariel/projects/moq-dev/notes/captures/golden-bbb-1080i5994.ts \
  -P regulate --bitrate 11000000 --packet-burst 7 \
  -O ip --local-address 10.6.6.1 --ttl 4 --packet-burst 7 \
  239.1.74.20:9009

### 20 Mb/s
tsp \
  -I file /home/ariel/projects/moq-dev/notes/captures/golden-bbb-1080i5994-20m.ts \
  -P regulate --bitrate 20000000 --packet-burst 7 \
  -O ip --local-address 10.6.6.1 --ttl 4 --packet-burst 7 \
  239.1.74.20:9009
```

## Artifacts verificados

| Mux | Archivo | Tamaño | Bitrate medido | Null packets | SHA-256 |
|---:|---|---:|---:|---:|---|
| 4 Mb/s | `golden-bbb-1080i5994-4m.ts` | 298,232,672 bytes | 3,999,875 bit/s | 268,589 (16.9%) | `b2374b2982f160873c784fe14ca0184bc9913a4859da4c746d0dafef9c04ff7e` |
| 8 Mb/s | `golden-bbb-1080i5994-8m.ts` | 596,506,140 bytes | 8,000,584 bit/s | 522,046 (16.5%) | `72deb806f6fcf7f0e42fa723febadd82bbe167442c0bf0a7641987df88edc874` |
| 11 Mb/s | `golden-bbb-1080i-clean.ts` | 820,195,308 bytes | 11,000,795 bit/s | 901,466 (20.7%) | `81d4b5ab2613661e53e95d88e8f32f33b83272335cd33d288bc7b9a281817560` |
| 20 Mb/s | `golden-bbb-1080i5994-20m.ts` | 1,491,262,248 bytes | 20,001,550 bit/s | 828,215 (10.4%) | `c10208f2a135fdddefb32c42a0a4142db309c3d02fe4399d7e2721a186891723` |

Los hashes identifican los artifacts usados en el banco. Distintas versiones de FFmpeg o `libx264`
pueden producir un archivo diferente usando los mismos parámetros.

## Resultados directos con el Sencore

| Mux | Corrida | Slips | Resultado |
|---:|---|---:|---|
| 4 Mb/s | parcial | 0 | A/V perfecto |
| 8 Mb/s | parcial | 0 | A/V perfecto |
| 11 Mb/s | completa, ~596 s | 19, máximo 7.966 ms | A/V perfecto |
| 20 Mb/s | completa, ~596 s | 306, máximo 7.294 ms | A/V perfecto |

En la corrida de 20 Mb/s, 290 de los 306 slips se concentraron en un episodio de unos 15 segundos.
No hubo atraso acumulativo ni efecto visible en A/V.

TSDuck nativo también produjo A/V correcto a 4, 8, 11 y 20 Mb/s usando `regulate` y ráfagas de siete
paquetes. En el Gate directo, `rawsendmpeg2ts` alcanza la misma aceptación del IRD que TSDuck.

Logs completos:

```text
logs/full-bbb.log
  logs/full-bbb-20m.log
```

## Comparación con FFmpeg

- Los mismos CBR de 4, 8 y 11 Mb/s produjeron glitches con FFmpeg `-re` por cable directo.
- Los tres funcionaron con `rawsendmpeg2ts` y cero slips en las pruebas comparativas.
- El remux de 4 Mb/s producido por FFmpeg funcionó al reproducirse con `rawsendmpeg2ts`. Esto separó
  el contenido remuxeado de la cadencia UDP.
- FFmpeg regula la lectura por timestamps, pero sus escrituras UDP ocurren en grupos y con tamaños
  variables. Eso no equivale a espaciar uniformemente cada datagrama TS.

La limitación del switch y la cadencia de FFmpeg fueron problemas independientes. El switch explicaba
el backpressure a 11 Mb/s, pero no los glitches de FFmpeg, que también aparecieron a 4 y 8 Mb/s por
cable directo.

## MOV & TS Across MoQ tests with IRD-compliant output

Se probaron tres fuentes end-to-end a través de MoQ: un MOV recodificado live, un TS crudo
1080i59.94 y un TS europeo 1080i50. En los tres casos, MoQ transportó media VBR/non-stuffed y el
egress reconstruyó un MPEG-TS CBR null-stuffed de 11 Mb/s. Los tres produjeron audio y video limpios
en el Sencore por cable directo.

### Ingreso MOV con audio AAC 5.1

```bash
ffmpeg -re -stream_loop -1 \
  -i /home/ariel/projects/poky/downloads/bigbuckbunny1080p.mov \
  -map 0:v:0 -map 0:a:0 \
  -vf "scale=1920:1080,fps=60000/1001,interlace=scan=tff:lowpass=1,setfield=tff,format=yuv420p" \
  -c:v libx264 \
  -profile:v high \
  -level 4.0 \
  -preset veryfast \
  -x264opts "interlaced=1:tff=1:weightp=0" \
  -b:v 9M \
  -maxrate 9M \
  -bufsize 1M \
  -g 30 \
  -bf 2 \
  -c:a copy \
  -f mpegts - |
/home/ariel/projects/moq-dev/target/release/moq-cli publish \
  --url https://192.168.99.6 \
  --client-tls-disable-verify \
  --broadcast test/bbb \
  ts
```

### Ingreso TS crudo 1080i59.94

```bash
ffmpeg -re -stream_loop -1 \
  -i /home/ariel/projects/poky/downloads/bbb_1080i5994.ts \
  -map 0 \
  -c copy \
  -f mpegts pipe:1 |
/home/ariel/projects/moq-dev.main/target/release/moq \
  --client-connect https://192.168.99.6 \
  --client-tls-disable-verify \
  --broadcast test/bbb \
  import ts
```

### Ingreso TS crudo europeo 1080i50

```bash
ffmpeg -re -stream_loop -1 \
  -i /home/ariel/projects/moq-dev/notes/captures/CNNiEMEA2.ts \
  -map 0 \
  -c copy \
  -f mpegts pipe:1 |
/home/ariel/projects/moq-dev.main/target/release/moq \
  --client-connect https://192.168.99.6 \
  --client-tls-disable-verify \
  --broadcast test/bbb \
  import ts
```

Los dos comandos TS usan `-c copy`, por lo que conservan el video codificado, framerate, field order,
audio y relaciones temporales sin recodificar. Al omitir `-muxrate`, el TS entregado al importer es
VBR/non-stuffed. El importer
de MoQ descarta cualquier null stuffing de ingreso de todas formas.

### Egress común hacia el Sencore

```bash
/home/ariel/projects/moq-dev.main/target/release/moq \
  --client-connect https://192.168.99.6 \
  --client-tls-disable-verify \
  --broadcast test/bbb \
  export ts |
ffmpeg -hide_banner -loglevel warning \
  -i pipe:0 \
  -map 0:v:0 \
  -map '0:a:0?' \
  -c copy \
  -muxrate 11M \
  -pcr_period 20 \
  -f mpegts pipe:1 |
/home/ariel/projects/rawsendmpeg2ts/build/rawsendmpeg2ts \
  --stdin 239.1.74.20:9009
```

En el egress, FFmpeg vuelve a crear el multiplexor CBR y sus null packets. `rawsendmpeg2ts` conserva
ese TS byte por byte y aplica el pacing UDP uniforme que necesita el IRD.
