#ifndef CORE_H
#define CORE_H

#include <stdlib.h>
#include <whad.h>

#include "version.h"
#include "led.h"
#include "display.h"
#include "serial.h"
#include "timer.h"
#include "radio.h"
#include "sequences/sequenceModule.h"

#include "messageQueue.h"
#include "messagePool.h"
#include "runtime.h"

 #ifdef BOARD_CLUE
#include "boardModule.h"
#include "menu.h"
class BleRuntime;
#endif

#include "controller.h"
#include "controllers/blecontroller.h"
#include "controllers/dot15d4controller.h"
#include "controllers/esbcontroller.h"
#include "controllers/antcontroller.h"
#include "controllers/mosartcontroller.h"
#include "controllers/genericcontroller.h"

class SerialComm;


extern "C" void core_send_bytes(uint8_t *p_bytes, int size);

class Core {
	private:
        whad_transport_cfg_t transportConfig;
		LedModule *ledModule;
		DisplayModule *displayModule;
		SerialComm *serialModule;
		TimerModule *timerModule;
		SequenceModule *sequenceModule;
		Radio *radio;

		MessageQueue messageQueue;

		BLEController *bleController;
		Dot15d4Controller *dot15d4Controller;
		ESBController *esbController;
		ANTController *antController;
		MosartController *mosartController;

		GenericController *genericController;
		Controller* currentController;

 #ifdef BOARD_CLUE
		BoardModule *boardModule;
		MenuManager *menuManager;
		BleRuntime *m_bleRuntime;
	#endif

		runtime_mode_t m_runtimeMode;

	public:
		static Core *instance;

		Core(runtime_mode_t mode = RUNTIME_RAW_WHAD);
		LedModule *getLedModule();
		DisplayModule *getDisplayModule();
		SerialComm *getSerialModule();
		SequenceModule *getSequenceModule();
		TimerModule *getTimerModule();
		Radio *getRadioModule();

		/* Runtime-aware accessors. */
		runtime_mode_t getRuntimeMode(void) const;
		bool hasRawRadio(void) const;

 	void setControllerChannel(int channel);

#ifdef BOARD_CLUE
		BleRuntime *getBleRuntime() { return m_bleRuntime; }
		void setProfileManager(void *profiles) {}
#endif

		#ifdef PA_ENABLED
			void configurePowerAmplifier(bool enabled);
		#endif

		void sendVerbose(const char* data);


		void sendDebug(const char* message);
		void sendDebug(uint8_t *buffer, uint8_t size);

		void processInputMessage(Message msg);
		void processGenericInputMessage(whad::NanoPbMsg msg);
		void processDiscoveryInputMessage(whad::discovery::DiscoveryMsg msg);
		void processDot15d4InputMessage(whad::dot15d4::Dot15d4Msg dot15d4Msg);
		void processBLEInputMessage(whad::ble::BleMsg bleMsg);
		void processESBInputMessage(whad::esb::EsbMsg esbMsg);
		void processUnifyingInputMessage(whad::unifying::UnifyingMsg uniMsg);
		void processPhyInputMessage(whad::phy::PhyMsg msg);

		bool selectController(Protocol controller);

		MessagePoolStatus pushMessageToQueue(whad::NanoPbMsg *msg, MessagePoolTrafficClass trafficClass = MESSAGE_POOL_TRAFFIC_COMMAND_RESPONSE, uint32_t sourceTimestamp = 0);
		MessagePoolStatus pushMessageToQueue(Message *msg, MessagePoolTrafficClass trafficClass = MESSAGE_POOL_TRAFFIC_COMMAND_RESPONSE, uint32_t sourceTimestamp = 0);
		Message* popMessageFromQueue();

		bool sendMessage(Message *msg);

		//void handleCommand(Command *cmd);
		void init();
		void loop();

};
#endif
