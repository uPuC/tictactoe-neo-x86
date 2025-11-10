#include <avr/io.h>
#include <stdbool.h>
#include <stdint.h> // Para uint32_t, uint16_t, etc. (aunque <avr/io.h> lo puede incluir)
#include <string.h> // Para memset

#define LED_LINE0           PA1
#define LED_LINE1           PA5
#define LED_LINE2           PC6
#define LED_LINE3           PC4
#define LED_LINE4           PC0

#define BTN_GPIO            PB2

#define NUM_LED_PER_COLOR   9
#define NUM_LINES           5

// Estado de los LEDs
typedef enum LedColor_tag
{
    eRedLed = 0,
    eGreenLed = 1,
    eNumOfColors = 2
} eLedColor_t;

// Estado del botón
typedef enum ButtonState_tag
{
    eBtnUndefined = 0,
    eBtnShortKeyPress,
    eBtnDoubleKeyPress,
    eBtnLongKeyPress
} eButtonState_t;

// Estado del juego
typedef enum GameState_tag
{
    eGameRestart = 0,
    eOngoingGame,
    eStalemate,
    eRedPlayerWin,
    eGreenPlayerWin
} eGameState_t;

// Estado del tablero
typedef struct BoardState_tag
{
    bool gameBoard[eNumOfColors][NUM_LED_PER_COLOR];
    uint8_t cursor;
    eLedColor_t currentColor;
} sBoardState_t;

// Describe un pin de Charlieplexing
typedef struct Pin_tag
{
    volatile uint8_t *ddr;
    volatile uint8_t *port;
    uint8_t bit;
} Pin;

uint32_t milis = 0;

extern void asm_delay(uint16_t mseg); // Declaración de la función asm_delay

static eGameState_t currentGameState = eGameRestart;
static sBoardState_t boardState;

// Retardo con actualización de milis
static inline void delay_ms(uint16_t ms)
{
    asm_delay(ms);
    milis += ms;
}

// Se usa un arreglo de Pin para hacer la manipulación de pines genérica.
static const Pin kLines[NUM_LINES] = {
    { &DDRA, &PORTA, LED_LINE0 }, // L1 (PA1)
    { &DDRA, &PORTA, LED_LINE1 }, // L2 (PA5)
    { &DDRC, &PORTC, LED_LINE2 }, // L3 (PC6)
    { &DDRC, &PORTC, LED_LINE3 }, // L4 (PC4)
    { &DDRC, &PORTC, LED_LINE4 }, // L5 (PC0)
};

// Pairs para ROJO (ánodo->cátodo) por casilla 0..8
static const uint8_t kRedPairs[NUM_LED_PER_COLOR][2] = {
    {0,1}, {0,2}, {0,3},
    {1,2}, {1,3}, {3,4},
    {2,3}, {1,4}, {0,4}
};

// Combinaciones ganadoras (índices de celdas)
static const uint8_t kWins[8][3] = {
    {0,1,2}, {3,4,5}, {6,7,8}, // Filas
    {0,3,6}, {1,4,7}, {2,5,8}, // Columnas
    {0,4,8}, {2,4,6}           // Diagonales
};

/*
    @brief Obtiene el par de líneas (src,sink) para un LED específico y color.
    @param color Color del LED (eRedLed o eGreenLed)
    @param cellIdx Índice de la celda (0..8)
    @param src Puntero para devolver la línea de ánodo (HIGH)
    @param sink Puntero para devolver la línea de cátodo (LOW)
*/
static inline void getPair(eLedColor_t color, uint8_t cellIdx, uint8_t *src, uint8_t *sink)
{
    const uint8_t anR = kRedPairs[cellIdx][0];
    const uint8_t caR = kRedPairs[cellIdx][1];

    // En verde se invierte ánodo y cátodo
    *src = (color == eRedLed) ? anR : caR;
    *sink = (color == eRedLed) ? caR : anR;
}

/*
    @brief Pone la línea como ENTRADA (Hi-Z) sin glitch.
    @param line Línea a configurar
*/
static inline void lineHiZ(uint8_t line) 
{
    *kLines[line].port &= ~(1u << kLines[line].bit); // Latch LOW (primero para desactivar pull-up)
    *kLines[line].ddr  &= ~(1u << kLines[line].bit); // Modo entrada
}

/*
    @brief Pone la línea como SALIDA en LOW sin glitch.
    @param line Línea a configurar
*/
static inline void lineLow(uint8_t line) 
{
    *kLines[line].port &= ~(1u << kLines[line].bit); // Latch LOW
    *kLines[line].ddr  |=  (1u << kLines[line].bit); // Modo salida
}

