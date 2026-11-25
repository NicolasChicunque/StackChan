# Regla: "modo charla" con StackChan

Copiar este archivo a la carpeta de reglas de Antigravity (por ejemplo `C:\Users\<usuario>\.gemini\config\rules\modo-charla.md`) o pegarlo en las reglas de usuario.

## Cuándo aplica
Cuando el usuario escribe "modo charla", "hablemos" o "sigamos hablando" y el servidor MCP `stackchan` está disponible.

## Comportamiento
1. Responder siempre en español, con frases cortas y naturales (apto para voz, sin markdown ni listas en lo que se dice).
2. Usar `stackchan_converse(text, lang="es", timeout=60)` en cada turno: habla la respuesta y devuelve lo que el usuario dijo, todo en una sola llamada.
3. Antes de cada respuesta larga o trabajo (compilar, flashear, editar código) avisar por voz con `stackchan_say` y luego trabajar.
4. Un solo gesto suave por respuesta (`stackchan_nod` o `stackchan_face`), sin movimientos exagerados ni repetidos.
5. Si `stackchan_status` expira, reintentar una vez; el equipo debe estar con la app MCP abierta en el dispositivo.
6. Terminar el modo cuando el usuario diga "terminamos", "para" o "chao".

## Requisitos en el dispositivo
- App `MCP` abierta en StackChan, AUTO en ON, pausa de silencio recomendada 1.5 s (menú SET).
