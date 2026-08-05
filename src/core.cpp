#include "core.h"
#include "capabilities.h"
#include <whad.h>
#include "nrf.h"
#include "nrf_gpio.h"
#include "messagePool.h"

// Global instance of Core
Core* Core::instance = NULL;
static Message msg;

void Core::processInputMessage(Message msg) {
  whad::NanoPbMsg whadMsg(&msg);

  switch (whadMsg.getType())
  {
    case whad::MessageType::GenericMsg:
      this->processGenericInputMessage(whadMsg);
      break;

    case whad::MessageType::DiscoveryMsg:
        this->processDiscoveryInputMessage(whad::discovery::DiscoveryMsg(whadMsg));
        break;

    case whad::MessageType::DomainMsg:
    {
#ifdef BOARD_CLUE
        /* Board domain is always available on CLUE regardless of
         * runtime mode — it does not depend on raw radio. */
        if (whadMsg.getDomain() == whad::MessageDomain::DomainBoard) {
            whad::board::BoardMsg boardMsg(whadMsg);
            this->boardModule->processMessage(boardMsg);
            break;
        }
#endif

        /* BLE-HID runtime does not serve radio domains — reject
         * before touching raw controller paths. Board domain
         * routing is handled by BoardModule (Todo 14). */
        if (!this->hasRawRadio()) {
            whad::generic::UnsupportedDomain err;
            this->pushMessageToQueue(&err);
            break;
        }

        switch (whadMsg.getDomain())
        {
            case whad::MessageDomain::DomainBle:
                this->processBLEInputMessage(whad::ble::BleMsg(whadMsg));
                break;

            case whad::MessageDomain::DomainDot15d4:
                this->processDot15d4InputMessage(whad::dot15d4::Dot15d4Msg(whadMsg));
                break;

            case whad::MessageDomain::DomainEsb:
                this->processESBInputMessage(whad::esb::EsbMsg(whadMsg));
                break;

            case whad::MessageDomain::DomainUnifying:
                this->processUnifyingInputMessage(whad::unifying::UnifyingMsg(whadMsg));
                break;

            case whad::MessageDomain::DomainPhy:
                this->processPhyInputMessage(whad::phy::PhyMsg(whadMsg));
                break;

            default:
                // send error ?
                break;
        }
    }
    break;

    default:
        break;
  }
}

void Core::processGenericInputMessage(whad::NanoPbMsg msg) {
}

void Core::processDiscoveryInputMessage(whad::discovery::DiscoveryMsg msg) {


#ifdef BOARD_CLUE
    const whad_domain_desc_t *activeCaps =
        getRuntimeCapabilities(m_runtimeMode);
#else
    const whad_domain_desc_t *activeCaps =
        (const whad_domain_desc_t *)CAPABILITIES;
#endif

    switch (msg.getType())
    {
        /* Device reset message processing. */
        case whad::discovery::MessageType::DeviceResetMsg:
            {
                /* Raw-WHAD: stop radio and clear controller.
                 * BLE-HID: reset transport/Board state without
                 * touching RADIO — BLE owns the radio path. */
                if (this->hasRawRadio()) {
                    this->radio->disable();
                    this->radio->setController(NULL);
                }
                this->currentController = NULL;

                whad::discovery::ReadyResp resp;
                this->pushMessageToQueue(&resp);
            }
            break;

        /* Device info query message processing. */
        case whad::discovery::MessageType::DeviceInfoQueryMsg:
            {
                whad::discovery::DeviceInfoQuery query(msg);

                if (query.getVersion() >= WHAD_MIN_VERSION)
                {
                    /* Craft device ID from unique values. */
                    uint8_t deviceId[16];
                    memcpy(&deviceId[0], (const void *)NRF_FICR->DEVICEID, 8);
                    memcpy(&deviceId[8], (const void *)NRF_FICR->DEVICEADDR, 8);

                    whad::discovery::DeviceInfoResp resp(
                        whad::discovery::Butterfly,
                        deviceId,
                        WHAD_MIN_VERSION,
                        115200,
                        std::string(FIRMWARE_AUTHOR),
                        std::string(FIRMWARE_URL),
                        VERSION_MAJOR,
                        VERSION_MINOR,
                        VERSION_REVISION,
                        (whad_domain_desc_t *)activeCaps
                    );
                    this->pushMessageToQueue(&resp);
                }
                else
                {
                    { whad::generic::Error _resp; this->pushMessageToQueue(&_resp); }
                }
            }
            break;

        /* Domain info query message processing. */
        case whad::discovery::MessageType::DomainInfoQueryMsg:
            {
                whad::discovery::DomainInfoQuery query(msg);
                whad::discovery::Domains domain = query.getDomain();

                if (whad::discovery::isDomainSupported(activeCaps, domain))
                {
                    whad::discovery::DomainInfoResp resp(
                        (whad::discovery::Domains)domain,
                        (whad_domain_desc_t *)activeCaps
                    );
                    this->pushMessageToQueue(&resp);
                }
                else {
                    { whad::generic::UnsupportedDomain _resp; this->pushMessageToQueue(&_resp); }
                }
            }
            break;

        default:
            { whad::generic::Error _resp; this->pushMessageToQueue(&_resp); }
            break;
    }

}