/*
    @brief Pone la línea como SALIDA en HIGH sin glitch.
    @param line Línea a configurar
*/
static inline void lineHigh(uint8_t line)
{
    *kLines[line].port |=  (1u << kLines[line].bit); // Latch HIGH
    *kLines[line].ddr  |=  (1u << kLines[line].bit); // Modo salida
}

/*
    @brief Pone todas las líneas en alta impedancia (Hi-Z).
*/
static inline void allHiZ(void)
{
    for (uint8_t i = 0; i < NUM_LINES; i++)
	{
		lineHiZ(i);
	}
}

/*
    @brief Descarga todas las líneas para evitar ghosting.
    @param src Línea de ánodo (HIGH)
    @param sink Línea de cátodo (LOW)
*/
static inline void descarga(uint8_t src, uint8_t sink)
{
	lineHiZ(src); // Asegura que no haya glitch en la transición
    
	// Todas las líneas como SALIDA=LOW (drena cargas).
    for (uint8_t i = 0; i < NUM_LINES; i++)
	{
        // Garantiza LOW en latch y luego activa salida
        *kLines[i].port &= ~(1u << kLines[i].bit);
        *kLines[i].ddr |=  (1u << kLines[i].bit);
    }

    allHiZ();
}

/*
    @brief Activa un par de líneas para encender un LED específico.
    @param src Línea de ánodo (HIGH)
    @param sink Línea de cátodo (LOW)
*/
static inline void drivePair(uint8_t src, uint8_t sink)
{
    // Se asume que el resto de las líneas están en Hi-Z.
    lineHigh(src); // ÁNODO = HIGH (salida)
    lineLow(sink); // CÁTODO = LOW  (salida)
}

/*
    @brief Enciende un LED específico por un tiempo determinado.
    @param color Color del LED (eRedLed o eGreenLed)
    @param idx Índice de la celda (0..8)
    @param led_ms Tiempo en milisegundos para mantener el LED encendido
*/
static inline void lightCell(eLedColor_t color, uint8_t idx, uint16_t led_ms)
{
    uint8_t src, sink;
    getPair(color, idx, &src, &sink); // (src,sink) según color

    drivePair(src, sink); // Solo activar el LED deseado (demás en Hi-Z)
    delay_ms(led_ms);
    descarga(src, sink);  // Blanking/descarga global para matar fantasma
}

/*
    @brief Inicialización de los puertos y pines.
*/
static void initIO(void)
{
    DDRB &= ~(1 << PB2);
    PORTB |=  (1 << PB2);

    PORTA &= ~((1 << PA1) | (1 << PA5)); // <-- Limpia PORTA
    DDRA &= ~((1 << PA1) | (1 << PA5)); // <-- Limpia DDRA

    PORTC &= ~((1 << PC6) | (1 << PC4) | (1 << PC0)); // <-- Limpia PORTC
    DDRC &= ~((1 << PC6) | (1 << PC4) | (1 << PC0)); // <-- Limpia DDRC

    allHiZ();
}

/*
    @brief Despliega el estado del tablero en los LEDs
    @param bs Estado actual del tablero
*/
void displayBoard(const sBoardState_t *bs)
{
    // Constantes de parpadeo
    const uint32_t T_ON = 500; // Tiempo encendido
    const uint32_t T_OFF = 100; // Tiempo apagado
    const uint32_t T_TOTAL = T_ON + T_OFF; // Periodo total
    const bool cursorOn = ((milis % T_TOTAL) < T_ON); // Cursor parpadeante

    // Escaneo de las 9 casillas: rojo y verde; ~3 ms por LED encendido
    for (uint8_t i = 0; i < NUM_LED_PER_COLOR; i++)
    {
        bool red = bs->gameBoard[eRedLed][i];
        bool green = bs->gameBoard[eGreenLed][i];

		if (red && green) // Ambos ocupados
            green = false; // Prioridad a rojo
        
        if (red)
            lightCell(eRedLed, i, 3);
        else if (green)
            lightCell(eGreenLed, i, 3);
    }

    // Cursor (solo si la casilla actual está libre)
    if (cursorOn)
    {
        const uint8_t c = bs->cursor;
        const bool libre = !bs->gameBoard[eRedLed][c] && !bs->gameBoard[eGreenLed][c];
        
        if (libre)
            lightCell(bs->currentColor, c, 1);
    }

    allHiZ();
}

/*
    @brief Verifica si una celda está ocupada por algún jugador.
    @param bs Estado actual del tablero
    @param idx Índice de la celda a verificar
    @return true si la celda está ocupada, false si está libre
*/
static inline bool cellOccupied(const sBoardState_t *bs, uint8_t idx)
{
    return bs->gameBoard[eRedLed][idx] || bs->gameBoard[eGreenLed][idx];
}

