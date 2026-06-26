#include "host/usbh.h"
#include "class/hid/hid.h"
#include "class/hid/hid_host.h"
#include "drivers/xbone/XBOneAuthUSBListener.h"
#include "CRC32.h"
#include "peripheralmanager.h"
#include "usbhostmanager.h"

#include "drivers/xbone/XBOneDescriptors.h"
#include "drivers/shared/xgip_protocol.h"
#include "drivers/shared/xinput_host.h"

// Report Queue for big report sizes from dongle
#include <queue>

typedef struct {
	uint8_t report[XBONE_ENDPOINT_SIZE];
	uint16_t len;
} report_queue_t;

static std::queue<report_queue_t> report_queue;
static uint32_t lastReportQueue = 0;
#define REPORT_QUEUE_INTERVAL 15

void XBOneAuthUSBListener::setup() {
    mounted = false;
    xbone_dev_addr = 0;
    xbone_instance = 0;
    xboxOneAuthData = nullptr;
    incomingXGIP.reset();
    outgoingXGIP.reset();
    lastRetryTime = 0;
    while (!report_queue.empty()) report_queue.pop();
    lastReportQueue = 0;
    XBONE_DBG("setup complete");
}

void XBOneAuthUSBListener::setAuthData(XboxOneAuthData * authData ) {
    xboxOneAuthData = authData;
    xboxOneAuthData->dongle_ready = false;
}

void XBOneAuthUSBListener::process() {
    // Do nothing if auth data or dongle are not ready
    if ( mounted == false || xboxOneAuthData == nullptr) // do nothing if we have not mounted an xbox one dongle
        return;

    // Received a packet from the console (or Windows) to dongle
    if ( xboxOneAuthData->xboneState == GPAuthState::send_auth_console_to_dongle ) {
        uint8_t isChunked = ( xboxOneAuthData->consoleBuffer.length > GIP_MAX_CHUNK_SIZE );
        uint8_t needsAck = ( xboxOneAuthData->consoleBuffer.length > 2 );
        outgoingXGIP.reset();
        outgoingXGIP.setAttributes(xboxOneAuthData->consoleBuffer.type,
            xboxOneAuthData->consoleBuffer.sequence, 1, isChunked, needsAck);
        outgoingXGIP.setData(xboxOneAuthData->consoleBuffer.data, xboxOneAuthData->consoleBuffer.length);
        XBONE_DBG("XSX->dongle: cmd=0x%02X seq=%d len=%d",
            xboxOneAuthData->consoleBuffer.type,
            xboxOneAuthData->consoleBuffer.sequence,
            xboxOneAuthData->consoleBuffer.length);
        xboxOneAuthData->consoleBuffer.reset();
        xboxOneAuthData->xboneState = GPAuthState::wait_auth_console_to_dongle;
    }

    // Process waiting (always on first frame)
    if ( xboxOneAuthData->xboneState == GPAuthState::wait_auth_console_to_dongle) {
        queue_host_report(outgoingXGIP.generatePacket(), outgoingXGIP.getPacketLength());
        if ( outgoingXGIP.getChunked() == false || outgoingXGIP.endOfChunk() == true) {
            XBONE_DBG("XSX->dongle transfer complete");
            xboxOneAuthData->xboneState = GPAuthState::auth_idle_state;
        }
    }

    // Process the report queue
    process_report_queue();

}

void XBOneAuthUSBListener::xmount(uint8_t dev_addr, uint8_t instance, uint8_t controllerType, uint8_t subtype) {
    if ( controllerType == xinput_type_t::XBOXONE) {
        xbone_dev_addr = dev_addr;
        xbone_instance = instance;
        incomingXGIP.reset();
        outgoingXGIP.reset();
        lastRetryTime = 0;
        mounted = true;
        XBONE_DBG("mount: dev_addr=%d instance=%d type=%d", dev_addr, instance, controllerType);
    }
}

void XBOneAuthUSBListener::unmount(uint8_t dev_addr) {
    if ( dev_addr == xbone_dev_addr ) {
        XBONE_DBG("unmount: dev_addr=%d", dev_addr);
        mounted = false;
        while (!report_queue.empty()) report_queue.pop();
        incomingXGIP.reset();
        outgoingXGIP.reset();
        lastRetryTime = 0;
        if ( xboxOneAuthData != nullptr ) {
            xboxOneAuthData->dongle_ready = false; // not ready for auth if we unmounted
        }
    }
}

