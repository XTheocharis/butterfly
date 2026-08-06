#include "serial.h"
#include "bsp.h"
#include <whad.h>

#ifdef BOARD_CLUE
#include "platformRuntime.h"
#include "runtime.h"
/* CDC ACM internal header for DTR workaround (kernels without TIOCMBIS). */
#include "app_usbd_cdc_acm_internal.h"
#endif

uint8_t tmp_buf[64];

SerialComm* SerialComm::instance = NULL;

void SerialComm::cdcAcmHandler(app_usbd_class_inst_t const * p_inst, app_usbd_cdc_acm_user_event_t event) {

	switch (event)
	{
		case APP_USBD_CDC_ACM_USER_EVT_PORT_OPEN:
		{
			instance->m_portOpen = true;
            app_usbd_class_inst_t const * p_inst =
                app_usbd_cdc_acm_class_inst_get(&m_app_cdc_acm);
            if (p_inst != NULL && p_inst->p_data != NULL) {
                app_usbd_cdc_acm_ctx_t * p_ctx =
                    (app_usbd_cdc_acm_ctx_t *)
                    ((uint8_t *)p_inst->p_data + sizeof(app_usbd_class_data_t));
                p_ctx->line_state |= APP_USBD_CDC_ACM_LINE_STATE_DTR;
            }

            ret_code_t ret = app_usbd_cdc_acm_read_any(
                &m_app_cdc_acm,
                instance->rxBuffer,
                RX_BUFFER_SIZE
            );

			UNUSED_VARIABLE(ret);
		}
  	    break;

		case APP_USBD_CDC_ACM_USER_EVT_PORT_CLOSE:
			instance->m_portOpen = false;
#ifdef BOARD_CLUE
			/* BLE-HID must survive the host closing the serial port.
			 * Only raw-WHAD preserves the legacy CDC-close-reset. */
			if (runtime_cdc_close_should_reset()) {
				platform_runtime_system_reset();
			}
#else
			NVIC_SystemReset();
#endif
			break;

		case APP_USBD_CDC_ACM_USER_EVT_TX_DONE:
            ret_code_t ret;
			//SerialComm::instance->txState.done = true;
            //Core::instance->getLedModule()->off(LED2);
            SerialComm::instance->txInProgress = false;
            whad_transport_data_sent();

            /* Fetch up to RX_BUFFER_SIZE bytes from the internal buffer */
            ret = app_usbd_cdc_acm_read_any(
                &m_app_cdc_acm,
                instance->rxBuffer,
                RX_BUFFER_SIZE
            );
			UNUSED_VARIABLE(ret);

			break;

		case APP_USBD_CDC_ACM_USER_EVT_RX_DONE:
		{
            int size;
			ret_code_t ret;

            /* Exit if read operation did not read anything. */
            size = app_usbd_cdc_acm_rx_size(&m_app_cdc_acm);
            if (size > 0)
            {
#ifdef BOARD_CLUE
                for (int i = 0; i + 2 < size; i++) {
					if (instance->rxBuffer[i] == 'D' &&
						instance->rxBuffer[i+1] == 'F' &&
						instance->rxBuffer[i+2] == 'U')
					{
						(void)platform_runtime_power_gpregret_clr(0, 0xFF);
						(void)platform_runtime_power_gpregret_set(0, 0x57);
						platform_runtime_system_reset();
					}
				}
#endif
                /* Forward read data to WHAD library. */
                whad_transport_data_received(instance->rxBuffer, size);
            }

            /* Send pending data, if any. */
            if (!SerialComm::instance->txInProgress)
            {
                whad_transport_send_pending();
            }

            if (!SerialComm::instance->txInProgress)
            {
                ret = app_usbd_cdc_acm_read_any(
                    &m_app_cdc_acm,
                    instance->rxBuffer,
                    RX_BUFFER_SIZE
                );
            }

            UNUSED_VARIABLE(ret);
        }
        break;


        default:
            break;
    }
}

void  SerialComm::usbdHandler(app_usbd_event_type_t event)
{
	switch (event)
	{
		case APP_USBD_EVT_DRV_SUSPEND:
			break;
		case APP_USBD_EVT_DRV_RESUME:
			break;
		case APP_USBD_EVT_STARTED:
			break;
		case APP_USBD_EVT_STOPPED:
			app_usbd_disable();
			break;
		case APP_USBD_EVT_POWER_DETECTED:
			if (!nrf_drv_usbd_is_enabled())
			{
				app_usbd_enable();
			}
			break;
		case APP_USBD_EVT_POWER_REMOVED:
			app_usbd_stop();
			break;
		case APP_USBD_EVT_POWER_READY:
			app_usbd_start();
			break;
		default:
			break;
	}
}


SerialComm::SerialComm(void) {
    instance = this;
    this->rxState.index = 0;
    this->rxState.decoding = false;
    this->rxState.last_received_byte = 0x00;
    this->txState.size = 0;
    this->txState.waiting = false;
    this->txState.done = true;
    this->currentByte = 0x00;
    this->txInProgress = false;
    this->m_portOpen = false;
    this->init();
}

void SerialComm::init() {

	ret_code_t ret;

    /* Configure USBD event handler. */
	static app_usbd_config_t usbdConfig;
    usbdConfig.ev_state_proc = SerialComm::usbdHandler;

	ret = nrf_drv_clock_init();
	/* SDH or a prior call may have already initialized the clock driver. */
	if (ret != NRF_SUCCESS && ret != NRF_ERROR_MODULE_ALREADY_INITIALIZED)
		APP_ERROR_CHECK(ret);

	nrf_drv_clock_lfclk_request(NULL);
	while (!nrf_drv_clock_lfclk_is_running())
		;

	ret = app_timer_init();
	APP_ERROR_CHECK(ret);

	app_usbd_serial_num_generate();

	ret = app_usbd_init(&usbdConfig);
	APP_ERROR_CHECK(ret);

	app_usbd_class_inst_t const * classCdcAcm =
		app_usbd_cdc_acm_class_inst_get(&m_app_cdc_acm);
	ret = app_usbd_class_append(classCdcAcm);
	APP_ERROR_CHECK(ret);

	if (USBD_POWER_DETECTION)
	{
		ret = app_usbd_power_events_enable();
		APP_ERROR_CHECK(ret);
	}
	else
	{
		app_usbd_enable();
		app_usbd_start();
	}
}

bool SerialComm::send(uint8_t *buffer, size_t size) {
    ret_code_t ret;

    /* If CDC port is not open (host hasn't connected or enumeration
     * isn't complete), drop the packet instead of busy-waiting.
     * The original do-while loop here would deadlock during boot:
     * app_usbd_cdc_acm_write returns !SUCCESS when the port is
     * closed, blocking the main loop from calling
     * app_usbd_event_queue_process() — which prevents USB
     * enumeration from ever completing (error -110). */
    if (!m_portOpen) {
        return false;
    }

    do
    {
        ret = app_usbd_cdc_acm_write(&m_app_cdc_acm, buffer, size);
    } while (ret != NRF_SUCCESS);

    this->txInProgress = true;
    return true;
}

void SerialComm::process() {
    ret_code_t ret;

    /* Read awaiting data. */
    if (!this->txInProgress)
    {
        ret = app_usbd_cdc_acm_read_any(
            &m_app_cdc_acm,
            instance->rxBuffer,
            RX_BUFFER_SIZE
        );
    }

    whad_transport_send_pending();

    /*  Process events. */
    app_usbd_event_queue_process();
 		UNUSED_VARIABLE(ret);
}