/*
    @brief Incremento circular del índice de casilla (0..N-1)
    @param idx Índice actual
    @return Índice incrementado circularmente
*/
static inline uint8_t wrapInc(uint8_t idx)
{
    return (uint8_t)((idx + 1u) % NUM_LED_PER_COLOR);
}

/*
    @brief Decremento circular del índice de casilla (0..N-1)
    @param idx Índice actual
    @return Índice decrementado circularmente
*/
static inline uint8_t wrapDec(uint8_t idx)
{
    return (idx == 0u) ? (NUM_LED_PER_COLOR - 1u) : (uint8_t)(idx - 1u);
}

/*
    @brief Función para encontrar la siguiente casilla libre desde un índice dado.
    @param bs Estado actual del tablero
    @param start Índice de inicio para la búsqueda
    @param dir Dirección de búsqueda (+1 o -1)
    @param outIdx Puntero para devolver el índice de la siguiente casilla libre
    @return true si se encontró una casilla libre, false si no hay libres
*/
static bool findNextFreeFrom(const sBoardState_t *bs, uint8_t start, int8_t dir, uint8_t *outIdx)
{
    uint8_t i = start;
    *outIdx = 0;
    for (uint8_t count = 0; count < NUM_LED_PER_COLOR; count++)
    {
        if (!cellOccupied(bs, i))
        {
            *outIdx = i;
            return true;
        }
        
        i = (dir > 0) ? wrapInc(i) : wrapDec(i);
    }

    return false;
}

/*
   @brief Función para verificar si un jugador ha ganado.
   @param bs Estado actual del tablero
   @param c Color del jugador a verificar
   @return true si el jugador ha ganado, false en caso contrario
*/
static bool hasWin(const sBoardState_t *bs, eLedColor_t c)
{
    for (uint8_t w = 0; w < 8; w++)
    {
        if (bs->gameBoard[c][kWins[w][0]] &&
            bs->gameBoard[c][kWins[w][1]] &&
            bs->gameBoard[c][kWins[w][2]])
        {
            return true;
        }
    }
    return false;
}

/*
   @brief Función para verificar si el tablero está completamente lleno.
   @param bs Estado actual del tablero
   @return true si el tablero está lleno, false en caso contrario
*/
static bool boardFull(const sBoardState_t *bs)
{
    for (uint8_t i = 0; i < NUM_LED_PER_COLOR; i++)
    {
        if (!cellOccupied(bs, i))
            return false;
    }
    return true;
}

/*
   @brief Función para mover el cursor a la siguiente casilla libre.
   @param bs Estado actual del tablero
   @param dir Dirección en la que mover el cursor (+1 o -1)
*/
static void moveCursorToNextFree(sBoardState_t *bs, int8_t dir)
{
    uint8_t start = (dir > 0) ? wrapInc(bs->cursor) : wrapDec(bs->cursor);
    uint8_t next;
    
    if (findNextFreeFrom(bs, start, dir, &next))
        bs->cursor = next;
}

/*
    @brief Función para finalizar el turno de un jugador.
    Verifica si hay un ganador o empate, y cambia el turno.
    @param bs Estado actual del tablero
    @return Estado del juego tras finalizar el turno
*/
static eGameState_t endTurn(sBoardState_t *bs)
{
    if (hasWin(bs, bs->currentColor))
        return (bs->currentColor == eRedLed) ? eRedPlayerWin : eGreenPlayerWin;
    else if (boardFull(bs))
        return eStalemate;

    bs->currentColor = (bs->currentColor == eRedLed) ? eGreenLed : eRedLed; // Cambiar turno

    moveCursorToNextFree(bs, +1);

    return eOngoingGame;
}

/*
    @brief Función para verificar el estado del tablero tras una acción del jugador.
    @param boardState Estado actual del tablero
    @param buttonState Estado del botón (presionado, doble, largo)
    @return Estado del juego tras la acción
*/
eGameState_t checkBoard(sBoardState_t *boardState, eButtonState_t buttonState)
{
    switch (buttonState)
    {
        case eBtnShortKeyPress:
            moveCursorToNextFree(boardState, +1);
            break;
        case eBtnDoubleKeyPress:
            moveCursorToNextFree(boardState, -1);
            break;
        case eBtnLongKeyPress:
            if (!cellOccupied(boardState, boardState->cursor)) // Si la celda actual está libre
            {
                boardState->gameBoard[boardState->currentColor][boardState->cursor] = true;
                return endTurn(boardState);
            }
            else
                moveCursorToNextFree(boardState, +1);
            break;
        default:
            break;
    }

    // Garantizar que el cursor siempre apunte a una celda libre
    if (cellOccupied(boardState, boardState->cursor))
    {
        uint8_t next;
        if (!findNextFreeFrom(boardState, boardState->cursor, +1, &next))
        {
            // No hay libres en absoluto
            return eStalemate;
        }
        boardState->cursor = next;
    }

    if (hasWin(boardState, eRedLed))
        return eRedPlayerWin;
    else if (hasWin(boardState, eGreenLed))
        return eGreenPlayerWin;
    else if (boardFull(boardState))
        return eStalemate;
    
    return eOngoingGame;
}

