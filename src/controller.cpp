#include "controller.h"
#include "core.h"
#include "whad.h"

Controller::Controller(Radio *radio) {
	this->radio = radio;
}

/**
 * @brief Add packet to the list of packets to be sent to the host
 *
 * @param[in]   packet      Pointer to a `Packet` object containing the packet
 *                          information.
 */

void Controller::addPacket(Packet* packet) {
  if (packet->getPacketType() == BLE_PACKET_TYPE) {
    BLEPacket *blePacket = static_cast<BLEPacket*>(packet);
    whad::ble::RawPdu message(
        blePacket->getChannel(),
        blePacket->getRssi(),
        blePacket->getConnectionHandle(),
        blePacket->getAccessAddress(),
        whad::ble::PDU(packet->getPacketBuffer()+4, blePacket->extractPayloadLength() + 2),
        blePacket->getCrc(),
        blePacket->isCrcValid(),
        blePacket->getTimestamp(),
        (blePacket->getAccessAddress()==0x8e89bed6)?0:blePacket->getRelativeTimestamp(),
        (whad::ble::Direction)blePacket->getSource(),
        false,
        false
    );
    Core::instance->pushMessageToQueue(&message, MESSAGE_POOL_TRAFFIC_STREAM_EVENT, packet->getTimestamp());
  }
  else if (packet->getPacketType() == DOT15D4_PACKET_TYPE) {
    Dot15d4Packet *dot15d4Packet = static_cast<Dot15d4Packet*>(packet);
    whad::dot15d4::Dot15d4Packet dot15d4RawPacket(
        dot15d4Packet->getChannel(),
        dot15d4Packet->getPacketBuffer()+1,
        dot15d4Packet->getPacketSize()-3,
        dot15d4Packet->getFCS()
    );
    dot15d4RawPacket.addLqi(dot15d4Packet->getLQI());
    dot15d4RawPacket.addFcsValidity(dot15d4Packet->isCrcValid());
    dot15d4RawPacket.addRssi(dot15d4Packet->getRssi());
    dot15d4RawPacket.addTimestamp(dot15d4Packet->getTimestamp());
    whad::dot15d4::RawPduReceived message(dot15d4RawPacket);
    Core::instance->pushMessageToQueue(&message, MESSAGE_POOL_TRAFFIC_STREAM_EVENT, packet->getTimestamp());
  }
  else if (packet->getPacketType() == ESB_PACKET_TYPE) {
    ESBPacket *esbPacket = static_cast<ESBPacket*>(packet);
    if (esbPacket->isUnifying())
    {
        whad::unifying::PDU pdu(esbPacket->getPacketBuffer(), esbPacket->getPacketSize());
        whad::unifying::UnifyingAddress address(esbPacket->getAddress(), 5);
        whad::unifying::UnifyingPacket uniPacket(
            esbPacket->getChannel(),
            esbPacket->getPacketBuffer(),
            esbPacket->getPacketSize(),
            esbPacket->getCrc(),
            esbPacket->getRssi(),
            esbPacket->getTimestamp()
        );
        uniPacket.addCrcValidity(esbPacket->isCrcValid());
        whad::unifying::RawPduReceived message(uniPacket);
        message.addAddress(address);
        Core::instance->pushMessageToQueue(&message, MESSAGE_POOL_TRAFFIC_STREAM_EVENT, packet->getTimestamp());
    }
    else
    {
        whad::esb::Packet pdu(esbPacket->getPacketBuffer(), esbPacket->getPacketSize());
        whad::esb::EsbAddress address(esbPacket->getAddress(), 5);
        whad::esb::RawPacketReceived message(esbPacket->getChannel(), pdu);
        message.setRssi(esbPacket->getRssi());
        message.setCrcValidity(esbPacket->isCrcValid());
        message.setTimestamp(esbPacket->getTimestamp());
        message.setAddress(address);
        Core::instance->pushMessageToQueue(&message, MESSAGE_POOL_TRAFFIC_STREAM_EVENT, packet->getTimestamp());
    }
  }
  else if (packet->getPacketType() == GENERIC_PACKET_TYPE) {
    GenericPacket* genPacket = static_cast<GenericPacket*>(packet);
    whad::phy::Packet phyPacket(genPacket->getPacketBuffer(), genPacket->getPacketSize());
    whad::phy::Timestamp pktTimestamp(packet->getTimestamp()/1000000, packet->getTimestamp()%1000000);
    whad::phy::PacketReceived message(
        (2400000000 + packet->getChannel()*1000000),
        packet->getRssi(),
        pktTimestamp,
        phyPacket,
				genPacket->getSyncword(),
				genPacket->getEndianness(),
				genPacket->getDatarate(),
				genPacket->getDeviation(),
				genPacket->getModulation()
    );
    Core::instance->pushMessageToQueue(&message, MESSAGE_POOL_TRAFFIC_STREAM_EVENT, packet->getTimestamp());
  }
}


/**
 * @brief   Send debug message to the host.
 *
 * @param[in]   msg     Pointer to a text string to send as a debug message
 */

void Controller::sendDebug(const char* msg) {
    /* Craft a verbose message. */
    whad::generic::Verbose message(msg);

	/* Add notification to our message queue. */
    Core::instance->pushMessageToQueue(&message, MESSAGE_POOL_TRAFFIC_STREAM_EVENT, 0);
}
