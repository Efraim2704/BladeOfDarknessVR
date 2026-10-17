#pragma once

namespace BladeVR {

// ---------------------------------------------------------------------
// Combate con los mandos de movimiento (version 2.0).
// Analisis del motor en el proyecto: claude/combate_fase0.md.
//
// Teclas:
//   Ctrl+F8  : calibrar el agarre y el alcance: recentra la vista y, a los
//              5 s (un toque de vibracion por segundo), mide con los brazos
//              estirados al frente, los pulgares arriba y mirando al frente.
//              Se guarda en BladeVR.ini (ronda 55).
//   Mayus+F8 : los golpes con los mandos gastan la resistencia si / no
//              (ronda 60; se guarda en BladeVR.ini).
// Los brazos con los mandos se quitan con el modo de combate 0 (VR basico).
//
// Ronda 2: las MANOS del personaje siguen a los mandos (cinematica inversa
// de los dos brazos: hombro, codo, antebrazo, muneca y mano), solo en
// primera persona. Lo que lleva en la mano (arma, escudo, antorcha) va
// enganchado a la mano, asi que la sigue sin separarse ni estirarse.
// Vista: al cargar un nivel, al salir del diorama con los mandos y al pasar a
// un modo con mandos de movimiento (ronda 74) se pone la primera persona
// durante 3 s; despues las teclas del juego la cambian.
//
// Ronda 5: el cuerpo sigue al visor (la pelvis se desplaza bajo la cabeza y
// el pecho gira hacia donde se mira, sin mover la camara); los brazos se
// alargan hasta el alcance medido al calibrar; la torsion de la muneca es
// continua (sin saltos del escudo). Ataque basico por gesto: un tajo rapido
// con el arma la activa (golpea, chispas, la IA se defiende) sin animacion,
// sin estela y sin gastar resistencia.
//
// Ronda 6: el tajo se mide con el mando real (espacio de seguimiento), asi
// que acercarse a una pared o andar no lo enciende. Con escudo en el
// antebrazo la mano queda fija en su correa (la muneca guarda con el
// antebrazo la relacion de la animacion) y levantar el brazo izquierdo
// delante bloquea (VrPadHook). Si el personaje sigue andando con el joystick
// en reposo se sueltan las teclas del joystick.
//
// Ronda 7: Supr compara la vista con hacia donde mira la entidad (no la
// camara), no gira con un enemigo fijado y, quieto, solo a partir de 20
// grados (CombatVRGetPlayerFacing). El escudo tambien bloqueaba de frente
// (quitado en la ronda 14).
//
// Ronda 8: con el joystick en reposo y el personaje andando o corriendo se le
// para (Gof/Gob/Run = 0 por Python, como el juego al quedarse sin energia).
// La comida recogida se queda en la mano derecha y se come acercando el
// mando derecho a la boca (Python sobre pocimac.UsePotion3). El giro del
// antebrazo se mide con la descomposicion giro-balanceo (CombatVRIk.h).
//
// Ronda 9: el antebrazo se lleva la mitad del giro de la muneca (0,7 con
// escudo, como mucho 90 grados). Andar siempre y sprint con el clic del
// joystick izquierdo; el diorama pasa a pulgar derecho en reposo + clic
// izquierdo (VrPadHook).
//
// Ronda 10: el giro del antebrazo izquierdo se mide desde la pose de reposo
// (la animacion de guardia lo giraba). Sin sprint, Run = 0 siempre y tras
// escalar o caer con el joystick en reposo se para en cuanto vuelve a andar.
// Con un enemigo fijado el pecho no gira hacia la vista y Supr lo da por
// fijado 1 s mas; Y lo suelta y B con el joystick en reposo solo selecciona
// objetos (VrPadHook).
//
// Ronda 11: los dos brazos se alargan hasta el mayor alcance calibrado (el
// izquierdo medido salia corto y ese brazo no se alargaba). Supr no gira al
// personaje cuando el juego convertiria el giro en un paso lateral (al
// bloquear: se deslizaba solo). Con un enemigo fijado la camara del juego se
// estabiliza (HeadTrackHook).
//
// Ronda 12: con un enemigo fijado la camara se clava detras de su objetivo
// segun hacia donde mira el personaje (el temblor era su giro alrededor del
// objetivo, hasta 0,4 m). La parada con el joystick en reposo actua antes
// (0,25 s, desde 0,3 m/s) y tambien con Gof/Gob/Tr/Tl puestos fuera de una
// accion de andar; Supr sigue en pausa 0,6 s despues de bloquear. Zona muerta
// del joystick izquierdo 0,40 (VrPadHook).
//
// Ronda 13: quieto, Supr gira al personaje poniendo su angulo (Angle) poco a
// poco, sin peticiones de giro: estas le hacian dar pasos al girar en el
// sitio. Con un enemigo fijado la camara se ancla a la posicion del jugador
// (el que temblaba era el objetivo de la camara del juego, la cabeza).
//
// Ronda 14: el bloqueo por gesto solo con el escudo alzado delante (hombros o
// mas arriba, de cara hacia delante); los ejes del joystick con histeresis
// alrededor del umbral de las teclas del juego (sin dobles toques que lo ponen
// a correr) (VrPadHook). La camara fijada sigue al jugador suavizado (media de
// 4 fotogramas: el juego lo mueve a saltos) y el seguro de Supr ya no se queda
// midiendo con la camara del juego (HeadTrackHook).
//
// Ronda 15: la parada al soltar el joystick mira lo que se manda desde los
// Quest (no lo que lee el juego) y actua a los 0,12 s. VrPadHook manda 0 sin
// ninguna tecla del joystick pulsada, el sprint pide inclinarlo mas de la
// mitad (una inclinacion pequena se ponia a fondo) y el clic de correr ya no
// se pierde si el joystick se mueve en los 0,6 s siguientes.
//
// Ronda 16: la causa de que ande solo. El juego solo procesa el mando si fue
// lo ultimo usado, y los giros de Supr son movimientos de raton: al soltar el
// joystick mientras Supr gira, el mando no se procesa y la tecla de andar
// queda pulsada. Ahora se procesa igualmente cuando tiene algo por entregar
// (una tecla del joystick o de un gatillo que cambia, o botones en cola).
//
// Ronda 17: el joystick izquierdo solo cuenta con el pulgar encima (toque del
// joystick, VrPadHook): con un enemigo fijado la deriva del mando (hasta 0,6
// en x) era un paso lateral y daba vueltas a su alrededor. El diorama pasa a
// pulgar izquierdo en reposo + clic del joystick derecho.
//
// Ronda 18: con un enemigo fijado el pecho vuelve a girar hacia la vista: con
// la guardia de lado de la animacion el hombro izquierdo quedaba por delante
// de la camara y el brazo del escudo doblado.
//
// Ronda 19: con el escudo, el codo va hacia donde lo pone la orientacion del
// mando (la muneca va fija al antebrazo) y el antebrazo se lleva todo el giro
// (CombatVRIk.h, shieldPole): el escudo sigue al mando tambien junto al
// pecho. Al fijar, la estabilizacion de la camara entra en 0,06 s.
//
// Ronda 20: al morir y cargar partida el juego se cerraba. Al cambiar de
// jugador se marcaban los nodos del esqueleto viejo leyendo por el jugador
// viejo, ya liberado, y los -1,0 caian sobre el tipo int de Python y True
// (Blade.exe+0x9dfd90). Ahora solo con el jugador de la partida, y los
// evaluadores comprueban que la tabla de nodos sigue siendo la del esqueleto.
//
// Ronda 21: las pociones, como la comida, se quedan en la mano (derecha, o
// izquierda si se usan en combate con un arma) y se beben acercando ese mando
// a la boca (pocimac.UsePotion2 y PickPotion envueltas). Con un arma a dos
// manos (por su tipo, LIB/Reference.py) la mano izquierda va al arma con la
// relacion entre las manos de la animacion, sobre la mano derecha resuelta.
//
// Ronda 22: el brazo izquierdo en el arma a dos manos temblaba (objetivo cerca
// del alcance: 1 mm de ruido movia el codo hasta 19 grados). Alcance suave y
// el codo de la animacion para ese brazo (CombatVRIk.h, prueba T15).
//
// Ronda 23: seguia temblando. Relacion entre las manos filtrada, giro de la
// mano derecha con un filtro "1 euro" para llevar la izquierda, la mano se
// desliza por el mango si la muneca no llega, codo suavizado y giro del
// antebrazo desde la animacion (pruebas T16-T18). Modos de combate con el
// pulgar derecho en el reposapulgares + clic del joystick izquierdo: 0 VR
// basico, 1 mandos de movimiento (por defecto; A no ataca), 2 mandos + combos
// del juego con A.
//
// Ronda 24: el medidor de la ronda 23 dio que lo que temblaba era el cuerpo
// (hombro 20-40 mm por fotograma con el pecho girado 40-70 grados hacia la
// vista; con el pecho casi sin girar, 3-6 mm), no el agarre (relacion de la
// animacion quieta). Con el arma a dos manos el pecho se queda como lo pone
// el juego; la mano izquierda va fija al agarre del juego de cada arma (sin
// deslizarse); adelante de la vista sin saltos al mirar abajo (pecho y Supr).
//
// Ronda 25: la causa de todo el temblor (rondas 21-24) era nuestra: el peso
// de la mano izquierda en el arma (ronda 21) y el del pecho en los combos
// (ronda 23) no se quedaban en 1 al llegar, alternaban 1 / 0,93 en cada
// fotograma (mano ~3 cm, pecho ~3 grados). Fundidos con ik::StepToward; fuera
// el filtro "1 euro" de la mano derecha (dejaba la izquierda atras del arma).
//
// Ronda 26: confirmado, sin temblor. El baston de la amazona no quedaba en la
// mano izquierda: el agarre fijado arrastraba la animacion de sacar el arma.
// Ahora es la media de la de reposo sola (sin su primer medio segundo) y se
// comprueba contra el eje del arma (GripShaftCheck); tabla de agarres por
// personaje. Los ojos van 10 cm por delante de la camara del juego
// (HeadTrackHook; Ctrl + Re Pag / Av Pag).
//
// Ronda 27: el arma a dos manos empieza solo en la mano derecha; la izquierda
// la coge al acercar el mando a su agarre (15 cm) y la suelta al separarlo
// (50 cm en la ronda 35; desde la 36, al soltar el agarre izquierdo), con un toque en el mando; el giro del antebrazo y la postura del
// pecho pasan de una forma a otra poco a poco. La altura de la camara va
// suavizada (escaleras: el personaje caia cada escalon de golpe) y el cuerpo
// la sigue.
//
// Ronda 28: altura de la camara con 0,20 s y sin salir del sector de la
// camara del juego; el desplazamiento del visor vuelve con un muelle al
// soltarse un recorte (HeadTrackHook).
//
// Ronda 29: la mano izquierda suelta el arma a 40 cm. HeadTrackHook ya no
// para los ojos, la altura ni el desplazamiento del visor en el borde del
// sector (tirones al cruzar puertas y escalones), solo al salir del nivel;
// en cinematicas y seleccion de personaje la vista sigue a su camara.
//
// Ronda 30 (pedido del usuario): soltar el arma a dos manos a 45 cm. Gesto de
// la espalda: una mano por encima del hombro y detras de la cabeza y su
// agarre guardan las armas o, con las manos libres, las sacan
// (Actions.StdToggleWeapons, como la cruceta arriba; VrPadHook).
// Ronda 31: los punetazos con las manos libres van como los tajos: un golpe
// rapido de un mando enciende las armas naturales del personaje (mensajes 7 /
// 8 a la persona) sin animacion ni combos; en el modo 1, A sigue tapada (con
// las armas a la espalda el ataque del juego las sacaba).
//
// Ronda 32 (pedido del usuario): en el modo 1, con el agarre derecho (lanzar)
// apretado lo bastante, bajar el brazo derecho deprisa desde la altura de la
// cabeza lanza (VrPadHook manda A, como el mando: mantener lanzar + atacar).
// Fijar enemigo con la mano derecha vacia no saca las armas. Al morir, o sin
// fotogramas de partida (camara de la muerte, cinematicas), brazos y cuerpo
// vuelven a la animacion al momento.
//
// Ronda 33 (pedido del usuario): escudo VR. Con el escudo en el antebrazo
// izquierdo siguiendo al mando y la mano izquierda a la altura del pecho o
// mas arriba, el jugador "esta bloqueando" (+0xb2240) sin el gesto ni LB, y
// la prueba del escudo (+0xbded0) es la del escudo real: para lo que pasa
// cerca de el o por delante a su altura. Fijar con la mano derecha vacia ya
// no deja pasar el gatillo al juego ni un fotograma (VrPadHook).
//
// Ronda 34 (pedido del usuario): sin aviso en el visor al cargar. Con el escudo
// lejos (delante de la cara) el hombro izquierdo se adelanta hasta 10 cm hacia
// la mano (el codo dobla y el escudo sigue al mando). Recoger el brazo hacia
// el cuerpo no golpea (ni con el arma ni con los punos), y tras un golpe que
// toca el arma no vuelve a hacer dano hasta que pasa lo que dura el ataque
// basico del juego con ella (cadencia del juego).
// Ronda 35 (pedido del usuario): el escudo solo para con la mano izquierda a
// 0,30 m o menos bajo los ojos (antes 0,45, el pecho). Con el escudo lejos el
// hombro va hacia el codo que pide la orientacion del mando (y si no basta,
// alargamiento y la muneca se acerca): el codo dobla como el del jugador, el
// antebrazo y el escudo con la orientacion del mando y sin temblor. La
// cadencia es 0,85 de la animacion del ataque basico (lo que tarda el juego
// en repetirlo) y el personaje no se pierde con animaciones Rlx_*. La mano
// izquierda suelta el arma a dos manos a 50 cm.
// Ronda 36 (pedido del usuario): el arma a dos manos la sujeta la izquierda
// mientras se mantiene el agarre izquierdo (la coge con el agarre a menos de
// 20 cm de su agarre; sin distancia para soltarla; ese agarre no va al juego).
// Con el escudo el antebrazo no se alarga (el escudo se volvia ovalado): lo
// pone el brazo. La altura del cuerpo bajo el visor sale de la persona
// suavizada, no de la camara del juego (sus saltos hacian temblar todo el
// personaje).
// Ronda 37: con el arma a dos manos cogida, si su agarre queda fuera del
// alcance del brazo izquierdo, el arma se acerca al hombro izquierdo (la mano
// izquierda no se sale). Guardar las armas quita antes un IntermediateSelect
// pendiente del fijar del juego (bloqueaba StdToggleWeapons).
// Ronda 38: la parada del mod y Supr no actuan durante las acciones de script
// del juego (palancas, coger, escenas).
// Ronda 39: la fisica del juego (contacto con el suelo, "paso demasiado bajo",
// suelo bajo la camara) lee los pies del esqueleto; el cuerpo bajo el visor
// los movia (en el elevador de Ragnar, 40 cm por encima del suelo: caida mortal).
// Durante esas consultas la raiz y las piernas se calculan sin el desplazamiento.
// Ronda 40: al soltar el joystick se para ya (evento StopTest del juego en
// cuanto quita las banderas de movimiento, sin esperar al de la animacion); en
// plataformas y caidas los ojos toman la altura del personaje (HeadTrackHook),
// como el cuerpo, y el brazo del escudo no se retuerce.
// Ronda 41: esa parada pasa a Rlx sin pie apoyado (la del juego clava un pie y
// el cuerpo avanzaba o retrocedia hasta media zancada), y la animacion del
// juego (transiciones y pie apoyado) lee la raiz y los pies sin el cuerpo bajo
// el visor, como la fisica en la ronda 39.
// Ronda 42: la cadencia se sabe siempre (personaje por el prefijo o el sufijo de
// la animacion, o por el modelo; si no, 0,85 s por defecto: en la prueba del
// 12-10 no se supo y los tajos no tuvieron limite); subir el brazo hacia uno
// mismo no golpea.
// Ronda 43: con arma, la cadencia es la animacion del ataque basico + 0,04 s (el
// juego no empieza el siguiente hasta acabarla: 0,99 s con 0,95), o lo que se
// mida pulsando A (sin los ataques con la resistencia al minimo).
// Rondas 44-45 (HeadTrackHook, OpenVRHook): medidas de la altura de los ojos y
// de los mandos sobre el suelo (para la ronda 46).
// Ronda 46 (HeadTrackHook, StereoHook): altura anadida a los ojos (Mayus + Re Pag /
// Av Pag, 5 cm, guardada en BladeVR_altura.txt) y la separacion entre ojos en el
// visor al cambiarla.
// Ronda 47: modo de combate 3, sin cuerpo: como el 1, pero el jugador se dibuja
// solo con antebrazos y manos (el dibujo de la persona, +0xb87c0, con los demas
// huesos encogidos a un punto) y los ojos 45 cm mas altos (ajustable); el escudo
// cuenta a la altura del cuerpo del juego. (El modo se guardaba en
// BladeVR_modo.txt; desde la ronda 53 se empieza siempre en el 1.)
// Ronda 48: el modo 3 son manos flotantes, sin cinematica inversa: la mano va al
// mando y codo, antebrazo y muneca pegados a ella como en la pose de reposo (con
// el escudo, muneca y mano con la relacion de la animacion, como en el 1), sin
// el limite del brazo del personaje. El escudo cuenta donde se ve y se alza
// midiendo desde los ojos del personaje. Orden de los modos: 1 -> 3 -> 2 -> 0.
// Ronda 49: primera persona con margen para salir (1,2 m; los parpadeos de 1-3
// fotogramas al girar deprisa fijado soltaban los brazos y en el modo 3 se veia
// el cuerpo); VrPadHook gira el joystick hacia la vista con un enemigo fijado
// (quitado en la ronda 72: fijado, el joystick va respecto al enemigo, como en
// el juego); altura de los ojos de serie 30 cm.
// Ronda 50: arco VR en los modos 1 y 3 (arco en la izquierda, flecha en la
// derecha): gatillo derecho junto al arco = agarrar la cuerda, separar las manos
// = tensar (la mano derecha gira para que la flecha apunte al arco), soltar el
// gatillo = disparar de la mano derecha hacia el arco (Python del juego: Fly,
// MESSAGE_START_WEAPON, la siguiente flecha del carcaj). StereoHook: un panel
// con el juego en marcha (mensaje de la partida) va con el HUD, no anclado.
// Ronda 51: la flecha ya no se recarga sola: agarre derecho en la espalda (el
// izquierdo sigue guardando las armas). StereoHook: el panel de la partida ya
// no se desvia a la pantalla anclada (se quedaba fijo "pulse F1").
// Ronda 52: al sacar el arco (VR) la flecha que pone el juego vuelve al carcaj
// (Actions.SheatheArrow): hay que cogerla de la espalda. StereoHook: con el
// juego parado (F1 abierto sobre "pulse F1") el panel vuelve a ir anclado.
// Ronda 53: modo 1 siempre al arrancar (BladeVR_modo.txt ya no se usa) y el
// cambio de modo (pulgar derecho en reposo + clic izquierdo) se mantiene 1 s,
// como el diorama; un clic corto es el sprint.
// Ronda 54: BladeVR.ini (BladeVRIni.cpp) guarda el ultimo modo de combate, si se
// salio en el diorama (se vuelve a entrar al empezar a jugar) y la altura de los
// ojos del modo de manos flotantes (de serie 0, la del personaje).
// Ronda 55: agarre y alcance de serie para cualquiera; Ctrl+F8 los guarda en
// BladeVR.ini. Ronda 56: cada mando con su agarre de serie (no simetricos) y,
// en los modos de mandos de movimiento, el agarre izquierdo ya no bloquea.
// Ronda 57: el arma a dos manos cogida con las dos y alzada como el escudo
// bloquea (+0xb2240 = 1; la prueba del bloqueo, la del juego con el arma). El
// hombro izquierdo (clavicula y brazo) va en su pose de reposo sobre el pecho,
// sin la animacion (kRestShoulder), salvo con el arma a dos manos cogida.
// Ronda 58: tampoco con un arma a dos manos en la derecha (cogida o no: el
// agarre se mide con la mano izquierda de la animacion) ni con el arco en la
// izquierda (su pose adelanta el hombro): ahi el brazo sale del de la animacion.
// Ronda 59: los tajos y punetazos con el mando gastan la resistencia como el
// ataque basico del juego (Damage.CalculateFatigue sin animacion, kPyEnergy):
// ataque torpe con poca, sin ataque sin ella o si el arma pide tanto como el
// maximo del nivel (la Eclipse al principio). El valor sale de Python por
// PyRun_String (PyEvalNumbers).
// Ronda 60: Mayus + F8 pone o quita esa resistencia (BladeVR.ini "resistencia").
// El agarre fijo del arma a dos manos solo se mide con ella en la mano derecha
// (al cargar se media con la mano vacia y la izquierda no cogia el baston).
// ---------------------------------------------------------------------
bool InstallCombatVRHooks();

// Hilo del juego, una vez por fotograma de partida (lo llama el hook del
// escritor de fromWorld con la camara de partida recien escrita).
void CombatVROnGameFrame();

// Desde HookedPresent: teclas Ctrl+F8 / Mayus+F8, avisos en el visor y los
// mandos de Quest para el juego (VrPadOnPresent).
void CombatVROnPresent();

// Cualquier hilo: poner la vista en primera persona en cuanto se pueda
// (durante 3 s, como al cargar). Lo pide VrPadHook al salir del diorama y
// CombatVRCycleCombatMode al pasar a un modo con mandos de movimiento (ronda 74).
void CombatVRRequestFirstPerson();

// Cualquier hilo: el brazo izquierdo sigue al mando y lleva escudo en el
// antebrazo (VrPadHook puede bloquear con el gesto de levantarlo).
bool CombatVRShieldGestureAllowed();

// Hilo del juego (lo usa Supr en HeadTrackHook al escribir la camara): hacia
// donde mira el jugador (la entidad, que es hacia donde anda; guinada como
// la camara: atan2(x, z)) y si Supr no debe girarlo: enemigo fijado (con 1 s
// de margen) o un estado en que el juego haria pasos laterales con el giro
// (bloqueando). false fuera de partida.
bool CombatVRGetPlayerFacing(double* yaw, bool* enemyLocked);

// Hilo del juego, desde el proceso del mando del juego (HeadTrackHook, tras
// el original): con el joystick izquierdo en reposo y el personaje andando,
// suelta las teclas del joystick (adelante, atras y a los lados).
void CombatVROnGamepadInput(long long dev, float t);

// Ronda 67, hilo del juego, desde el proceso del mando del juego (HeadTrackHook,
// antes del original): true = no procesar este mando en este fotograma (esta
// quieto y otro mando se esta moviendo; con dos mandos, el quieto pisaba los
// ejes del que se mueve).
bool CombatVRSkipIdlePad(long long dev);

// Cualquier hilo: hay partida en marcha (fotograma de juego reciente, sin el
// menu abierto, sin diorama y con el jugador vivo). Fuera de ella Y y B van
// al juego como botones normales (menus).
bool CombatVRInGameplay();

// Cualquier hilo: el jugador tiene un enemigo fijado (con 1 s de margen).
bool CombatVREnemyLocked();

// Cualquier hilo: piden al hilo del juego soltar al enemigo fijado (Y) o
// seleccionar el siguiente objeto cercano sin saltar (B con el joystick en
// reposo). Se hacen en el siguiente fotograma de partida, por Python.
void CombatVRRequestUnlockEnemy();
void CombatVRRequestSelectObject();

// Hilo del juego (Supr, ronda 13): quieto (moving = false), sin enemigo fijado,
// estando de pie o bloqueando y con la relacion Angle/guinada ya medida, Supr
// gira al personaje poniendo su angulo (sin pasos) en vez de pedir giros.
bool CombatVRDirectTurnAvailable(bool moving);
// Un paso del giro directo hacia viewYaw (convenio de CombatVRGetPlayerFacing),
// como mucho 360 grados/s.
void CombatVRDirectTurnStep(double viewYaw);

// Hilo del juego: posicion del jugador como la usa el juego (unidades).
bool CombatVRGetPlayerPosition(double out[3]);

// Ronda 23: modo de combate. 0 = VR basico (sin mandos de movimiento: A ataca
// y hace combos, comida como en el juego), 1 = mandos de movimiento (por
// defecto; A no se manda al juego en partida), 2 = mandos + combos del juego
// con A (durante su animacion de ataque, brazos y pecho de la animacion y
// Supr sin girar al personaje). Cualquier hilo.
// Ronda 47: modo 3 = como el 1, pero sin cuerpo (solo antebrazos y manos) y con
// los ojos mas altos (HeadTrackEyeLiftM). CombatVRGetCombatMode devuelve el
// comportamiento (el 3 como el 1). Ronda 53: de serie el 1 y para cambiarlo hay
// que mantener el clic izquierdo 1 s. Ronda 54: el ultimo elegido se guarda en
// BladeVR.ini (modo_combate).
// Ronda 48: el 3 es el de las manos flotantes (antebrazos, manos y armas en los
// mandos, sin cinematica inversa).
int CombatVRGetCombatMode();
void CombatVRCycleCombatMode();        // ronda 48: 1 -> 3 -> 2 -> 0 -> 1, con aviso en el visor
// Ronda 47: modo sin cuerpo activo (modo 3, en partida y en primera persona).
bool CombatVRBodilessActive();
// Modo 2: se acaba de pulsar A (los brazos sueltan antes de que empiece la animacion).
void CombatVRNotifyAttackButton();
// Modo 2: hay un ataque del juego en curso (Supr no gira y la vista no sigue al personaje).
bool CombatVRComboHold();
// El ataque basico por gesto esta listo (primera persona, brazo derecho con el
// mando, un arma en la mano; ronda 31: o las manos libres, con los punos): en
// el modo 1, A no se manda al juego.
bool CombatVRGestureAttackReady();

// Ronda 50, cualquier hilo: arco VR listo (arco en la izquierda, flecha en la
// derecha, modo 1 o 3) con la mano derecha junto al arco, o tensandolo: el
// gatillo derecho es la cuerda (VrPadHook no lo manda al juego).
bool CombatVRBowWantsTrigger();
// Ronda 51, cualquier hilo: arco VR listo (el agarre derecho en la espalda saca
// una flecha del carcaj en vez de guardar las armas) / pedirla (la saca el hilo
// del juego si la mano derecha esta vacia).
bool CombatVRBowArrowGesture();
void CombatVRRequestBowArrow();

// Ronda 30: armas a la espalda con un gesto (VrPadHook, hilo de Present).
// true si vale el gesto (partida, primera persona, brazos con los mandos,
// modo 1 o 2, fuera de un ataque del juego); weaponsOut = arma en la derecha
// o algo en la izquierda.
bool CombatVRArmsGestureState(bool* weaponsOut);
// Ronda 36: arma a dos manos en la derecha con los brazos en los mandos (modo
// 1 o 2, en partida): el agarre izquierdo es para sujetarla con la izquierda
// (CombatVR la coge cerca de su agarre y la suelta al soltarlo) y no va al
// juego (LB, bloquear). Hilo de Present.
bool CombatVRLeftGripHoldsWeapon();
// Ronda 38: accion de script en curso (palancas lvr_*, coger tke_*, escenas...):
// Supr no gira al personaje (HeadTrackHook). Cualquier hilo.
bool CombatVRScriptedAction();
// Guardar (true) o sacar las armas: Actions.StdToggleWeapons en el hilo del
// juego, como la cruceta arriba (soltando antes al enemigo activo).
void CombatVRRequestWeaponToggle(bool stow);
// Ronda 32: con la mano derecha vacia (modos 1 y 2, en partida) el gatillo
// derecho no va al juego: fijar enemigo lo hace el mod sin sacar las armas
// guardadas (el del juego las saca). Hilo de Present.
bool CombatVRSelectEnemyWithoutDraw();
void CombatVRRequestSelectEnemy();

} // namespace BladeVR
