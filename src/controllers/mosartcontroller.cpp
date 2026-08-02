#include "mosartcontroller.h"
#include "../core.h"

MosartController::MosartController(Radio *radio) : Controller(radio) {}

void MosartController::start() {
    this->channel = 1;
		this->setFilter(0xFF,0xFF,0xFF,0xFF);
		this->disableDonglePackets();
    this->setHardwareConfiguration();
}


void MosartController::onMatch(uint8_t *buffer, size_t size) {

}

void MosartController::setFilter(uint8_t a,uint8_t b,uint8_t c,uint8_t d) {
	this->filter.bytes[0] = a;
	this->filter.bytes[1] = b;
	this->filter.bytes[2] = c;
	this->filter.bytes[3] = d;
}

void MosartController::enableDonglePackets() {
	this->donglePackets = true;
}

void MosartController::disableDonglePackets() {
	this->donglePackets = false;
}

void MosartController::stop() {
  this->radio->disable();
}


int MosartController::getChannel() {
    return this->channel;
}

void MosartController::setChannel(int channel) {
  this->channel = channel;
  this->radio->fastFrequencyChange(channel, channel);
}

void MosartController::send(uint8_t *data, size_t size) {}


void MosartController::setHardwareConfiguration() {
  uint8_t preamble[] = {0xAA,0xAA};
  this->radio->setPreamble(preamble, 2);
	this->radio->setPrefixes();
  this->radio->setMode(MODE_NORMAL);
  this->radio->setFastRampUpTime(true);
  this->radio->setEndianness(BIG);
  this->radio->setTxPower(POS8_DBM);
  this->radio->disableRssi();
  this->radio->setPhy(BLE_1MBITS);
  this->radio->setHeader(0,0,0);
  this->radio->setWhitening(NO_WHITENING);
  this->radio->setWhiteningDataIv(0);
	this->radio->disableJammingPatterns();
  this->radio->setCrc(NO_CRC);
  this->radio->setCrcSkipAddress(false);
  this->radio->setCrcSize(2);
  this->radio->setCrcInit(0xFFFF);
  this->radio->setCrcPoly(0x1021);
  this->radio->setPayloadLength(15);
  this->radio->setInterFrameSpacing(0);
  this->radio->setExpandPayloadLength(15);
  this->radio->setFrequency(this->channel);
	this->radio->reload();
}
void MosartController::setJammerConfiguration() {
	uint8_t preamble[] = {0xAA,0xAA};
  this->radio->setPreamble(preamble, 2);
	this->radio->setPrefixes();
  this->radio->setMode(MODE_JAMMER);
  this->radio->setFastRampUpTime(false);
  this->radio->setEndianness(BIG);
  this->radio->setTxPower(POS8_DBM);
  this->radio->disableRssi();
  this->radio->setPhy(BLE_1MBITS);
  this->radio->setHeader(0,0,0);
  this->radio->setWhitening(NO_WHITENING);
  this->radio->setWhiteningDataIv(0);
	this->radio->disableJammingPatterns();
  this->radio->setCrc(NO_CRC);
  this->radio->setCrcSkipAddress(true);
  this->radio->setCrcSize(3);
  this->radio->setCrcInit(0x000000);
  this->radio->setCrcPoly(0x000000);
  this->radio->setPayloadLength(0);
  this->radio->setInterFrameSpacing(0);
  this->radio->setExpandPayloadLength(0);
	this->radio->setJammingInterval(0); // prevent jamming the same packet multiple times
  this->radio->setFrequency(this->channel);
  this->radio->reload();
}

void MosartController::startAttack(MosartAttack attack) {
  this->attackStatus.attack = attack;
	if (attack == MOSART_ATTACK_NONE) {
		this->setHardwareConfiguration();
		this->attackStatus.running = false;
		this->attackStatus.successful = false;
	}
	else if (attack == MOSART_ATTACK_JAMMING) {
		this->setJammerConfiguration();
		this->attackStatus.running = true;
		this->attackStatus.successful = false;
	}
}


