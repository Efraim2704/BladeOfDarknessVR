# BladeVR

*[Read in English](README.md)*

Mod de realidad virtual para **Severance: Blade of Darkness** (remaster DirectX 11 de 2021, Steam).
Añade **estéreo 3D completo** (una imagen distinta para cada ojo) y **cámara 6DOF con seguimiento
de cabeza** a través de SteamVR / OpenVR. Desarrollado y probado con una Meta Quest 2 por SteamVR.

Los ficheros del juego no se modifican. El mod es una DLL que se carga al arrancar `Blade.exe`
(mediante un `dxgi.dll` proxy) y engancha DirectX 11 y unas pocas funciones del motor.

> **Aviso.** Este mod ha sido elaborado íntegramente con ayuda de inteligencia
> artificial, dirigido, supervisado y probado por su autor a lo largo de muchas
> etapas e iteraciones: cada cambio se ha probado en el juego y en
> el visor, y lo que no funcionaba se ha descartado. Es un proyecto de aficionado sin
> relación con los desarrolladores ni la editora del juego. Úsalo bajo tu
> responsabilidad.

## Qué funciona

* **Estéreo 3D real.** Cada draw call 3D se dibuja dos veces, una por ojo, con la proyección
  propia de cada ojo del visor. El mundo, los personajes, las armas, las sombras y el cielo
  tienen profundidad correcta. Los menús y el HUD se muestran en una pantalla virtual cómoda.
* **Seguimiento de cabeza 6DOF.** La cámara sigue la rotación *y* la posición del visor,
  anclada a la cámara del propio juego, tanto en primera como en tercera persona. No se puede
  asomar la cabeza a través de paredes u objetos: se usa la colisión del propio motor.
* **Modo "camino hacia donde miro".** Opcional: el personaje gira para seguir al visor, de
  modo que caminas en la dirección en la que miras.
* En el monitor se ve el ojo izquierdo centrado (con bandas negras a los lados) en vez
  de la imagen partida; también se puede usar la ventana "Vista de RV" de SteamVR.
* Se juega con **teclado y ratón o con mando**, exactamente igual que el juego original.
  No se usan los mandos de VR.
* Funciona desde el menú principal; no hace falta ningún inyector ni lanzador.

## Requisitos

* Blade of Darkness (remaster de 2021, Steam, 64 bits).
* SteamVR y un visor compatible (probado con Quest 2 por Link / Air Link).
* Windows 10/11 x64.

## Instalación

Copiar estos tres ficheros a la carpeta `bin\bin` del juego (la que contiene `Blade.exe`,
normalmente `...\steamapps\common\Blade of Darkness\bin\bin`):

* `dxgi.dll` — proxy que carga el mod
* `BladeVR.dll` — el mod
* `openvr_api.dll` — runtime de OpenVR (se busca junto a `BladeVR.dll`)

Arrancar primero SteamVR y después el juego. Si SteamVR no está en marcha o no hay visor,
el juego funciona con normalidad, sin VR. Para desinstalar, borrar los tres ficheros.

## Ajustes recomendados del juego

* **Resolución: la máxima que permita tu gráfica** (p. ej. 2560x1440). Cada ojo recibe la
  mitad del ancho de pantalla, así que a más resolución, más nitidez en el visor.
* **Campo de visión (FOV): 145**. El motor recorta la geometría en la CPU al FOV del juego;
  con un FOV bajo quedan zonas negras en los bordes del visor.
* **Activa el límite de FPS** en las opciones del juego. Sin él, la cámara se mueve a
  tirones en lugar de con fluidez.

## Teclas

| Tecla | Función |
|---|---|
| **F5** | Activa / desactiva el estéreo (se activa solo al aparecer el menú). |
| **Re Pág / Av Pág** | Separación entre ojos +4 / −4 unidades del juego (por defecto 54). Ajústala hasta que el mundo tenga el tamaño correcto. |
| **Fin** | Tamaño de la pantalla virtual de menús y HUD (53° / 75° / 90°). |
| **Insert** | Seguimiento de cabeza activo (por defecto) / apagado. Al activarlo se recentra la vista. |
| **Espacio** | Recentra la posición: la cámara vuelve a los ojos del personaje. |
| **Supr** | Alterna entre *vista libre* (el personaje no gira con la cabeza) y *camino hacia donde miro* (el personaje gira siguiendo al visor). |

## Cómo funciona

**Carga.** Windows busca las DLL en la carpeta del ejecutable antes que en System32, así que
el juego carga nuestro `dxgi.dll`. Este reenvía todas las funciones al DXGI real del sistema
(cargado por ruta absoluta) y en su `DllMain` carga `BladeVR.dll`. Además intercepta
`CreateDXGIFactory*` para pasar el factory al mod, que engancha
`IDXGIFactory::CreateSwapChain` y así conoce el device D3D11 del juego antes del primer
`Present`.