void XBOneAuthUSBListener::report_received(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len) {
    if ( mounted == false || xboxOneAuthData == nullptr || dev_addr != xbone_dev_addr || instance != xbone_instance ) {
        return;
    }

    incomingXGIP.parse(report, len);
    if ( incomingXGIP.validate() == false ) {
        // Invalid packet: reset and return, do NOT block with sleep_ms
        XBONE_DBG("invalid packet, dropping (len=%d)", len);
        incomingXGIP.reset();
        return;
    }

    XBONE_DBG("recv: cmd=0x%02X seq=%d len=%d chunked=%d ack=%d",
        incomingXGIP.getCommand(), incomingXGIP.getSequence(),
        len, incomingXGIP.getChunked(), incomingXGIP.ackRequired());

    // Setup an ack before we change anything about the incoming packet
    if ( incomingXGIP.ackRequired() == true ) {
        XBONE_DBG("sending ACK for cmd=0x%02X seq=%d",
            incomingXGIP.getCommand(), incomingXGIP.getSequence());
        queue_host_report((uint8_t*)incomingXGIP.generateAckPacket(), incomingXGIP.getPacketLength());
    }

    switch ( incomingXGIP.getCommand() ) {
        case GIP_ANNOUNCE:
            XBONE_DBG("GIP_ANNOUNCE received, requesting device descriptor");
            outgoingXGIP.reset();
            outgoingXGIP.setAttributes(GIP_DEVICE_DESCRIPTOR, 1, 1, false, 0);
            queue_host_report((uint8_t*)outgoingXGIP.generatePacket(), outgoingXGIP.getPacketLength());
            break;
        case GIP_DEVICE_DESCRIPTOR:
            if ( incomingXGIP.endOfChunk() == true && xboxOneAuthData->dongle_ready != true) {
                XBONE_DBG("device descriptor complete, starting initialization");

                outgoingXGIP.reset();  // Power-on full string
                outgoingXGIP.setAttributes(GIP_POWER_MODE_DEVICE_CONFIG, 2, 1, false, 0);
                outgoingXGIP.setData(XBOXONE_POWER_ON, sizeof(XBOXONE_POWER_ON));
                queue_host_report((uint8_t*)outgoingXGIP.generatePacket(), outgoingXGIP.getPacketLength());
                XBONE_DBG("sent: Power-on full (cmd=0x05 seq=2)");

                outgoingXGIP.reset();  // Power-on with 0x00
                outgoingXGIP.setAttributes(GIP_POWER_MODE_DEVICE_CONFIG, 3, 1, false, 0);
                outgoingXGIP.setData(XBOXONE_POWER_ON_SINGLE, sizeof(XBOXONE_POWER_ON_SINGLE));
                queue_host_report((uint8_t*)outgoingXGIP.generatePacket(), outgoingXGIP.getPacketLength());
                XBONE_DBG("sent: Power-on single (cmd=0x05 seq=3)");

                outgoingXGIP.reset();  // LED On (internal=1 for genuine controller)
                outgoingXGIP.setAttributes(GIP_CMD_LED_ON, 1, 1, false, 0); // internal=1
                outgoingXGIP.setData(XBOXONE_LED_ON, sizeof(XBOXONE_LED_ON));
                queue_host_report((uint8_t*)outgoingXGIP.generatePacket(), outgoingXGIP.getPacketLength());
                XBONE_DBG("sent: LED on (cmd=0x0A seq=1 internal=1)");

                outgoingXGIP.reset();  // Rumble init via POWER_MODE_DEVICE_CONFIG (internal=1)
                outgoingXGIP.setAttributes(GIP_POWER_MODE_DEVICE_CONFIG, 1, 1, false, 0); // cmd=0x05, internal=1
                outgoingXGIP.setData(XBOXONE_RUMBLE_ON, sizeof(XBOXONE_RUMBLE_ON));
                queue_host_report((uint8_t*)outgoingXGIP.generatePacket(), outgoingXGIP.getPacketLength());
                XBONE_DBG("sent: Rumble init (cmd=0x05 seq=1 internal=1)");

                // Dongle is ready!
                xboxOneAuthData->dongle_ready = true; // dongle is ready
                XBONE_DBG("dongle_ready = true");
            } else if ( incomingXGIP.getChunked() == true && incomingXGIP.endOfChunk() == false ) {
                XBONE_DBG("device descriptor chunk continuing");
            }
            break;
        case GIP_AUTH:
        case GIP_FINAL_AUTH:
            if ( incomingXGIP.getChunked() == false ||
                (incomingXGIP.getChunked() == true && incomingXGIP.endOfChunk() == true )) {
                XBONE_DBG("dongle auth response: cmd=0x%02X seq=%d len=%d",
                    incomingXGIP.getCommand(), incomingXGIP.getSequence(),
                    incomingXGIP.getDataLength());
                xboxOneAuthData->dongleBuffer.setBuffer(incomingXGIP.getData(), incomingXGIP.getDataLength(),
                    incomingXGIP.getSequence(), incomingXGIP.getCommand());
                xboxOneAuthData->xboneState = GPAuthState::send_auth_dongle_to_console;
                XBONE_DBG("dongle->XSX: state set to send_auth_dongle_to_console");
                incomingXGIP.reset();
            } else {
                XBONE_DBG("auth chunk continuing");
            }
            break;
        case GIP_ACK_RESPONSE:
            XBONE_DBG("ACK received: seq=%d", incomingXGIP.getSequence());
            break;
        default:
            XBONE_DBG("unhandled cmd=0x%02X", incomingXGIP.getCommand());
            break;
    };
}

void XBOneAuthUSBListener::queue_host_report(void* report, uint16_t len) {
    if ( len > XBONE_ENDPOINT_SIZE ) {
        XBONE_DBG("WARNING: report len %d exceeds XBONE_ENDPOINT_SIZE %d, truncating", len, XBONE_ENDPOINT_SIZE);
        len = XBONE_ENDPOINT_SIZE;
    }
    report_queue_t new_queue;
    memcpy(new_queue.report, report, len);
    new_queue.len = len;
    report_queue.push(new_queue);
}

void XBOneAuthUSBListener::process_report_queue() {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if ( mounted == true && !report_queue.empty() && (now - lastReportQueue) > REPORT_QUEUE_INTERVAL  ) {
        if ( tuh_xinput_send_report(xbone_dev_addr, xbone_instance, report_queue.front().report, report_queue.front().len) ) {
			report_queue.pop();
            lastReportQueue = now; // last time we checked report queue
            lastRetryTime = 0;    // reset retry timer on success
        } else {
            // Non-blocking retry: keep item in queue, will retry on next process() call
            // Do NOT sleep_ms here; it would block USB callbacks and input processing
            if ( lastRetryTime == 0 ) {
                lastRetryTime = now;
            }
            XBONE_DBG("send failed, will retry (queued %dms ago)", now - lastRetryTime);
            lastReportQueue = now; // prevent tight-loop retries
        }
	}
}