/*
    @brief Función de animación para encender un conjunto de LEDs.
    @param color Color del LED (eRedLed o eGreenLed)
    @param mask Máscara que indica qué LEDs encender
    @param duration_ms Duración total de la animación en milisegundos
*/
static void lightMask(eLedColor_t color, const bool mask[NUM_LED_PER_COLOR], uint16_t duration_ms)
{
    uint16_t  elapsed = 0;

    while (elapsed < duration_ms)
	{
        for (uint8_t i = 0; i < NUM_LED_PER_COLOR; i++)
		{
            if (mask[i])
			{
                lightCell(color, i, 2);
                elapsed += 2;
            }

            if (elapsed >= duration_ms)
                break;
        }
    }

    allHiZ(); // Asegura que todo quede en Hi-Z al final
}

/*
    @brief Función de animación para encender todos los LEDs de un color.
    @param color Color del LED (eRedLed o eGreenLed)
    @param duration_ms Duración total de la animación en milisegundos
*/
static void lightAll(eLedColor_t color, uint16_t  duration_ms)
{
    bool mask[NUM_LED_PER_COLOR]; // Todas las celdas a true
    for (uint8_t i = 0; i < NUM_LED_PER_COLOR; i++) {
        mask[i] = true;
    }
    lightMask(color, mask, duration_ms);
}

/*
    @brief Función de animación de la 'X' para un jugador.
    @param color Color del LED (eRedLed o eGreenLed)
    @param duration_ms Duración total de la animación en milisegundos
*/
static void lightX(eLedColor_t color, uint16_t  duration_ms)
{
    bool xMask[NUM_LED_PER_COLOR] = { false };
    xMask[0] = xMask[2] = xMask[4] = xMask[6] = xMask[8] = true;

    lightMask(color, xMask, duration_ms);
}

/* 
    @brief Función de animación de fin de juego.
    Muestra una secuencia según el estado del juego.
    @param gameState Estado del juego (eRedPlayerWin, eGreenPlayerWin, eStalemate)
    @return true si la animación terminó, false si aún continúa
*/
bool playSequence(eGameState_t gameState)
{
    const uint16_t  T_ON  = 1000;    // Tiempo encendido
    const uint16_t  T_OFF = 500;     // Tiempo apagado

    // Variables estáticas para la FSM de la animación
    static eGameState_t lastState;  // Último estado de juego
    static bool started = false;    // Flag de inicio
    static uint8_t cycles = 0;      // Contador de ciclos

    // Si cambia de estado o es la primera vez, reinicia el contador de ciclos
    if (!started || lastState != gameState)
	{
        started = true;
        lastState = gameState;
        cycles = 0;
    }

    switch (gameState)
    {
        case eRedPlayerWin:
            lightAll(eRedLed, T_ON);
            allHiZ();
            delay_ms(T_OFF);
            break;
        case eGreenPlayerWin:
            lightAll(eGreenLed, T_ON);
            allHiZ();
            delay_ms(T_OFF);
            break;
        case eStalemate:
        default:
        {
            eLedColor_t c = (cycles % 2 == 0) ? eRedLed : eGreenLed;
            lightX(c, T_ON);
            allHiZ();
            delay_ms(T_OFF);
            break;
        }
    }

    cycles++;
    
    // ¿Terminó la animación?
    if (cycles >= 3)
    {
        started = false;
        cycles = 0;
        allHiZ();
        return true;   // avisa a loop() que ya puede resetear tablero
    }
    return false; // aún en animación
}

