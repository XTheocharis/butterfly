#include "bsp.h"

#ifndef LED_H
#define LED_H

#define LED1 0
#define LED2 1
#ifdef BOARD_CLUE
#define LED3 2
#define LED_COUNT 3
#else
#define LED_COUNT 2
#endif

#define LED2_RED 1
#define LED2_GREEN 2
#define LED2_BLUE 3


typedef enum LedColor {
	RED,
	GREEN,
	BLUE,
	YELLOW,
	PURPLE,
	CYAN
} LedColor;

class LedModule
{
	private:
		bool state[LED_COUNT];
		LedColor ledColor;
	public:
		LedModule();

		void on(int num);
		void off(int num);
		void toggle(int num);		
		bool isOn(int num);
		void setColor(LedColor color);
};
#endif