void Core::processDot15d4InputMessage(whad::dot15d4::Dot15d4Msg dot15d4Msg) {


    if (this->currentController != this->dot15d4Controller) {
        this->selectController(DOT15D4_PROTOCOL);
    }

    switch (dot15d4Msg.getType())
    {
        case whad::dot15d4::SniffModeMsg:
        {
            whad::dot15d4::SniffMode query(dot15d4Msg);

            int channel = query.getChannel();
            if (channel >= 11 && channel <= 26) {
                this->dot15d4Controller->setChannel(channel);
                this->dot15d4Controller->enterReceptionMode();
                this->dot15d4Controller->setAutoAcknowledgement(false);
                { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
            }
            else {
                { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
            }
        }
        break;

        case whad::dot15d4::EnergyDetectionMsg:
        {
            whad::dot15d4::EnergyDetect query(dot15d4Msg);

            int channel = query.getChannel();
            if (channel >= 11 && channel <= 26) {
                this->dot15d4Controller->setChannel(channel);
                this->dot15d4Controller->enterEDScanMode();
                { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
            }
            else {
                { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
            }
        }
        break;

        case whad::dot15d4::StartMsg:
        {
            this->currentController->start();
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        case whad::dot15d4::EndDeviceModeMsg:
        {
            whad::dot15d4::EndDeviceMode query(dot15d4Msg);

            int channel = query.getChannel();
            if (channel >= 11 && channel <= 26) {
                this->dot15d4Controller->setChannel(channel);
                this->dot15d4Controller->enterReceptionMode();
                this->dot15d4Controller->setAutoAcknowledgement(true);
                { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
            }
            else {
                { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
            }
        }
        break;


        case whad::dot15d4::CoordModeMsg:
        {
            whad::dot15d4::CoordMode query(dot15d4Msg);

            int channel = query.getChannel();
            if (channel >= 11 && channel <= 26) {
                this->dot15d4Controller->setChannel(channel);
                this->dot15d4Controller->enterReceptionMode();
                this->dot15d4Controller->setAutoAcknowledgement(true);
                { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
            }
            else {
                { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
            }
        }
        break;


        case whad::dot15d4::RouterModeMsg:
        {
            whad::dot15d4::RouterMode query(dot15d4Msg);

            int channel = query.getChannel();
            if (channel >= 11 && channel <= 26) {
                this->dot15d4Controller->setChannel(channel);
                this->dot15d4Controller->enterReceptionMode();
                this->dot15d4Controller->setAutoAcknowledgement(true);
                { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
            }
            else {
                { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
            }
        }
        break;

        case whad::dot15d4::SetNodeAddressMsg:
        {
            whad::dot15d4::SetNodeAddress query(dot15d4Msg);

            if (query.getAddressType() == whad::dot15d4::AddressShort) {
                uint16_t shortAddress = query.getAddress() & 0xFFFF;
                this->dot15d4Controller->setShortAddress(shortAddress);
                { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }

            }
            else {
                uint64_t extendedAddress = query.getAddress();
                this->dot15d4Controller->setExtendedAddress(extendedAddress);
                { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
            }
        }
        break;


        case whad::dot15d4::StopMsg:
        {
            this->currentController->stop();
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;


        case whad::dot15d4::SendMsg:
        {
            whad::dot15d4::SendPdu query(dot15d4Msg);

            int channel = query.getChannel();

            if (channel >= 11 && channel <= 26) {
                /* Set channel. */
                this->dot15d4Controller->setChannel(channel);

				/* Build packet. */
				size_t size = query.getPdu().getSize();
				uint8_t packet[MESSAGE_POOL_PACKET_SLOT_SIZE];
				if ((1 + size) > sizeof(packet)) {
					{ whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
					break;
				}
				packet[0] = size;
				memcpy(packet+1,query.getPdu().getBytes(), size);
				this->dot15d4Controller->send(packet, size+1, false);

                /* Success. */
                { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
            }
            else {
                { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
            }
        }
        break;


        case whad::dot15d4::SendRawMsg:
        {
            whad::dot15d4::SendRawPdu query(dot15d4Msg);

            int channel = query.getChannel();
            uint16_t fcs = (uint16_t)(query.getFcs() & 0xFFFF);

            if (channel >= 11 && channel <= 26) {
                /* Set channel. */
                this->dot15d4Controller->setChannel(channel);

				/* Build packet. */
				size_t size = query.getPdu().getSize();
				uint8_t packet[MESSAGE_POOL_PACKET_SLOT_SIZE];
				if ((3 + size) > sizeof(packet)) {
					{ whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
					break;
				}
				packet[0] = size+2;
				memcpy(packet+1, query.getPdu().getBytes(), size);
				memcpy(packet+1+size, &fcs, 2);
				this->dot15d4Controller->send(packet, size+3, true);

                /* Success. */
                { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
            }
            else {
                { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
            }
        }
        break;

        default:
            { whad::generic::Error _resp; this->pushMessageToQueue(&_resp); }
            break;
    }

}

void Core::processBLEInputMessage(whad::ble::BleMsg bleMsg) {


    if (this->currentController != this->bleController) {
        this->selectController(BLE_PROTOCOL);
    }

    switch (bleMsg.getType())
    {
        case whad::ble::SniffAdvMsg:
        {
            whad::ble::SniffAdv query(bleMsg);
            uint8_t *bd_addr = query.getAddress().getAddressBuf();

            this->bleController->setChannel(query.getChannel());
            this->bleController->setAdvertisementsTransmitIndicator(true);
            this->bleController->setFilter(
                                            true,
                                            bd_addr[5],
                                            bd_addr[4],
                                            bd_addr[3],
                                            bd_addr[2],
                                            bd_addr[1],
                                            bd_addr[0]
            );
            this->bleController->setFollowMode(false);
            this->bleController->sniff();
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        case whad::ble::ReactiveJamMsg:
        {
            whad::ble::ReactiveJam query(bleMsg);

            this->bleController->setChannel(query.getChannel());
            this->bleController->setReactiveJammerConfiguration(query.getPattern(), query.getPatternLength(), query.getPosition());
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        case whad::ble::SniffConnReqMsg:
        {
            whad::ble::SniffConnReq query(bleMsg);
            uint8_t *bd_address = query.getTargetAddress().getAddressBuf();

            this->bleController->setChannel(query.getChannel());
            this->bleController->setAdvertisementsTransmitIndicator(query.mustReportAdv());
            this->bleController->setEmptyTransmitIndicator(query.mustReportEmpty());
            this->bleController->setFilter(
                false,
                bd_address[5],
                bd_address[4],
                bd_address[3],
                bd_address[2],
                bd_address[1],
                bd_address[0]
            );
            this->bleController->setFollowMode(true);
            this->bleController->sniff();
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        case whad::ble::SniffAAMsg:
        {
            whad::ble::SniffAccessAddress query(bleMsg);
            this->bleController->setChannel(0);
            this->bleController->setMonitoredChannels(query.getChannelMap().getChannelMapBuf());
            this->bleController->sniffAccessAddresses();
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        case whad::ble::SniffActConnMsg:
        {
            whad::ble::SniffActiveConn query(bleMsg);

            this->bleController->setChannel(0);
            this->bleController->setMonitoredChannels(query.getChannels().getChannelMapBuf());

            if (query.getCrcInit() == 0)
            {
                this->bleController->recoverCrcInit(query.getAccessAddress());
            }
            else if (query.getChannelMap().isNull()) {
                this->bleController->recoverChannelMap(query.getAccessAddress(), query.getCrcInit());
            }
            else if (query.getHopInterval() == 0) {
                this->bleController->recoverHopInterval(query.getAccessAddress(), query.getCrcInit(), query.getChannelMap().getChannelMapBuf());
            }
            else if (query.getHopIncrement() == 0) {
                this->bleController->recoverHopIncrement(query.getAccessAddress(), query.getCrcInit(), query.getChannelMap().getChannelMapBuf(), query.getHopInterval());
            }
            else {
                this->bleController->attachToExistingConnection(query.getAccessAddress(), query.getCrcInit(), query.getChannelMap().getChannelMapBuf(), query.getHopInterval(), query.getHopIncrement());
            }

            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        case whad::ble::StartMsg:
        {
            this->bleController->start();
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        case whad::ble::StopMsg:
        {
            this->bleController->stop();
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        case whad::ble::ScanModeMsg:
        {
            whad::ble::ScanMode query(bleMsg);

            this->bleController->startScanning(query.isActiveModeEnabled());
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        case whad::ble::SendRawPduMsg:
        {
            whad::ble::SendRawPdu query(bleMsg);
            uint32_t max_retry = 1 << 24;

            switch (query.getDirection())
            {
                case whad::ble::DirectionInjectionToSlave:
                {
                     if (this->bleController->getState() == SNIFFING_CONNECTION) {
                        this->bleController->setAttackPayload(query.getPdu().getBytes(), query.getPdu().getSize());
                        this->bleController->startAttack(BLE_ATTACK_FRAME_INJECTION_TO_SLAVE);
                        { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
                    }
                    else {
                        { whad::generic::WrongMode _resp; this->pushMessageToQueue(&_resp); }
                    }
                }
                break;

                case whad::ble::DirectionInjectionToMaster:
                {
                    if (this->bleController->getState() == SNIFFING_CONNECTION) {
                        this->bleController->setAttackPayload(query.getPdu().getBytes(), query.getPdu().getSize());
                            this->bleController->startAttack(BLE_ATTACK_FRAME_INJECTION_TO_MASTER);
                        { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
                    }
                    else {
                        { whad::generic::WrongMode _resp; this->pushMessageToQueue(&_resp); }
                    }
                }
                break;

                case whad::ble::DirectionMasterToSlave:
                {
                    if (this->bleController->getState() == SIMULATING_MASTER || this->bleController->getState() == PERFORMING_MITM) {
                        this->bleController->setMasterPayload(query.getPdu().getBytes(), query.getPdu().getSize());
                        while (max_retry>0 && !this->bleController->isMasterPayloadTransmitted()) {--max_retry;}
                        if (max_retry != 0) {
                            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
                        } else {
                            { whad::generic::Error _resp; this->pushMessageToQueue(&_resp); }

                        }
                    }
                    else {
                        { whad::generic::WrongMode _resp; this->pushMessageToQueue(&_resp); }
                    }
                }
                break;

                case whad::ble::DirectionSlaveToMaster:
                {
                    if (this->bleController->getState() == SIMULATING_SLAVE || this->bleController->getState() == PERFORMING_MITM) {

                        this->bleController->setSlavePayload(query.getPdu().getBytes(), query.getPdu().getSize());
                        while (!this->bleController->isSlavePayloadTransmitted()) {}
                        { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
                    }
                    else {
                        { whad::generic::WrongMode _resp; this->pushMessageToQueue(&_resp); }
                    }
                }
                break;

                case whad::ble::DirectionUnknown:
                {
                  /* TODO: we use conn handle as channel for raw injection, insert a channel field into protocol ? */
                  if (this->bleController->rawInject(
                      query.getPdu().getBytes(),
                      query.getPdu().getSize(),
                      query.getConnHandle(),
                      query.getAccessAddress()
                    )) {
                    { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
                  }
                  else {
                      { whad::generic::Error _resp; this->pushMessageToQueue(&_resp); }
                  }
                  break;

                }
                default:
                {
                    { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
                }
                break;
            }
        }
        break;

        case whad::ble::HijackMasterMsg:
        {
            this->bleController->startAttack(BLE_ATTACK_MASTER_HIJACKING);
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        case whad::ble::HijackSlaveMsg:
        {
            this->bleController->startAttack(BLE_ATTACK_SLAVE_HIJACKING);
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        case whad::ble::HijackBothMsg:
        {
            this->bleController->startAttack(BLE_ATTACK_MITM);
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        case whad::ble::CentralModeMsg:
        {
            //if (this->bleController->getState() == CONNECTION_INITIATION || this->bleController->getState() == SIMULATING_MASTER || this->bleController->getState() == PERFORMING_MITM) {
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
            /*}
            else {
                response = Whad::buildResultMessage(generic_ResultCode_WRONG_MODE);
            }*/
        }
        break;

        case whad::ble::PeriphModeMsg:
        {
            whad::ble::PeripheralMode query(bleMsg);

            if (this->bleController->getState() == SIMULATING_SLAVE || this->bleController->getState() == PERFORMING_MITM) {
                { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
            }
            else {
                /* Make sure we have at least some data to advertise. */
                if (query.getAdvDataLength() == 0)
                {
                    /* Parameter error ! */
                    { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
                }
                else
                {
                    /* Configure device with the required advertising data. */
                    this->bleController->advertise(
                        query.getAdvData(), query.getAdvDataLength(),
                        query.getScanRsp(), query.getScanRspLength(),
                        true,
                        100);

                    /* Start advertising. */
                    this->bleController->start();

                    /* Success. */
                    { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
                }
            }
        }
        break;

        case whad::ble::ConnectToMsg:
        {
            whad::ble::ConnectTo query(bleMsg);
            uint8_t *bd_address = query.getTargetAddr().getAddressBuf();

            this->bleController->setChannel(37);
            uint8_t address[6];
            address[0] = bd_address[5];
            address[1] = bd_address[4];
            address[2] = bd_address[3];
            address[3] = bd_address[2];
            address[4] = bd_address[1];
            address[5] = bd_address[0];

            uint32_t accessAddress = 0x23a3d487;
            uint32_t crcInit = 0x049095;
            uint16_t hopInterval = 56;
            uint8_t hopIncrement = 8;
            uint8_t channelMap[5] = {
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0x1F
            };

            if (query.getAccessAddr() != 0)
            {
                accessAddress = query.getAccessAddr();
            }

            if (!query.getChannelMap().isNull())
            {
                memcpy(channelMap, query.getChannelMap().getChannelMapBuf(), 5);
            }

            if (query.getCrcInit() != 0)
            {
                crcInit = query.getCrcInit();
            }

            if (query.getHopInterval() != 0)
            {
                hopInterval = query.getHopInterval();
            }

            if (query.getHopIncrement() != 0)
            {
                hopIncrement = query.getHopIncrement();
            }

            //this->bleController->setEmptyTransmitIndicator(true);

            this->bleController->connect(
                address,
                query.getTargetAddr().getType() == whad::ble::AddressRandom,
                accessAddress,
                crcInit,
                3,
                9,
                hopInterval,
                0,
                42,
                1,
                hopIncrement,
                channelMap
            );

            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        case whad::ble::DisconnectMsg:
        {
            this->bleController->disconnect();
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        case whad::ble::PrepareSeqMsg:
        {
            /* TODO: Implement pattern-based sequence. */

            SequenceDirection direction = BLE_TO_SLAVE;
            Trigger* trigger = NULL;
            int numberOfPackets = 0;
            PacketSequence *sequence = NULL;

            switch (whad::ble::PrepareSequence::getType(bleMsg))
            {
                case whad::ble::SequenceManual:
                {
                    whad::ble::PrepareSequenceManual query(bleMsg);

                    /* Change direction if targeted to master. */
                    if ((query.getDirection() == whad::ble::DirectionInjectionToMaster) ||
                        (query.getDirection() == whad::ble::DirectionSlaveToMaster))
                    {
                        direction = BLE_TO_MASTER;
                    }

                    /* Set trigger. */
                    trigger = new ManualTrigger();

                    /* Process sequence packets. */
                    numberOfPackets = query.getPackets().size();
                    sequence = this->sequenceModule->createSequence(numberOfPackets, trigger, direction, query.getId());
                    for (whad::ble::PDU& packet : query.getPackets())
                    {
                        sequence->preparePacket(packet.getBytes(), packet.getSize() , true);
                    }

                    /* Success. */
                    { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
                }
                break;

                case whad::ble::SequenceConnEvt:
                {
                    whad::ble::PrepareSequenceConnEvt query(bleMsg);

                    /* Change direction if targeted to master. */
                    if ((query.getDirection() == whad::ble::DirectionInjectionToMaster) ||
                        (query.getDirection() == whad::ble::DirectionSlaveToMaster))
                    {
                        direction = BLE_TO_MASTER;
                    }

                    /* Set trigger. */
                    trigger = new ConnectionEventTrigger(query.getConnEvt());

                    /* Process sequence packets. */
                    numberOfPackets = query.getPackets().size();
                    sequence = this->sequenceModule->createSequence(numberOfPackets, trigger, direction, query.getId());
                    for (whad::ble::PDU& packet : query.getPackets())
                    {
                        sequence->preparePacket(packet.getBytes(), packet.getSize() , true);
                    }

                    /* Success. */
                    { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
                }
                break;

                case whad::ble::SequencePattern:
                {
                    { whad::generic::WrongMode _resp; this->pushMessageToQueue(&_resp); }
                }
                break;
            }
        }
        break;

        case whad::ble::PrepareSeqTriggerMsg:
        {
            whad::ble::ManualTrigger query(bleMsg);

            uint8_t id = query.getId();
            if (this->bleController->checkManualTriggers(id)) {
                { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
            }
            else {
                { whad::generic::Error _resp; this->pushMessageToQueue(&_resp); }
            }
        }
        break;

        case whad::ble::SetBdAddressMsg:
        {
            whad::ble::SetBdAddress query(bleMsg);
            uint8_t *bd_address = query.getAddress()->getAddressBuf();

            uint8_t address[6] = {
                bd_address[5],
                bd_address[4],
                bd_address[3],
                bd_address[2],
                bd_address[1],
                bd_address[0]
            };
            this->bleController->setOwnAddress(address, false);
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        case whad::ble::PrepareSeqDeleteMsg:
        {
            whad::ble::DeleteSequence query(bleMsg);

            uint8_t id = query.getId();
            if (this->bleController->deleteSequence(id)) {
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
            }
            else {
            { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
            }
        }
        break;

        case whad::ble::SetEncryptionMsg:
        {
            whad::ble::SetEncryption query(bleMsg);

            if (query.isEnabled()) {
                this->bleController->configureEncryption(
                    query.getLLKey(),
                    query.getLLIv(),
                    0
                );
                if (this->bleController->startEncryption()) {
                    { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
                }
                else {
                    { whad::generic::Error _resp; this->pushMessageToQueue(&_resp); }
                }
            }
            else {
                if (this->bleController->stopEncryption()) {
                    { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
                }
                else {
                    { whad::generic::Error _resp; this->pushMessageToQueue(&_resp); }
                }
            }
        }
        break;

        default:
        {
            { whad::generic::Error _resp; this->pushMessageToQueue(&_resp); }
        }
        break;
    }

}


void Core::processESBInputMessage(whad::esb::EsbMsg esbMsg) {

    uint8_t address[5];
    uint8_t addressLen = 0;

    if (this->currentController != this->esbController) {
        this->selectController(ESB_PROTOCOL);
        this->esbController->disableUnifying();
    }

    /* Dispatch ESB message. */
    switch (esbMsg.getType())
    {
        /* Sniffing mode. */
        case whad::esb::SniffMsg:
        {
            /* Wrap our ESB message into a SniffMode message. */
            whad::esb::SniffMode query(esbMsg);

            /* If channel is valid (0xFF is a magic value to sniff on all channels). */
            int channel = query.getChannel();
            if (channel == 0xFF || (channel >= 0 && channel <= 100))
            {
                /* Retrieve the address length in bytes. */
                addressLen = query.getAddress().getLength();

                /* Make sure this length is valid (0 < length <= 5). */
                if ((addressLen > 0) && (addressLen <= 5))
                {
                    /* Copy address temporarily. */
                    memcpy(address, query.getAddress().getAddressBuf(), addressLen);

                    /* Set this address as a filter for our ESB controller. */
                    this->esbController->setFilter(
                        address[0],
                        address[1],
                        address[2],
                        address[3],
                        address[4]
                    );

                    /* Set channel information. */
                    this->esbController->setChannel(query.getChannel());

                    /* If acks must be reported, ask our controller to do so. */
                    if (query.mustShowAcks()) {
                        this->esbController->enableAcknowledgementsSniffing();
                    }
                    else {
                        this->esbController->disableAcknowledgementsSniffing();
                    }

                    /* Success ! */
                    { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
                }
                else
                {
                    /* Parameter error (wrong address size). */
                    { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
                }
            }
            else {
                /* Parameter error (Invalid channel value). */
                { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
            }
        }
        break;

        /* Start message. */
        case whad::esb::StartMsg:
        {
            /* Start our controller in current mode. */
            this->esbController->start();

            /* Success. */
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        /* Stop message. */
        case whad::esb::StopMsg:
        {
            /* Stop our controller (go to idle mode). */
            this->esbController->stop();

            /* Success. */
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        /* Send raw packet message. */
        case whad::esb::SendRawMsg:
        {   
            /* Wrap our esbMsg into a SendPacketRaw message. */
            whad::esb::SendPacketRaw query(esbMsg);

            int channel = query.getChannel();
            if (channel >= 0 && channel <= 100) {
                this->esbController->setChannel(channel);
            }

            if (query.getPacket().getSize() > 0)
            {
                if (this->esbController->send(query.getPacket().getBytes(), query.getPacket().getSize(), query.getRetrCount())) {
                    /* Success. */
                    { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
                }
                else {
                    /* Error while sending packet. */
                    { whad::generic::Error _resp; this->pushMessageToQueue(&_resp); }
                }
            }
            else
            {
                /* Error while sending packet, we can't send nothing. */
                { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
            }
        }
        break;

        /* Set node address message. */
        case whad::esb::SetNodeAddrMsg:
        {
            /* Wrap our ESB message into a SetNodeAddress message. */
            whad::esb::SetNodeAddress query(esbMsg);

            /* Retrieve the address length in bytes. */
            addressLen = query.getAddress().getLength();

            /* Make sure this length is valid (0 < length <= 5). */
            if ((addressLen > 0) && (addressLen <= 5))
            {
                /* Copy address temporarily. */
                memcpy(address, query.getAddress().getAddressBuf(), addressLen);

                /* Set ESB address. */
                this->esbController->setFilter(
                    address[0],
                    address[1],
                    address[2],
                    address[3],
                    address[4]
                );

                /* Success. */
                { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
            }
            else
            {
                /* Parameter error (invalid address size). */
                { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
            }
        }
        break;

        /* Receiver mode message. */
        case whad::esb::PrxMsg:
        {
            /* Wrap our ESB message in a PrxMode message. */
            whad::esb::PrxMode query(esbMsg);

            /* Configure our controller in PRX (receiver) mode. */
            this->esbController->setChannel(query.getChannel());
            this->esbController->disableAcknowledgementsSniffing();
            this->esbController->enableAcknowledgementsTransmission();

            /* Success. */
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        /* Transmitter mode message. */
        case whad::esb::PtxMsg:
        {
            /* Wrap our ESB message into a PtxMode message. */
            whad::esb::PtxMode query(esbMsg);

            /* Configure our ESB controller in PTX (transmitter) mode. */
            this->esbController->setChannel(query.getChannel());
            this->esbController->enableAcknowledgementsSniffing();
            this->esbController->disableAcknowledgementsTransmission();

            /* Success. */
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        case whad::esb::UnknownMsg:
        default:
        {
            /* Error (unknown message). */
            { whad::generic::Error _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

    }

}

void Core::processUnifyingInputMessage(whad::unifying::UnifyingMsg uniMsg) {

    uint8_t address[5];
    uint8_t addressLen = 0;

    if (this->currentController != this->esbController) {
    this->selectController(ESB_PROTOCOL);
    this->esbController->enableUnifying();
    }

    /* Dispatch messages. */
    switch (uniMsg.getType())
    {
        /* Sniffing mode message. */
        case whad::unifying::SniffModeMsg:
        {
            /* Wrap our Unifying message into a SniffMode message. */
            whad::unifying::SniffMode query(uniMsg);

            /* Check channel validity. */
            int channel = query.getChannel();
            if (channel == 0xFF || (channel >= 0 && channel <= 100))
            {

                addressLen = query.getAddress().getLength();
                if ((addressLen > 0) && (addressLen <= 5))
                {
                    /* Copy address. */
                    memcpy(address, query.getAddress().getBytes(), addressLen);

                    /* Set address. */
                    this->esbController->setFilter(
                        address[0],
                        address[1],
                        address[2],
                        address[3],
                        address[4]
                    );

                    /* Set channel for our ESB controller. */
                    this->esbController->setChannel(channel);

                    /* Enable acks if required. */
                    if (query.mustShowAcks()) {
                        this->esbController->enableAcknowledgementsSniffing();
                    }
                    else {
                        this->esbController->disableAcknowledgementsSniffing();
                    }

                    /* Success. */
                    { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
                }
                else
                {
                    /* Parameter error (invalid address size). */
                    { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
                }
            }
            else
            {
                /* Parameter error (invalid channel). */
                { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
            }
        }
        break;

        /* Start message. */
        case whad::unifying::StartMsg:
        {
            /* Start controller in current mode. */
            this->esbController->start();

            /* Success. */
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        /* Stop message. */
        case whad::unifying::StopMsg:
        {
            /* Stop controller. */
            this->esbController->stop();

            /* Success. */
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        /* Send raw packet message. */
        case whad::unifying::SendRawMsg:
        {
            /* Wrap our Unifying message into a SendRawPdu message. */
            whad::unifying::SendRawPdu query(uniMsg);

            /* Check channel. */
            int channel = query.getChannel();
            if (channel >= 0 && channel <= 100) {

                /* Set controller channel. */
                this->esbController->setChannel(channel);

                /* Send packet. */
                if (this->esbController->send(query.getPdu().getBytes(), query.getPdu().getSize(), query.getRetrCounter()))
                {
                    /* Success. */
                    { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
                }
                else
                {
                    /* Error while sending packet. */
                    { whad::generic::Error _resp; this->pushMessageToQueue(&_resp); }
                }
            }
            else
            {
                /* Parameter error (invalid channel). */
                { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
            }
        }
        break;

        /* Set node address message. */
        case whad::unifying::SetNodeAddressMsg:
        {
            /* Wrap our Unifying message into a SetNodeAddress message. */
            whad::unifying::SetNodeAddress query(uniMsg);

            /* Check address. */
            addressLen = query.getAddress().getLength();
            if ((addressLen > 0) && (addressLen <= 5))
            {
                /* Copy address. */
                memcpy(address, query.getAddress().getBytes(), addressLen);

                /* Set controller address. */
                this->esbController->setFilter(
                    address[0],
                    address[1],
                    address[2],
                    address[3],
                    address[4]
                );

                /* Success. */
                { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
            }
            else
            {
                /* Parameter error (invalid address size). */
                { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
            }
        }
        break;

        /* Dongle mode message. */
        case whad::unifying::DongleModeMsg:
        {
            /* Wrap our Unifying message into a DongleMode message. */
            whad::unifying::DongleMode query(uniMsg);

            /* Check channel value. */
            int channel = query.getChannel();
            if (((channel >= 0) && (channel <= 100)) || (channel == 0xFF))
            {
                /* Configure our ESB controller accordingly. */
                this->esbController->setChannel(channel);
                this->esbController->disableAcknowledgementsSniffing();
                this->esbController->enableAcknowledgementsTransmission();

                /* Success. */
                { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
            }
            else
            {
                /* Parametter error (invalid channel). */
                { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
            }
        }
        break;

        /* Mouse mode message. */
        case whad::unifying::MouseModeMsg:
        {
            /* Wrap our Unifying message into a MouseMode message. */
            whad::unifying::MouseMode query(uniMsg);

            /* Check channel value. */
            int channel = query.getChannel();
            if (((channel >= 0) && (channel <= 100)) || (channel == 0xFF))
            {
                /* Configure our ESB controller accordingly. */
                this->esbController->setChannel(channel);
                this->esbController->enableAcknowledgementsSniffing();
                this->esbController->disableAcknowledgementsTransmission();

                /* Success. */
                { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
            }
            else
            {
                /* Parametter error (invalid channel). */
                { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
            }
        }
        break;

        /* Keyboard mode message. */
        case whad::unifying::KeyboardModeMsg:
        {
            /* Wrap our Unifying message into a KeyboardMode message. */
            whad::unifying::KeyboardMode query(uniMsg);

            /* Check channel value. */
            int channel = query.getChannel();
            if (((channel >= 0) && (channel <= 100)) || (channel == 0xFF))
            {
                /* Configure our ESB controller accordingly. */
                this->esbController->setChannel(channel);
                this->esbController->enableAcknowledgementsSniffing();
                this->esbController->disableAcknowledgementsTransmission();

                /* Success. */
                { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
            }
            else
            {
                /* Parametter error (invalid channel). */
                { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
            }
        }
        break;

        /* Pairing sniffing mode. */
        case whad::unifying::SniffPairingMsg:
        {
            /*
             * Put our ESB controller in sniffing mode in order to sniff
             * Logitech Unifying pairing requests on channel 5.
             */
            this->esbController->setFilter(0xBB, 0x0A, 0xDC, 0xA5, 0x75);
            this->esbController->setChannel(5);

            /* Success. */
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        /* Unkown message. */
        case whad::unifying::UnknownMsg:
        default:
        {
            /* Unkown message error. */
            { whad::generic::Error _resp; this->pushMessageToQueue(&_resp); }
        }
        break;
    }

}

void Core::processPhyInputMessage(whad::phy::PhyMsg msg) {


    if (this->currentController != this->genericController) {
    this->selectController(GENERIC_PROTOCOL);
    }

    switch (msg.getType())
    {
        case whad::phy::SetGfskModMsg:
        {
            whad::phy::SetGfskMod query(msg);

            switch (query.getDeviation())
            {
                case 170000:
                    {
                        this->genericController->setPhy(GENERIC_PHY_1MBPS_ESB);
                        { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
                    }
                    break;

                case 250000:
                    {
                        { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
                        this->genericController->setPhy(GENERIC_PHY_1MBPS_BLE);
                    }
                    break;

                case 320000:
                    {
                        this->genericController->setPhy(GENERIC_PHY_2MBPS_ESB);
                        { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
                    }
                    break;

                case 500000:
                    {
                        this->genericController->setPhy(GENERIC_PHY_2MBPS_BLE);
                        { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
                    }
                    break;

                default:
                    /* Error, deviation is not supported. */
                    { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
                    break;

            }
        }
        break;

        case whad::phy::SendMsg:
        {
            whad::phy::SendPacket query(msg);
            whad::phy::Packet packet = query.getPacket();
            this->genericController->send(packet.getBytes(), packet.getSize());
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        case whad::phy::GetSupportedFreqsMsg:
        {
            whad::phy::SupportedFreqsResp resp(SUPPORTED_FREQUENCY_RANGES);
            this->pushMessageToQueue(&resp);
        }
        break;

        case whad::phy::SetFreqMsg:
        {
            whad::phy::SetFreq query(msg);

            uint64_t frequency = query.getFrequency();
            if (frequency >= 2400000000L && frequency <= 2500000000L) {
                int frequency_offset = (frequency / 1000000) - 2400;
                this->genericController->setChannel(frequency_offset);
                { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
            }
            else {
                { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
            }
        }
        break;

        case whad::phy::SetDatarateMsg:
        {
            whad::phy::SetDatarate query(msg);

            switch (query.getDatarate())
            {
                case 1000000:
                    {
                        if (
                            this->genericController->getPhy() == GENERIC_PHY_1MBPS_ESB ||
                            this->genericController->getPhy() == GENERIC_PHY_2MBPS_ESB
                        ) {
                            this->genericController->setPhy(GENERIC_PHY_1MBPS_ESB);
                            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
                        }
                        else if (
                            this->genericController->getPhy() == GENERIC_PHY_1MBPS_BLE ||
                            this->genericController->getPhy() == GENERIC_PHY_2MBPS_BLE
                        ) {
                            this->genericController->setPhy(GENERIC_PHY_1MBPS_BLE);
                            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
                        }
                        else {
                            { whad::generic::Error _resp; this->pushMessageToQueue(&_resp); }
                        }
                    }
                    break;

                case 2000000:
                    {
                        if (
                            this->genericController->getPhy() == GENERIC_PHY_1MBPS_ESB ||
                            this->genericController->getPhy() == GENERIC_PHY_2MBPS_ESB
                        ) {
                            this->genericController->setPhy(GENERIC_PHY_2MBPS_ESB);
                            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
                        }
                        else if (
                            this->genericController->getPhy() == GENERIC_PHY_1MBPS_BLE ||
                            this->genericController->getPhy() == GENERIC_PHY_2MBPS_BLE
                        ) {
                            this->genericController->setPhy(GENERIC_PHY_2MBPS_BLE);
                            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
                        }
                        else {
                            { whad::generic::Error _resp; this->pushMessageToQueue(&_resp); }
                        }
                    }
                    break;

                default:
                    { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
                    break;
            }
        }
        break;

        case whad::phy::SetEndiannessMsg:
        {
            whad::phy::SetEndianness query(msg);
            if (query.getEndianness() == whad::phy::PhyBigEndian) {
                this->genericController->setEndianness(GENERIC_ENDIANNESS_BIG);
            }
            else {
                this->genericController->setEndianness(GENERIC_ENDIANNESS_LITTLE);
            }
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        case whad::phy::SetTxPowerMsg:
        {
            whad::phy::SetTxPower query(msg);

            if (query.getPower() == whad::phy::PhyTxPowerLow) {
                this->genericController->setTxPower(LOW);
            }
            else if (query.getPower() == whad::phy::PhyTxPowerMedium) {
                this->genericController->setTxPower(MEDIUM);
            }
            else {
                this->genericController->setTxPower(HIGH);
            }
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        case whad::phy::SetPacketSizeMsg:
        {
            whad::phy::SetPacketSize query(msg);

            /*
             * WHAD protocol (protobuf) sets packet's payload maximum size to 255,
             * meaning we cannot accept a packet size that would cause more data to
             * be written into a received packet's buffer.
             *
             * Packet's payload is also expected to contain the configured synchronization
             * word (that's a bit counter-intuitive no?), thus consuming bytes that are not
             * available for payload. We need to take this into account to validate the
             * requested payload size in order not to exceed the 255 bytes limit.
             *
             * For now, we are returning a parameter error to notify host that the supplied
             * parameter is invalid, but will configure the packet size to its maximum
             * value anyway (we cannot report the expected maximum size to host, so the least
             * we can do is to accept the maximum size closest to the required value and let
             * host handle the error we report).
             */

            if (query.getSize() + this->genericController->getPreambleSize()  <= 255) {
                /* Supplied size is correct, update packet size and return a success message. */
                this->genericController->setPacketSize(query.getSize());
                { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
            }
            else {
                /* Update packet size to its maximum value, based on current synchronization word. */
                this->genericController->setPacketSize(255 - this->genericController->getPreambleSize());

                /* Notify host something went wrong because of an invalid supplied parameter. */
                { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
            }
        }
        break;

        case whad::phy::SetSyncWordMsg:
        {
            whad::phy::SetSyncWord query(msg);

            /*
             * We need to make sure the supplied synchronization word may not interfere with the current
             * packet size set. If the new synchronization word size + packet size exceeds 255 bytes, we
             * simply reduce the packet size in order to make everything fit in the 255-byte buffer.
             */
          
            /* Make sure syncword is at most 8-byte long. */
            if (query.get().getSize() <= 8)
            {
                /* Update synchronization word. */
                this->genericController->setPreamble(query.get().get(), query.get().getSize());

                /* Update packet size if needed and notify host if we had to do so. */
                if (this->genericController->getPacketSize() + query.get().getSize() > 255)
                {
                    /* Update packet size to fit the buffer's maximum size. */
                    this->genericController->setPacketSize(255 - query.get().getSize());


                    /* Notify host that something went wrong. */
                    { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
                }
                else
                {
                    /* Everything is good, return success. */
                    { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
                }
            }
            else
            {
                /* Supplied synchronization word is too long. */
                { whad::generic::ParameterError _resp; this->pushMessageToQueue(&_resp); }
            }
        }
        break;

        case whad::phy::SetSniffModeMsg:
        {
            whad::phy::SniffMode query(msg);

            if (query.isIqModeEnabled()) {
                { whad::generic::Error _resp; this->pushMessageToQueue(&_resp); }
            }
            else {
                { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
            }
        }
        break;

        case whad::phy::StartMsg:
        {
            this->genericController->start();
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        case whad::phy::StopMsg:
        {
            this->genericController->stop();
            { whad::generic::Success _resp; this->pushMessageToQueue(&_resp); }
        }
        break;

        default:
        {
            { whad::generic::Error _resp; this->pushMessageToQueue(&_resp); }
        }
        break;
    }

}

#ifdef PA_ENABLED
void Core::configurePowerAmplifier(bool enabled) {
	NRF_P1->PIN_CNF[11] = (GPIO_PIN_CNF_SENSE_Disabled << GPIO_PIN_CNF_SENSE_Pos)
	                                    | (GPIO_PIN_CNF_DRIVE_S0S1 << GPIO_PIN_CNF_DRIVE_Pos)
	                                    | (GPIO_PIN_CNF_PULL_Pulldown << GPIO_PIN_CNF_PULL_Pos)
	                                    | (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos)
	                                    | (GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos);
	NRF_P1->PIN_CNF[12] = (GPIO_PIN_CNF_SENSE_Disabled << GPIO_PIN_CNF_SENSE_Pos)
	                                    | (GPIO_PIN_CNF_DRIVE_S0S1 << GPIO_PIN_CNF_DRIVE_Pos)
	                                    | (GPIO_PIN_CNF_PULL_Pulldown << GPIO_PIN_CNF_PULL_Pos)
	                                    | (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos)
	                                    | (GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos);
	if (enabled) {
		NRF_P1->OUTSET =   (1ul << 11) | (1ul << 12);
	}
	else {
		NRF_P1->OUTCLR =   (1ul << 11) | (1ul << 12);
	}
}

#endif

void core_send_bytes(uint8_t *p_bytes, int size)
{
    if (Core::instance != NULL)
    {
        //Core::instance->getLedModule()->off(LED2);
        Core::instance->getSerialModule()->send(p_bytes, size);
    }
    else
    {
        whad_transport_data_sent();
    }
}

Core::Core(runtime_mode_t mode) {
	instance = this;
	m_runtimeMode = mode;

	/* Common services — always constructed. */
	this->ledModule = new LedModule();
	this->displayModule = new DisplayModule();

	/* Raw-only services — constructed BEFORE SerialComm so that
	 * TimerModule's manual HFXO start happens before the clock
	 * driver initializes (in SerialComm::init), keeping the
	 * driver's internal hfclk state consistent with hardware.
	 * BLE-HID leaves these NULL behind checked accessors. */
	if (mode == RUNTIME_RAW_WHAD) {
		this->timerModule = new TimerModule();
		this->sequenceModule = new SequenceModule();
		this->radio = new Radio();
	} else {
		this->timerModule = NULL;
		this->sequenceModule = NULL;
		this->radio = NULL;
	}

	/* SerialComm starts the USB CDC stack from its constructor.
	 * Must come after timer/radio so no USB interrupts can fire
	 * while those services are mid-construction. */
	this->serialModule = new SerialComm();

	/* Raw controllers — left NULL until init() in BLE mode. */
	this->bleController = NULL;
	this->dot15d4Controller = NULL;
	this->esbController = NULL;
	this->antController = NULL;
	this->mosartController = NULL;
	this->genericController = NULL;
	this->currentController = NULL;

#ifdef BOARD_CLUE
 	this->boardModule = new BoardModule(this);
 	this->menuManager = NULL;
 	this->m_bleRuntime = NULL;
#endif

    /* Initialize WHAD library. */
    memset(&this->transportConfig, 0, sizeof(whad_transport_cfg_t));
    this->transportConfig.max_txbuf_size = 64;
    this->transportConfig.pfn_data_send_buffer = core_send_bytes;
    whad_init(&this->transportConfig);

	#ifdef PA_ENABLED
	this->configurePowerAmplifier(true);
	#endif

  /*
  //this->linkModule->configureLink(LINK_SLAVE);


  this->linkModule->configureLink(LINK_MASTER);

  while (true) {
    this->linkModule->sendSignalToSlave(1);
    nrf_delay_ms(1000);
    this->linkModule->sendSignalToSlave(2);
    nrf_delay_ms(1000);

  }
  */
}

LedModule* Core::getLedModule() {
	return (this->ledModule);
}

DisplayModule* Core::getDisplayModule() {
	return (this->displayModule);
}

SerialComm *Core::getSerialModule() {
	return (this->serialModule);
}

SequenceModule* Core::getSequenceModule() {
	return (this->sequenceModule);
}

TimerModule* Core::getTimerModule() {
	return (this->timerModule);
}

Radio* Core::getRadioModule() {
	return (this->radio);
}

runtime_mode_t Core::getRuntimeMode(void) const {
	return m_runtimeMode;
}

bool Core::hasRawRadio(void) const {
	return (m_runtimeMode == RUNTIME_RAW_WHAD && this->radio != NULL);
}

void Core::setControllerChannel(int channel) {
	if (this->currentController == this->bleController) {
    this->bleController->setChannel(channel);
  }
}
void Core::init() {

	messagePoolReset();
	this->messageQueue.size = 0;
	this->messageQueue.firstElement = NULL;
	this->messageQueue.lastElement = NULL;

#ifdef BOARD_CLUE
	this->displayModule->init();
	this->displayModule->drawText(4, 4, "BUTTERFLY", COLOR_CYAN, COLOR_BLACK);
 	this->displayModule->drawText(4, 16, "v1.2.0", COLOR_GRAY, COLOR_BLACK);
	this->displayModule->drawText(4, 32, "IDLE", COLOR_WHITE, COLOR_BLACK);
	this->displayModule->endBootSplash();
 	nrf_gpio_cfg_input(BSP_BUTTON_0, BUTTON_PULL);
	this->boardModule->initHardware();
#endif

	/* USB CDC stack is started from the SerialComm constructor (called
	 * during Core construction above). Do NOT re-init here — double
	 * initialization corrupts the USBD driver state. */

	/* Raw controllers — created ONLY in raw-WHAD runtime.
	 * BLE-HID never constructs these; BLE stack owns the radio. */
	if (m_runtimeMode == RUNTIME_RAW_WHAD) {
		this->bleController = new BLEController(this->getRadioModule());
		this->dot15d4Controller = new Dot15d4Controller(this->getRadioModule());
		this->esbController = new ESBController(this->getRadioModule());
		this->antController = new ANTController(this->getRadioModule());
		this->mosartController = new MosartController(this->getRadioModule());
		this->genericController = new GenericController(this->getRadioModule());
		this->radio->setController(NULL);
	}

	this->currentController = NULL;
}

bool Core::selectController(Protocol controller) {
  if (!this->hasRawRadio()) {
    return false;
  }
  //this->getLedModule()->on(LED2);
	const char *protoName = NULL;
	if (controller == BLE_PROTOCOL) {
    this->getLedModule()->setColor(BLUE);
		protoName = "BLE";
		this->radio->disable();
		this->currentController = this->bleController;
		this->radio->setController(this->currentController);
	}
	else if (controller == DOT15D4_PROTOCOL) {
    this->getLedModule()->setColor(GREEN);
		protoName = "802.15.4";
		this->radio->disable();
		this->currentController = this->dot15d4Controller;
		this->radio->setController(this->currentController);
	}
	else if (controller == ESB_PROTOCOL) {
    this->getLedModule()->setColor(PURPLE);
		protoName = "ESB";
		this->radio->disable();
		this->currentController = this->esbController;
		this->radio->setController(this->currentController);
	}
	else if (controller == ANT_PROTOCOL) {
    this->getLedModule()->setColor(RED);
		protoName = "ANT";
		this->radio->disable();
		this->currentController = this->antController;
		this->radio->setController(this->currentController);
	}
	else if (controller == MOSART_PROTOCOL) {
    this->getLedModule()->setColor(YELLOW);
		protoName = "MOSART";
		this->radio->disable();
		this->currentController = this->mosartController;
		this->radio->setController(this->currentController);
	}
	else if (controller == GENERIC_PROTOCOL) {
    this->getLedModule()->setColor(CYAN);
		protoName = "PHY";
		this->radio->disable();
		this->currentController = this->genericController;
		this->radio->setController(this->currentController);
	}
	else {
    //this->getLedModule()->off(LED2);
		protoName = "IDLE";
		this->radio->disable();
		this->currentController = NULL;
		this->radio->setController(NULL);
	}

#ifdef BOARD_CLUE
	if (protoName) {
		this->displayModule->fillRect(4, 32, 80, 8, COLOR_BLACK);
		this->displayModule->drawText(4, 32, protoName, COLOR_WHITE, COLOR_BLACK);
	}
#else
	(void)protoName;
#endif

	return (this->currentController != NULL);
}

void Core::sendDebug(const char *message) {
}

void Core::sendDebug(uint8_t *buffer, uint8_t size) {
	//this->pushMessageToQueue(new DebugNotification(buffer,size));
}

MessagePoolStatus Core::pushMessageToQueue(whad::NanoPbMsg *msg, MessagePoolTrafficClass trafficClass, uint32_t sourceTimestamp) {
	if (msg == NULL) {
		messagePoolRecordOverflow();
		return MESSAGE_POOL_EXHAUSTED;
	}
	Message *raw = msg->getRaw();
	msg->disown();
	return this->pushMessageToQueue(raw, trafficClass, sourceTimestamp);
}

MessagePoolStatus Core::pushMessageToQueue(Message *msg, MessagePoolTrafficClass trafficClass, uint32_t sourceTimestamp) {
	if (msg == NULL) {
		messagePoolRecordOverflow();
		return MESSAGE_POOL_EXHAUSTED;
	}
	MessageQueueElement *element = messagePoolAllocateQueueNode(trafficClass, NULL);
	if (element == NULL) {
		(void)messagePoolReleaseMessage(msg);
		return MESSAGE_POOL_EXHAUSTED;
	}
	element->message = msg;
	element->nextElement = NULL;
	element->sourceTimestamp = sourceTimestamp;
	if (this->messageQueue.size == 0) {
		this->messageQueue.firstElement = element;
		this->messageQueue.lastElement = element;
	}
	else {
		/* We insert the message at the end of the queue. */
		this->messageQueue.lastElement->nextElement = element;
		this->messageQueue.lastElement = element;
	}
	this->messageQueue.size = this->messageQueue.size + 1;
	return MESSAGE_POOL_OK;
}

Message* Core::popMessageFromQueue() {
	if (this->messageQueue.size == 0) return NULL;
	else {
		MessageQueueElement* element = this->messageQueue.firstElement;
		Message* msg = element->message;
		this->messageQueue.firstElement = element->nextElement;
		this->messageQueue.size = this->messageQueue.size - 1;
		if (this->messageQueue.size == 0) {
			this->messageQueue.lastElement = NULL;
		}
		(void)messagePoolReleaseQueueNode(element);
		return msg;
	}
}

void Core::sendVerbose(const char* data) {
  std::string message(data);
  whad::generic::Verbose verbMsg(message);
  this->pushMessageToQueue(&verbMsg, MESSAGE_POOL_TRAFFIC_STREAM_EVENT, 0);
}

void Core::loop() {
    Message *message = this->popMessageFromQueue();

    /* === DIAGNOSTIC BUILD (temporary) ===
     * Loop simplified to match known-good exactly to isolate why
     * WHAD responses never reach the host. BOARD_CLUE per-iteration
     * work (button sampling, menu tick, stream emit) is disabled.
     * LED_1 (red P1.01) lights when whad_get_message succeeds.
     * LED_2 (white P0.10) lights when whad_send_message is invoked.
     * Original behavior can be restored from git history. */
    bool rxEverSeen = false;
    bool txEverSent = false;

 	while (true) {

		this->serialModule->process();

#ifdef BOARD_CLUE
		if (this->boardModule != NULL) {
			this->boardModule->tick();
		}
		this->displayModule->flushDirty((uint32_t)timebase_now_ms(), DISPLAY_DEFAULT_QUANTUM, false);
#endif

        /* Check if we receveived a WHAD message. */
        if (whad_get_message(&msg) == WHAD_SUCCESS)
        {
            if (!rxEverSeen) {
                this->getLedModule()->on(LED1);
                rxEverSeen = true;
            }
            this->processInputMessage(msg);
        }
        if (message != NULL) {
          if (!txEverSent) {
              this->getLedModule()->on(LED2);
              txEverSent = true;
          }
          if (whad_send_message(message) == WHAD_ERROR)
          {
          }
          (void)messagePoolReleaseMessage(message);
          message = this->popMessageFromQueue();
        }
        else {
          message = this->popMessageFromQueue();
        }
    }
}