/*
    @brief Función de chequeo del estado del botón con máquina de estados.
    Detecta pulsaciones cortas, dobles y largas con debounce.
    @return Estado del botón (corto, largo, doble)
*/
eButtonState_t checkButton(void)
{
    // Máquina de estados del botón
    typedef enum btn_state_tag
    {
    	S_IDLE,
		S_DEB_PRESS,
		S_PRESSED,
		S_DEB_RELEASE,
		S_WAIT2,
		S_DEB_PRESS2,
		S_PRESSED2,
		S_DEB_RELEASE2
    } btn_state_t;

    // Umbrales (ms)
    const uint16_t T_DB   = 10;   // Debounce
    const uint16_t T_LONG = 1000; // Long  >= 1000 ms
    const uint16_t T_DBL  = 500;  // Doble = 500 ms tras soltar (tiempo máximo de espera)

    // Lectura instantánea
    const bool pressed = ((PINB & (1U << BTN_GPIO)) == 0);

    // Inicializaciones estáticas
    static btn_state_t st = S_IDLE; // Estado inicial
    static uint16_t  t0 = 0;         // Marca de tiempo para el estado actual
    static uint16_t  dur1 = 0;       // Duración de la 1ª pulsación
    static uint16_t  dur2 = 0;       // Duración de la 2ª pulsación

    switch (st)
    {
        case S_IDLE: // Reposo
            if (pressed)
            {
              st = S_DEB_PRESS; // Cambio: posible PRESSED
              t0 = milis;
            }
            break;
        case S_DEB_PRESS: // Debounce de PRESSED
            if (!pressed) // Si se suelta antes del tiempo, es debounce
            {
              st = S_IDLE; // Rebote (vuelve a IDLE)
            }
            else if (milis - t0 >= T_DB)
            {
              st = S_PRESSED; // Confirmado PRESSED
              t0 = milis;
            }
            break;
        case S_PRESSED: // Botón presionado
            if (!pressed)
            {
              dur1 = milis - t0; // Guarda duración de la 1ª pulsación
              st = S_DEB_RELEASE; // Cambio: posible RELEASE
              t0 = milis;
            }
            break;
        case S_DEB_RELEASE: // Debounce de RELEASE
            if (pressed)
            {
                st = S_PRESSED; // rebote (vuelve a PRESSED)
            }
            else if (milis - t0 >= T_DB) // Confirmado RELEASE
            {
                if (dur1 >= T_LONG) // Si fue larga
                {
                    st = S_IDLE; // Volver a IDLE
                    return eBtnLongKeyPress;
                }
                // Corta: abrir ventana para doble
                st = S_WAIT2;
                t0 = milis;
            }
            break;
        case S_WAIT2:
            if (pressed)
            {
                st = S_DEB_PRESS2; // Cambio: posible 2ª PRESSED
                t0 = milis;
            }
            else if (milis - t0 >= T_DBL)
            {
                st = S_IDLE;
                return eBtnShortKeyPress; // SHORT (expiró ventana)
            }
            break;
        case S_DEB_PRESS2:
            if (!pressed) 
            {
                st = S_WAIT2; // rebote (vuelve a la ventana)
            }
            else if (milis - t0 >= T_DB)
            {
                st = S_PRESSED2;
                t0 = milis;
            }
            break;
        case S_PRESSED2:
            if (!pressed)
            {
                dur2 = milis - t0;
                st = S_DEB_RELEASE2;
                t0 = milis;
            }
            break;
        case S_DEB_RELEASE2:
            if (pressed) 
            {
                st = S_PRESSED2; // rebote (vuelve a PRESSED2)
            }
            else if (milis - t0 >= T_DB)
            {
                st = S_IDLE;
                if (dur2 >= T_LONG)
                    return eBtnLongKeyPress; // LONG (si la 2ª pulsación fue larga)
                return eBtnDoubleKeyPress; // DOUBLE
            }
            break;
        default:
            st = S_IDLE; // Estado inválido: resetear
            break;
    }

    return eBtnUndefined;
}

/*
    @brief Función de configuración inicial del sistema.
*/
void setup(void)
{
    initIO();

    // Inicialización del estado del tablero (Nota: memset opera a nivel de bytes)
    memset(boardState.gameBoard, 0, sizeof(boardState.gameBoard));

    boardState.cursor = 0;
    boardState.currentColor = eRedLed;
    currentGameState = eOngoingGame;
}

/*
    @brief Bucle principal del juego.
*/
void loop(void)
{
    eButtonState_t buttonState = checkButton();

    switch (currentGameState)
	{
        case eOngoingGame:
            if (buttonState != eBtnUndefined)
                currentGameState = checkBoard(&boardState, buttonState);
            
            displayBoard(&boardState);
            break;
        case eRedPlayerWin:
        case eGreenPlayerWin:
        case eStalemate:
            if (playSequence(currentGameState)) // Si la animación terminó
            {
                setup(); // Reiniciar el juego
            }
            break;
        default:
            currentGameState = eOngoingGame;
            break;
    }
	
    delay_ms(1);
}