bool MosartController::buildMosartPacket(uint32_t timestamp, uint8_t size, uint8_t *buffer, CrcValue crcValue, uint8_t rssi, MosartDecodedPacket *decoded) {
	(void)timestamp;
	(void)rssi;
	size_t end = 0;
	size_t start = 0;
	bool startFound = false;
	bool endFound = false;
	for (size_t i=0;i<size;i++) {
		if (buffer[i] != 0xAA && !startFound) {
			startFound = true;
			start=i-1;
		}
		if (buffer[i] == 0xFF) {
			endFound = true;
			end = i+1;
			break;
		}
	}
	if (!startFound || !endFound || end < 5) {
		return false;
	}
	size_t packetSize = (end - start) + 2;
	if (packetSize > MESSAGE_POOL_PACKET_SLOT_SIZE) {
		messagePoolRecordOverflow();
		return false;
	}
	uint8_t packet[packetSize];
	packet[0] = 0xF0;
	packet[1] = 0xF0;
	bool dongle = false;
	int stepdongle = 0;
	for (size_t i=0;i<(end-start);i++) {
		packet[i+2] = buffer[start+i] ^ 0x5A;
		if (packet[i+2] == 0x22 && stepdongle==1) {
			stepdongle++;
			dongle = true;
		}
		else {
			stepdongle = 0;
		}
		if (packet[i+2] == 0x11 && stepdongle==0) {
			stepdongle++;
		}
	}
	if (!dongle) {
		uint16_t extractedCrc = packet[packetSize-3] | (packet[packetSize-2] << 8);

		if (calculate_crc_mosart(packet+6, packetSize-3-6) == extractedCrc) crcValue.validity = VALID_CRC;
		else crcValue.validity = INVALID_CRC;
		memcpy(decoded->packet, packet, packetSize);
		decoded->size = packetSize;
		decoded->source = 0x00;
		decoded->crcValue = crcValue;
		return true;

	}
	else {

		crcValue.validity = VALID_CRC;
		memcpy(decoded->packet, packet+1, packetSize-1);
		decoded->size = packetSize-1;
		decoded->source = 0x01;
		decoded->crcValue = crcValue;
		return true;
	}
}

bool MosartController::checkAddress(MosartPacket *pkt) {
	if (this->filter.bytes[0] == 0xFF && this->filter.bytes[1] == 0xFF && this->filter.bytes[2] == 0xFF && this->filter.bytes[3] == 0xFF) {
		return true;
	}
	uint8_t *address = pkt->getAddress();
	for (int i=0;i<4;i++) {
		if (address[i] != this->filter.bytes[i]) {
			return false;
		}
	}
	return true;
}

void MosartController::sendJammingReport(uint32_t timestamp) {
  ////Core::instance->pushMessageToQueue(new JammingReportNotification(timestamp));
}

void MosartController::onReceive(uint32_t timestamp, uint8_t size, uint8_t *buffer, CrcValue crcValue, uint8_t rssi) {
  MosartDecodedPacket decoded;
  memset(&decoded, 0, sizeof(decoded));
	if (this->buildMosartPacket(timestamp,size,buffer,crcValue,rssi, &decoded)) {
		MosartPacket pkt(decoded.packet, decoded.size, timestamp, decoded.source, channel, rssi, decoded.crcValue);
		if (!pkt.isValid()) return;
		if (
				pkt.isCrcValid() &&
				(this->donglePackets || pkt.getSource() != 0x01) &&
				this->checkAddress(&pkt)
			) {
	  		this->addPacket(&pkt);
		}
	}
}


void MosartController::onJam(uint32_t timestamp) {
	  if (this->attackStatus.attack == MOSART_ATTACK_JAMMING) {
	    this->attackStatus.successful = true;
	    this->sendJammingReport(timestamp);
  	}
}

void MosartController::onEnergyDetection(uint32_t timestamp, uint8_t value) {}
