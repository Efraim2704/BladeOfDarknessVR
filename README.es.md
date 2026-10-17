# BladeVR

*[Read in English](README.md)*

Mod de realidad virtual para **Severance: Blade of Darkness** (remaster DirectX 11 de 2021, Steam).
Añade **estéreo 3D completo** (una imagen distinta para cada ojo) y **cámara 6DOF con seguimiento
de cabeza** a través de SteamVR / OpenVR. Desarrollado y probado con una Meta Quest 2 por SteamVR.

Los ficheros del juego no se modifican. El mod es una DLL que se carga al arrancar `Blade.exe`
(mediante un `dxgi.dll` proxy) y engancha DirectX 11 y unas pocas funciones del motor.

**Novedad de la 2.0: el [combate con mandos de movimiento](#mandos-de-movimiento-en-el-juego).**
Tus manos son las del personaje: mueve el arma para golpear, alza el escudo para bloquear, saca
una flecha de la espalda y tensa el arco, da puñetazos con las manos vacías, acércate la comida
a la boca para comer. Los mandos de Quest son además un mando completo para todo lo demás, y hay
cuatro modos de combate, desde los mandos de movimiento completos hasta el juego de siempre con
los mandos haciendo de mando de Xbox.

Desde la 1.6: el [modo diorama](#modo-diorama). Con una tecla el nivel entero se convierte en
una maqueta sobre una mesa virtual en la que sigues jugando.

> **Aviso.** Este mod ha sido elaborado íntegramente con ayuda de inteligencia
> artificial, dirigido, supervisado y probado por su autor a lo largo de muchas
> etapas e iteraciones: cada cambio se ha probado en el juego y en
> el visor, y lo que no funcionaba se ha descartado. Es un proyecto de aficionado sin
> relación con los desarrolladores ni la editora del juego. Úsalo bajo tu
> responsabilidad.

## Qué funciona

* **Estéreo 3D real.** El motor dibuja el mundo dos veces por fotograma, una desde cada ojo,
  con la proyección propia de cada ojo del visor. Cada ojo tiene su propio recorte por
  portales, sus sombras, sus reflejos y su iluminación, así que no hay costuras en los bordes
  de puertas y columnas. El mundo, los personajes, las armas, las sombras, el agua y el cielo
  tienen profundidad correcta. Los menús y el HUD se muestran en una pantalla virtual cómoda.
* **Seguimiento de cabeza 6DOF.** La cámara sigue la rotación *y* la posición del visor,
  anclada a la cámara del propio juego, tanto en primera como en tercera persona. No se puede
  asomar la cabeza a través de paredes u objetos: se usa la colisión del propio motor.
* **Combate con mandos de movimiento.** En primera persona los brazos del personaje siguen a
  tus manos y su cuerpo se queda bajo el visor. Las armas golpean donde de verdad tocan (la
  detección de golpes del propio motor, con chispas, sangre y cortes), al mismo ritmo y con el
  mismo gasto de resistencia que el ataque básico del juego. El escudo, las armas a dos manos,
  el arco, los puñetazos, lanzar, sacar y guardar las armas, la comida y las pociones
  funcionan con gestos.
* **Los mandos de Quest como mando del juego.** Le llegan al juego como un mando de Xbox más,
  así que los menús, los combos, el inventario y todo lo demás funcionan como con un mando de
  verdad. Se pueden usar a la vez un mando de verdad y el teclado.
* **Modo "camino hacia donde miro".** El personaje gira para seguir al visor, de modo que
  caminas en la dirección en la que miras (activo de serie con los mandos de movimiento). El
  giro le llega solo al juego, así que el cursor del ratón de Windows no se mueve.
* En el monitor se ve el ojo izquierdo centrado (con bandas negras a los lados) en vez
  de la imagen partida; también se puede usar la ventana "Vista de RV" de SteamVR.
* **Modo diorama.** El nivel entero como maqueta sobre una mesa, con cortes, cono de
  visión, tres modos de maqueta, cinemáticas y fondo para realidad mixta (ver más abajo).
* Funciona desde el menú principal; no hace falta ningún inyector ni lanzador.

## Requisitos

* Blade of Darkness (remaster de 2021, Steam, 64 bits).
* SteamVR y un visor compatible (probado con Quest 2 por Link / Air Link y Virtual Desktop).
* Windows 10/11 x64.
* Mandos de movimiento: los de Quest (Touch) por SteamVR. Le llegan al juego por Steam Input,
  que es lo que usa el juego de serie: déjalo activado.
* Modo diorama con mando: Steam Input. Realidad mixta: Virtual Desktop (opcional).

## Instalación

Copia estos tres ficheros en la carpeta `bin\bin` del juego (la que contiene `Blade.exe`,
normalmente `...\steamapps\common\Blade of Darkness\bin\bin`):

* `dxgi.dll` — proxy que carga el mod
* `BladeVR.dll` — el mod
* `openvr_api.dll` — runtime de OpenVR (se busca junto a `BladeVR.dll`)

Arranca primero SteamVR y después el juego. Si SteamVR no está en marcha o no hay visor,
el juego funciona con normalidad, sin VR. Para actualizar, basta con sobrescribir los tres
ficheros.

En esa misma carpeta el mod crea la carpeta `BladeVR_bindings` (los enlaces de los mandos que
le da a SteamVR) y, la primera vez que cambias un ajuste, `BladeVR.ini`: el último modo de
combate, la calibración, la resistencia sí o no, la altura de los ojos y si saliste del juego
en el modo diorama (entonces la siguiente partida empieza en él). Para desinstalar, borra los
tres ficheros, `BladeVR_bindings` y `BladeVR.ini` (y `BladeVR_logs` y `BladeVR_debug.txt` si
sacaste un log de depuración).

Si al arrancar el juego se abre solo el panel de SteamVR, abre las *Propiedades* del juego en
Steam y desactiva *Use Desktop Game Theatre while SteamVR is active* (usar el teatro de juegos
de escritorio con SteamVR).

## Ajustes recomendados del juego

* **Resolución: la máxima que permita tu gráfica** (p. ej. 2560x1440). Cada ojo recibe la
  mitad del ancho de pantalla, así que a más resolución, más nitidez en el visor.
* **Suavizado de bordes: desactivado**. Este no es opcional: con él activado se pierde la
  imagen en estéreo.
* **Desenfoque de movimiento: desactivado**. Emborrona la imagen dentro del visor.
* **Florecimiento: desactivado** (viene así de fábrica, y solo se habilita con el HDR
  activado). El resplandor se calcula sobre el fotograma completo, que lleva los dos ojos uno
  al lado del otro, así que el halo de una antorcha se derrama de la imagen de un ojo a la
  del otro.
* **Oclusión ambiental: mejor desactivada**. También se calcula en espacio de pantalla, así
  que sale mal calculada en la unión de los dos ojos, y consume tiempo de fotograma que en VR
  hace falta.
* **HDR: a tu gusto.** Solo cambia la precisión con la que se calcula la iluminación, no cómo
  se compone la imagen, así que no molesta de ninguna de las dos maneras.
* **Activa el límite de FPS** en las opciones del juego. Sin él, la cámara se mueve a
  tirones en lugar de con fluidez.
* **Campo de visión (FOV): no lo toques.** Mientras el mod está en marcha fuerza un mínimo de
  145 grados, diga lo que diga el menú, para que no aparezcan márgenes negros en los bordes
  de la vista.

## Teclas

| Tecla | Función |
|---|---|
| **Re Pág / Av Pág** | Separación entre ojos +4 / −4 unidades del juego (por defecto 58). Ajústala hasta que el mundo tenga el tamaño correcto. |
| **Ctrl + Re Pág / Av Pág** | Ojos más adelantados / más atrás en la cabeza del personaje, 1 cm cada vez (de serie 10 cm). |
| **Mayús + Re Pág / Av Pág** | Solo en el modo de manos flotantes: ojos más altos / más bajos, 5 cm cada vez (de serie 0; se guarda). |
| **Fin** | Tamaño de la pantalla virtual de menús y HUD (53° / 75° / 90°). |
| **Insert** | Seguimiento de cabeza activo (por defecto) / apagado. Al activarlo se recentra la vista. |
| **Inicio** | Recentra la posición: la cámara vuelve a los ojos del personaje. |
| **Supr** | Alterna entre *vista libre* (el personaje no gira con la cabeza) y *camino hacia donde miro* (el personaje gira siguiendo al visor). Empieza en *camino hacia donde miro* en los modos con mandos de movimiento y en *vista libre* en el modo 0. |
| **Mayús + F8** | Los golpes con los mandos gastan la resistencia (de serie) / no la gastan. Se guarda. |
| **Ctrl + F8** | Calibrar el agarre y el largo de los brazos (opcional, ver más abajo). |
| **F5** | Modo diorama sí / no (ver más abajo). |

## Mandos de movimiento en el juego

### Modos de combate

| Modo | Qué hace |
|---|---|
| **1 · Mandos de movimiento** (de serie) | Los brazos del personaje siguen a los tuyos. Se ataca moviendo el arma; A no ataca. |
| **3 · Manos flotantes** | Como el 1, pero sin el cuerpo: solo los antebrazos, las manos y las armas, que siguen a los mandos sin límite del largo del brazo. Los ojos se pueden subir (Mayús + Re Pág / Av Pág). |
| **2 · Mandos + combos** | A y las direcciones del stick hacen los combos del juego (los brazos siguen la animación del combo), y mover el arma hace el ataque básico. |
| **0 · VR básico** | Sin mandos de movimiento: los mandos de Quest son un mando de Xbox y se juega exactamente igual que en plano. *Vista libre* de serie. |

**Pulgar derecho en el reposapulgares + clic del stick izquierdo mantenido 1 segundo** pasa al
siguiente modo, en este orden: 1 → 3 → 2 → 0 → 1. Un aviso en el visor muestra el modo nuevo, y
se recuerda el último que elegiste. Al pasar a un modo con mandos de movimiento la vista se
pone en primera persona; al pasar al modo 0, la vista se recentra como al pulsar Insert dos
veces.

### Qué hacen tus manos (modos 1, 3 y 2)

* **Atacar**: un movimiento rápido con el arma. Golpea donde el filo toca de verdad, con
  chispas contra las paredes. Recoger el brazo hacia el cuerpo no golpea.
* **Puñetazo**: con las manos vacías, el mismo movimiento rápido con el puño.
* **Bloquear**: alza el escudo delante de ti, a la altura de los hombros o más arriba, con el
  dorso de la mano izquierda hacia delante. Con un arma a dos manos, cógela con las dos manos
  y álzala.
* **Armas a dos manos**: lleva la mano izquierda al mango y mantén el agarre izquierdo para
  coger el arma también con la izquierda; suéltalo para soltarla.
* **Sacar y guardar las armas**: lleva la mano por encima del hombro, detrás de la cabeza, y
  aprieta el agarre de esa mano.
* **Lanzar** (modos 1 y 3): mantén el agarre derecho medio segundo y baja deprisa el brazo
  derecho desde la altura de la cabeza. En el modo 2, como en el juego: agarre derecho
  mantenido + A.
* **Arco** (modos 1 y 3, con el arco en la izquierda): saca una flecha de la espalda con el
  agarre derecho, aprieta el gatillo derecho con la mano derecha junto al arco para coger la
  cuerda, separa las manos para tensar y suelta el gatillo para disparar. El agarre izquierdo
  en la espalda guarda el arco.
* **Fijar a un enemigo** con el gatillo derecho. Con la mano derecha vacía lo fija sin sacar
  las armas. **Y** suelta al enemigo.
* **La comida y las pociones** se quedan en la mano: acércatelas a la boca para comer o beber.
* La vibración del juego llega a los mandos.

### Botones de los mandos de Quest

| Control | Modos 1, 3 y 2 | Modo 0 (mando de Xbox) |
|---|---|---|
| **Stick izquierdo** | Andar (nunca corre solo). Con un enemigo fijado, se mueve respecto al enemigo, como en el juego. | Andar; a fondo, correr. |
| **Clic del stick izquierdo** | Sprint, en la dirección del stick, hasta soltarlo o volver a hacer clic. | L3 (cambia la capa de la maqueta del mando, como en un mando de Xbox). |
| **Stick derecho** | Cámara. | Cámara. |
| **Clic del stick derecho** | Recentrar la vista. | R3. |
| **A** | Modo 2 y menús: A. Modos 1 y 3: A no ataca mientras tus brazos mueven los del personaje (se ataca con el arma). | A (atacar). |
| **B** | Saltar, solo con el stick izquierdo hacia delante o atrás (con un enemigo fijado, hacia cualquier lado: esquivas). Con el stick en reposo o hacia un lado, seleccionar los objetos cercanos. | B. |
| **X** | Usar. | X (usar). |
| **Y** | Soltar al enemigo fijado. | Y (darse la vuelta). |
| **Agarre izquierdo** | Sujetar un arma a dos manos con la izquierda. | LB (bloquear). |
| **Agarre derecho** | RB (lanzar). | RB (lanzar). |
| **Gatillo izquierdo / derecho** | LT / RT (RT fija al enemigo). | LT / RT. |

En los menús, A, B, Y y el agarre izquierdo son los de un mando. Con el **pulgar izquierdo en
el reposapulgares** (en todos los modos): el stick derecho es la cruceta (la cámara se queda
quieta), A es Start y B es Back. **Pulgar izquierdo en el
reposapulgares + clic del stick derecho** recentra la vista (en el modo 0 es la forma de
recentrar); mantenido 1 segundo, entra en el modo diorama.

### Ajustes

* **La calibración (Ctrl + F8)** es opcional: el agarre de serie vale para casi todo el mundo.
  Úsala solo si los brazos se notan largos o cortos. Al pulsarla se recentra la vista y empieza
  una cuenta atrás de 5 segundos (una vibración por segundo); antes de que acabe, ponte con los
  dos brazos estirados al frente, los pulgares arriba y mirando al frente: mide en ese momento.
  Se guarda; para volver al agarre de serie, borra las líneas `agarre_*` y `alcance_*` de
  `BladeVR.ini`.
* **Resistencia (Mayús + F8)**: los golpes y puñetazos con los mandos gastan la resistencia
  como el ataque básico del juego; con poca sale un ataque torpe, sin ella no sale nada.
* Un mando de verdad y el teclado y el ratón funcionan a la vez que los mandos de Quest: cuenta
  el que estés usando.
* Al cargar un nivel la vista se pone en primera persona. Los brazos siguen a los mandos solo
  en primera persona.

## Modo diorama

Pulsa **F5**, o con los mandos de Quest **pulgar izquierdo en el reposapulgares + clic del stick
derecho mantenido 1 segundo** (o **R3** con un mando en la capa de la maqueta). El nivel entero
aparece como una maqueta sobre una mesa virtual 45 cm delante de ti, algo por debajo de los
ojos, con tu personaje en el centro, y sigues jugando en ella. Mírala desde cualquier lado,
acércate, rodéala. Pulsa F5 otra vez (con los Quest: pulgar izquierdo en el reposapulgares +
clic del stick derecho) para volver a la vista VR normal.

* **El mapa entero** a la vez (1:10 al empezar, de 1:1 a 1:500): todas las salas, las paredes
  por los dos lados, los techos también, sin niebla. En la maqueta funcionan los reflejos del
  agua, las sombras de objetos, la luz de las antorchas que se mueven, las puertas, los muros
  que se derriban y el cielo. En este modo el nivel se dibuja en la GPU, así que el juego
  mantiene sus 60 fps con el mapa entero.
* **Modos de la maqueta** (LB, o clic del stick izquierdo de los mandos de movimiento): *fija*
  (se queda donde la pones), *acoplada* (por defecto: se mueve con el personaje, que queda
  siempre en el centro de la mesa) y *por la espalda* (acoplada, y además la maqueta gira
  despacio hasta ver al personaje por la espalda, como la cámara del juego).
* **Para ver por dentro:**
  * **Corte de altura**: quita todo lo que queda por encima de una altura.
  * **Corte vertical**: quita todo lo que queda entre tú y un plano vertical delante del
    personaje, en el lado que miras. Va recto, por las paredes del mapa, y se queda en su lado
    aunque gires la maqueta. En el modo *por la espalda* gira con la maqueta, así que siempre
    la ves cortada de frente; mantén **A** para cambiar a un corte recto que pasa con suavidad
    al lado nuevo del mapa cuando la maqueta gira más de la mitad.
  * **Área visible**: solo se dibuja un cuadrado del mapa alrededor del personaje.
  * **Cono de visión**: un agujero redondo entre tus ojos y el personaje que quita paredes,
    techos y pisos de arriba que tapan (apagado al empezar, tres anchos).
  * **Personajes**: siempre a la vista (por defecto) o cortados como el resto.
* **Moverse respecto a la vista** (Supr lo cambia; activo por defecto): el stick izquierdo
  hacia delante lleva al personaje hacia donde miras sobre la maqueta. El combate y los combos
  funcionan como siempre. En el modo *por la espalda* se usa el control del propio juego.
* **Jugar en la maqueta con los mandos de Quest**: pulgar derecho en el reposapulgares + clic
  del stick izquierdo mantenido 1 segundo, y los mandos manejan al personaje como un mando de
  Xbox, sin gestos, en cualquier modo de combate: stick izquierdo andar (clic: sprint), A
  ataca, B salta o selecciona, X usa, Y da la vuelta, el agarre izquierdo bloquea y el derecho
  lanza; el clic del stick derecho recoloca la maqueta. Lo mismo otra vez los devuelve a la
  maqueta.
* **Cinemáticas**: la vista sigue la cámara del juego. Solo funcionan el zoom, el cono, los
  cortes, el área y el fondo, y el zoom agranda o encoge el mundo como una maqueta. Recentrar
  (Inicio o X) vuelve a la cámara de la cinemática conservando el tamaño. Al acabar, la
  maqueta vuelve como estaba.
* Un aviso breve en el visor muestra cada cambio.

### Teclado

| Tecla | Función |
|---|---|
| **F5** | Diorama sí / no. |
| **Inicio** | Pone la maqueta delante, centrada en el personaje, a 1:15 para encontrarlo rápido (en una cinemática: vuelve a su cámara). |
| **Re Pág / Av Pág** | Zoom: maqueta más grande / más pequeña. |
| **F4** | Cono de visión: apagado → pequeño → mediano → grande. |
| **F6** | Área visible: mapa entero → 60 → 30 → 15 m. |
| **F7** | Fondo: el del juego → magenta → verde (realidad mixta). |
| **Pausa** | Personajes en los cortes: siempre a la vista ↔ se cortan. |
| **Supr** | Mover al personaje respecto a la vista sobre la maqueta sí / no. |

### Mando (distribución de Xbox, por Steam Input)

**L3** pasa todo el mando del juego a la maqueta y al revés, también fuera del modo diorama.
Mientras el mando controla la maqueta, el juego solo recibe Start y Back y el personaje se
queda quieto; pulsa L3 otra vez para jugar. En el modo de combate 0, el clic del stick
izquierdo de los mandos de Quest es este L3.

| Botón | Función |
|---|---|
| **R3** | Diorama sí / no. Mantenido: personajes en los cortes. |
| **X** | Pone la maqueta delante a 1:15 (en una cinemática: vuelve a su cámara). |
| **LB** | Modo: fija → acoplada → por la espalda. |
| **RB** | Área visible: mapa entero → 50 → 20 m (al volver al mapa entero también quita los cortes). |
| **Cruceta izquierda / derecha** | Cono de visión sí / no. |
| **Cruceta arriba / abajo** | Cono más ancho / más estrecho. |
| **A** | Corte vertical 1,5 m delante del personaje sí / no. Mantenido: en el modo *por la espalda*, corte que gira con la maqueta ↔ corte recto que cambia de lado. |
| **B** | Corte de altura 1,5 m sobre el personaje sí / no. |
| **Y** | Fondo de passthrough sí / no. |
| **Stick derecho** | Arriba / abajo: zoom. A los lados: girar la maqueta alrededor de la mesa (no en el modo *por la espalda*). |
| **RT / LT** | Zoom: maqueta más grande / más pequeña. |
| **Stick izquierdo** | Desplazar la mesa a los lados y adelante / atrás. |

### Mandos de movimiento (Quest por SteamVR)

| Control | Función |
|---|---|
| **Un agarre** | La maqueta sigue a la mano: moverla, subirla, bajarla. |
| **Los dos agarres** | Separa o junta las manos para escalar; gira la recta entre las manos para girar la maqueta. |
| **Un gatillo** | Sube o baja la mano: corte de altura. Acércala o aléjala de la maqueta: corte vertical. La primera dirección en que la muevas decide cuál. |
| **Los dos gatillos** | Separa o junta las manos: tamaño del área visible. |
| **Clic del stick izquierdo** | Modo de la maqueta. Mantenido: cono apagado → pequeño → mediano → grande. |
| **Clic del stick derecho** | Área visible. Mantenido: personajes en los cortes. |
| **Stick izquierdo** | Desplazar la mesa a los lados y adelante / atrás. |
| **Stick derecho** | A los lados: girar la maqueta. Arriba / abajo: zoom. |
| **A (derecho)** | Corte vertical 1,5 m delante del personaje sí / no. Mantenido: corte que gira con la maqueta ↔ recto (modo *por la espalda*). |
| **B (derecho)** | Corte de altura 1,5 m sobre el personaje sí / no. |
| **X (izquierdo)** | Pone la maqueta delante a 1:15. |
| **Y (izquierdo)** | Fondo de passthrough sí / no. |

### Realidad mixta con Virtual Desktop

En Virtual Desktop ve a *Streaming → VR Passthrough → Environment* y pon una clave de color
(magenta o verde, con la similitud, el suavizado y la opacidad a tu gusto). En el juego pulsa
**F7** (o **Y**) para pintar el fondo de ese color: la maqueta aparece sobre tu habitación.
Link y Air Link no tienen passthrough para juegos de SteamVR.

## Cómo funciona

**Carga.** Windows busca las DLL en la carpeta del ejecutable antes que en System32, así que
el juego carga nuestro `dxgi.dll`. Este reenvía todas las funciones al DXGI real del sistema
(cargado por ruta absoluta) y en su `DllMain` carga `BladeVR.dll`. Además intercepta
`CreateDXGIFactory*` para pasar el factory al mod, que engancha
`IDXGIFactory::CreateSwapChain` y así conoce el device D3D11 del juego antes del primer
`Present`.

**Estéreo** (`SceneCullingRootHook.cpp`, `StereoHook.cpp`). El motor transforma los vértices
en la CPU y recorta el nivel contra los portales desde su cámara; la GPU solo recibe geometría
en espacio de cámara más una única matriz de proyección en el constant buffer de VS slot 0
(`ProjectionHook.cpp` la captura). El render del mundo del motor (`B_Map::Render`) se llama
dos veces por fotograma, con la cámara desplazada media separación entre ojos a la izquierda
y después a la derecha. Todo lo que el motor guarda por fotograma (recorte por portales,
transformación de vértices, poses de los personajes, sombras, luces) va ligado a un contador
de fotograma que esa misma función incrementa, así que la segunda pasada se rehace desde el
otro ojo. El motor agrupa sus draws en lotes y los envía a bgfx al final del fotograma; los
lotes de cada pasada se mandan a copias de las vistas de bgfx del motor cuyo viewport es la
mitad izquierda o derecha del render target. `StereoHook.cpp` reconoce esos draws por su
viewport y los dibuja una vez con el frustum asimétrico de su ojo. Las luces que parpadean
(antorchas) sortean su intensidad solo en la pasada izquierda, para que los dos ojos vean la
misma llama. Lo que se dibuje en 3D fuera del render del mundo se emite dos veces, una por
ojo, con un desplazamiento de ojo off-axis. El backbuffer queda en formato lado a lado y se
entrega a SteamVR con bounds `0..0.5` / `0.5..1`. La interfaz se dibuja en una pantalla
virtual a 1,8 m.

**Seguimiento de cabeza** (`HeadTrackHook.cpp`). La matriz de vista global del motor
(`fromWorld`, 4x4 doubles) la escribe una función una vez por fotograma. Se engancha esa
función y, tras la última llamada de cada fotograma, se reescribe la matriz entera con la
pose del visor: la rotación completa del casco (con la guiñada anclada a la de la cámara del
juego) y la posición del casco sumada a la de la cámara, convertida a unidades del juego y
recortada con el trazado de rayos del propio motor para no atravesar paredes ni objetos. El
bloque de cámara del culling se sincroniza con la vista reescrita
(`SceneCullingRootHook.cpp`) para que sombras y entidades se vean en todas direcciones. La
pose usada para dibujar se adjunta al `Submit` (`Submit_TextureWithPose`) para que el
compositor reproyecte correctamente. En el modo *camino hacia donde miro* el personaje se
gira con movimiento de ratón que solo ve el juego: un mensaje de entrada *raw input* enviado a
su ventana, cuyo contenido entrega `GetRawInputData`, enganchada en la tabla de importaciones
del juego. Parado, se le gira poniendo directamente su ángulo, para que no dé pasos.

**Mandos de movimiento** (`VrInput.cpp`, `VrPadHook.cpp`, `CombatVRHook.cpp`, `CombatVRIk.h`).
Los mandos se leen con la entrada por acciones de SteamVR (el mod escribe su propio manifiesto
de acciones y los enlaces por defecto, que dan también los reposapulgares). El juego lee los
mandos por Steam Input; se cambian siete entradas de esa interfaz para que el juego vea un mando
de Xbox más, alimentado por los mandos de Quest, y lo trate exactamente como a uno de verdad.
Los brazos se colocan con cinemática inversa después de que el motor evalúe el esqueleto en
cada fotograma, con lo que lleve la mano enganchado a ella. Los tajos se miden con el mando
real y encienden la detección de golpes de la propia arma mientras va deprisa, así que es el
propio motor el que decide dónde golpea; el gasto de resistencia sale de la fórmula del juego.
El bloqueo con el escudo o con un arma a dos manos pasa por la prueba de bloqueo del motor, y
la comida, las pociones y el arco usan los scripts de Python del propio juego.

**Modo diorama** (`DioramaHook.cpp`, `DioramaGpuLevel.cpp`). La cámara que escribe el juego se
sustituye por la pose de la cabeza llevada a la mesa (ancla + giro · (cabeza − mesa) ·
escala), y la separación entre ojos se multiplica por la escala, que es lo que hace que el
mundo se vea como una maqueta pequeña. El mundo se dibuja una vez desde el centro de la
cabeza y cada draw se emite dos veces con un desplazamiento de ojo off-axis; los reflejos del
agua, que el motor recorta con conos que pasan por el ojo, se dibujan una vez por ojo. Se
apagan la visibilidad por portales, el descarte de caras traseras y la niebla para que se
dibuje el mapa entero. Con la transformación por CPU del motor el mapa entero iba demasiado
lento, así que al entrar en un nivel se capturan una vez las caras que manda el motor y las
dibuja la GPU con la iluminación del propio motor; las puertas, los muros que se derriban y
los líquidos que corren, que cambian durante la partida, los sigue dibujando el motor. Los
cortes y el cono se aplican por píxel.

**Direcciones del juego.** Los offsets de `Blade.exe` (constantes `k...` al principio de los
ficheros de enganches) corresponden a la versión de Steam del remaster; si el juego se
actualiza habrá que revisarlos.

## Compilar

Requisitos: Visual Studio 2019/2022 con "Desarrollo para el escritorio con C++", CMake 3.16+
y git (MinHook se descarga en el configure).

Desde una consola con CMake en el PATH (por ejemplo la "x64 Native Tools Command Prompt
for VS"), en la carpeta del repositorio:

```
cmake -B build -A x64
cmake --build build --config Release
```

Después, copia los tres ficheros de `output\Release\` (`dxgi.dll`, `BladeVR.dll` y
`openvr_api.dll`) junto al ejecutable del juego, en `Blade of Darkness\bin\bin\`.

## Log de depuración

El mod no escribe ningún log por defecto. Para obtenerlo, crea un fichero vacío llamado
`BladeVR_debug.txt` junto a `Blade.exe`; el mod escribirá entonces
`bin\bin\BladeVR_logs\BladeVR_<pid>_<fecha>.log` en cada ejecución: los módulos que instala,
el enlace con SteamVR, los mandos, los frustums de cada ojo, los cambios de modo de combate y
un breve resumen cada minuto (fotogramas por segundo, peor fotograma, si el mundo se está
dibujando por ojo, lo que se hizo con los mandos). Adjúntalo al informar de un problema.

## Estructura

```
hookdll/                 BladeVR.dll
  dllmain.cpp            punto de entrada, orden de instalación de los módulos
  Dx11Hook.*             Present/ResizeBuffers/CreateSwapChain, entrega a SteamVR
  ProjectionHook.*       captura de la matriz de proyección (VS slot 0)
  StereoHook.*           estéreo lado a lado, pantalla plana, interfaz anclada, teclas
  FovHook.*              campo de visión mínimo (selección de personaje, cinemáticas)
  OpenVRHook.*           SteamVR: init, poses, frustums, Submit, avisos en el visor
  VrInput.*              mandos de movimiento por las acciones de SteamVR (manifiesto, enlaces)
  VrPadHook.*            los mandos de Quest como mando de Steam Input, gestos
  CombatVRHook.*         combate con mandos: brazos, tajos, escudo, arco, comida, modos
  CombatVRIk.h           cinemática inversa de los brazos
  BladeVRIni.*           BladeVR.ini (los ajustes que se recuerdan)
  HeadTrackHook.*        seguimiento de cabeza 6DOF y colisión
  SceneCullingRootHook.* mundo dibujado una vez desde cada ojo; culling sincronizado
  DioramaHook.*          modo diorama: cámara de la maqueta, cortes, cono, controles, cinemáticas
  DioramaGpuLevel.*      modo diorama: nivel capturado y dibujado en la GPU
  FromWorldLocator.*     localiza la matriz de vista global en memoria
  HookLogger.*           log opcional
proxydll/                dxgi.dll proxy (ProxyMain.cpp + thunks MASM)
ThirdParty/openvr/       openvr.h, openvr_api.lib, openvr_api.dll (OpenVR SDK)
```

## Limitaciones conocidas

* El resplandor de las antorchas y demás luces no se atenúa hacia los bordes de la vista como
  sí hace en plano. El juego lo atenúa respecto a su propio encuadre, que en VR es mucho más
  ancho de lo que ve cada ojo, así que una antorcha mirada de reojo conserva el halo completo
  en lugar de reducirse.
* El recuadro con los datos del personaje, en la pantalla de selección, parece desplazarse
  ligeramente al girar la cabeza.
* Las armas y el escudo atraviesan paredes y objetos como en el juego original, y en tercera
  persona la cámara puede meterse en el personaje.
* El espejo del monitor no muestra las pantallas ancladas delante de ti (la lista de combos
  de F1 y la ficha de personaje): esas van directas al overlay de SteamVR y no pasan por la
  imagen del juego.
* En los modos de combate 1, 3 y 2 hay tres acciones del juego sin botón: bloquear con un arma
  de una mano sin escudo, lanzar lo que llevas en la mano izquierda (salvo alzando el escudo)
  y saltar en el sitio. Para ellas, usa el modo 0, un mando o el teclado.
* Los brazos siguen a los mandos solo en primera persona; en tercera persona se ven las
  animaciones del juego.
* Modo diorama: los efectos muy pequeños (las ondas de las pisadas en el agua) pueden quedar
  por debajo de un píxel con la maqueta pequeña, y los efectos de partículas (como las
  cascadas) no se reflejan en el agua de la maqueta.

## Historial de versiones

* **2.0** — Combate con mandos de movimiento: brazos que siguen a tus manos, ataques con la
  detección de golpes de la propia arma y el ritmo y el gasto de resistencia del juego, bloqueo
  con el escudo y con armas a dos manos, arco VR con flechas sacadas de la espalda, puñetazos,
  lanzar, sacar y guardar las armas con un gesto, la comida y las pociones a la boca. Los
  mandos de Quest como mando completo, que se puede usar a la vez que un mando de verdad y el
  teclado. Cuatro modos de combate (mandos de movimiento, manos flotantes, mandos con los
  combos del juego, VR básico), que se recuerdan en `BladeVR.ini`. *Camino hacia donde miro*
  activo de serie y sin dar pasos al girar parado. Modo diorama: se entra y se sale con los
  mandos de Quest, se juega en la maqueta con ellos, y A / B hacen ahora los cortes vertical y
  de altura.
* **1.6** — Modo diorama: el nivel entero como maqueta sobre una mesa, con cortes de altura y
  vertical, área visible, cono de visión, tres modos de maqueta, movimiento respecto a la
  vista, cinemáticas, fondo de realidad mixta para Virtual Desktop, mandos de movimiento y
  capa de maqueta en el mando, y avisos en el visor.
* **1.2** — El motor dibuja ahora el mundo desde cada ojo. Se acabaron las franjas negras en
  los bordes de puertas y columnas; las sombras, los reflejos del agua y la luz de las
  antorchas son correctos en los dos ojos. *Camino hacia donde miro* (Supr) gira ahora al
  personaje sin mover el ratón de Windows. Separación entre ojos por defecto: 58.
* **1.0** — Primera versión: estéreo 3D, seguimiento de cabeza 6DOF, pantalla plana para
  vídeos y menús, campo de visión mínimo.

## Licencia

BladeVR se publica bajo la [licencia MIT](LICENSE), copyright (c) 2026 Efraim27.
Puedes usarlo, modificarlo y redistribuirlo libremente siempre que se conserven el
aviso de copyright y el texto de la licencia, es decir, que se acredite al autor.

## Terceros

* [MinHook](https://github.com/TsudaKageyu/minhook) (licencia BSD de 2 cláusulas).
* [OpenVR SDK](https://github.com/ValveSoftware/openvr) (licencia BSD de 3 cláusulas).
