; ==========================================================
; INICIO DE FUNCIÓN asm_delay(uint16_t mseg)
; ==========================================================
.global asm_delay
.func asm_delay

asm_delay:
    
    push r26
    push r27
    push r28
    push r29
    push r30
    push r31
    
    movw r30, r24			; r31:r30 (Z) <- r25:r24 (mseg)

	loop_delay:
		sbiw r30, 0			; Comprueba si Z (mseg) es cero
		breq terminar_delay	; Si es cero, terminar

		; 1 ms @ 16 MHz = 16000 ciclos
		; 16000 / 4 (Realmente 4n - 1) = 4000 iteraciones aproximadamente
		; Contador para repetir loop (1 ms aproximadamente)
		; r27:r26 <- 4000
		ldi r27, 0xF	; Parte alta de 4000
		ldi r26, 0xA0	; Parte baja de 4000

		loop_1ms:			; Bucle de 1 ms
			sbiw r26, 1     ; Decrementar r27:r26
			brne loop_1ms	; Saltar si no es cero
    
		; Decrementar contador de mseg
		sbiw r30, 1			; Decrementar r31:r30
		rjmp loop_delay		; Saltar al inicio del bucle exterior
    
terminar_delay:
    
    pop     r31
    pop     r30
    pop     r29
    pop     r28
    pop     r27
    pop     r26
    
    ret

.endfunc