**Estéreo** (`StereoHook.cpp`). El motor transforma los vértices en la CPU; la GPU solo
recibe geometría en espacio de cámara más una única matriz de proyección en el constant
buffer de VS slot 0 (`ProjectionHook.cpp` la captura). Cada draw call 3D se emite dos veces
con todo el estado del juego intacto, cambiando solo el viewport (mitad izquierda / derecha
del render target) y ese constant buffer (matriz de cada ojo construida con el frustum
asimétrico del visor más un desplazamiento de ojo off-axis). El backbuffer queda en formato
lado a lado y se entrega a SteamVR con bounds `0..0.5` / `0.5..1`. La interfaz se dibuja en
una pantalla virtual a 1,8 m; el cielo, sin desplazamiento de ojo (al infinito).

**Seguimiento de cabeza** (`HeadTrackHook.cpp`). La matriz de vista global del motor
(`fromWorld`, 4x4 doubles) la escribe una función una vez por fotograma. Se engancha esa
función y, tras la última llamada de cada fotograma, se reescribe la matriz entera con la
pose del visor: la rotación completa del casco (con la guiñada anclada a la de la cámara del
juego) y la posición del casco sumada a la de la cámara, convertida a unidades del juego y
recortada con el trazado de rayos del propio motor para no atravesar paredes ni objetos. El
bloque de cámara del culling se sincroniza con la vista reescrita
(`SceneCullingRootHook.cpp`) para que sombras y entidades se vean en todas direcciones. La
pose usada para dibujar se adjunta al `Submit` (`Submit_TextureWithPose`) para que el
compositor reproyecte correctamente.

**Direcciones del juego.** Los offsets de `Blade.exe` (constantes `k...` al principio de
`HeadTrackHook.cpp`, `SceneCullingRootHook.cpp` y `FromWorldLocator.cpp`) corresponden a la
versión de Steam del remaster; si el juego se actualiza habrá que revisarlos.

## Compilar

Requisitos: Visual Studio 2019/2022 con "Desarrollo para el escritorio con C++", CMake 3.16+
y git (MinHook se descarga en el configure).

Desde una consola con CMake en el PATH (por ejemplo la "x64 Native Tools Command Prompt
for VS"), en la carpeta del repositorio:

```
cmake -B build -A x64
cmake --build build --config Release
```

Después, copiar los tres ficheros de `output\Release\` (`dxgi.dll`, `BladeVR.dll` y
`openvr_api.dll`) junto al ejecutable del juego, en `Blade of Darkness\bin\bin\`.

## Log de depuración

El mod no escribe ningún fichero por defecto. Para obtener un log, crea un fichero vacío
llamado `BladeVR_debug.txt` junto a `Blade.exe`; el mod escribirá entonces
`bin\bin\BladeVR_logs\BladeVR_<pid>_<fecha>.log` en cada ejecución.

## Estructura

```
hookdll/                 BladeVR.dll
  dllmain.cpp            punto de entrada, orden de instalación de los módulos
  Dx11Hook.*             Present/ResizeBuffers/CreateSwapChain, entrega a SteamVR
  ProjectionHook.*       captura de la matriz de proyección (VS slot 0)
  StereoHook.*           estéreo por viewport partido y teclas de estéreo
  OpenVRHook.*           SteamVR: init, poses, frustums, Submit
  HeadTrackHook.*        seguimiento de cabeza 6DOF y colisión
  SceneCullingRootHook.* culling coherente con la vista reescrita
  FromWorldLocator.*     localiza la matriz de vista global en memoria
  HookLogger.*           log opcional
proxydll/                dxgi.dll proxy (ProxyMain.cpp + thunks MASM)
ThirdParty/openvr/       openvr.h, openvr_api.lib, openvr_api.dll (OpenVR SDK)
```

## Limitaciones conocidas

* La selección de personaje y la cinemática inicial de cada personaje se ven en una pantalla
  rectangular con márgenes negros (efecto de "caja"), y algunos objetos se salen hacia la
  zona negra.
* El vídeo de introducción y la primera pantalla de carga se ven con separación incorrecta
  (el estéreo se activa al aparecer el menú).
* En los bordes de algunos portales pueden verse finas franjas negras: el motor recorta la
  geometría de cada sector contra el portal desde la cámara central, no desde cada ojo.
* Las armas y el escudo atraviesan paredes y objetos como en el juego original, y en tercera
  persona la cámara puede meterse en el personaje.

## Licencia

BladeVR se publica bajo la [licencia MIT](LICENSE), copyright (c) 2026 Efraim27.
Puedes usarlo, modificarlo y redistribuirlo libremente siempre que se conserven el
aviso de copyright y el texto de la licencia, es decir, que se acredite al autor.

## Terceros

* [MinHook](https://github.com/TsudaKageyu/minhook) (licencia BSD de 2 cláusulas).
* [OpenVR SDK](https://github.com/ValveSoftware/openvr) (licencia BSD de 3 cláusulas).
