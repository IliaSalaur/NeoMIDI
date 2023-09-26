#ifndef USB_MIDI_HPP
#define USB_MIDI_HPP

#include <cstdint>
#include <array>
#include <usb/usb_host.h>
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "sdkconfig.h"

static constexpr size_t k_host_event_timeout = 1; // ticks
static constexpr size_t k_client_event_timeout = 1; // ticks
static constexpr size_t k_midi_event_queue_size = 50;
static constexpr size_t k_transfers_count = 1;

#define USB_MIDI_ENABLE_LOGS

#ifdef USB_MIDI_ENABLE_LOGS

static const char _midiTag[] = "midi_host";
#include "esp_log.h"
#define USBMIDI_LOG(x...) ESP_LOGI(_midiTag, x);

#else
#define USBMIDI_LOG(x...) {}
#endif

namespace MIDI
{
    enum class StatusEnum : uint8_t
    {
        NOTE_ON = 0x9,
        CONTROL_CHANGE = 0xB,
    };

    enum class ControlChangeEnum : uint8_t
    {
        SUSTAIN_PEDAL = 0x40,
        PORTAMENTO = 0x41,
        SOSTENUTO_PEDAL = 0x42,
        SOFT_PEDAL = 0x43
        // To be continued with other ControlChange messages
    };

    struct midi_event_t{
        StatusEnum statusEnum;
        // channel from 0 to 15 | 0x0 -> 0xf
        uint8_t channel;
        union{
            // Status: NOTE ON/OFF
            struct{
                uint8_t note;
                uint8_t velocity;
            };

            // Status: CONTROL_CHANGE
            struct{
                ControlChangeEnum controlChangeEnum;
                uint8_t data;
            };
            uint8_t val[2];
        };
    };
};

class USBMidi
{
private:
    static usb_host_client_handle_t m_clientHandle;
    static usb_device_handle_t m_deviceHandle;
    static bool m_isMIDI;
    static bool m_isMIDIReady;
    static std::array<usb_transfer_t*, k_transfers_count> m_bufMIDIin;
    static usb_transfer_t* m_bufMIDIOut;
    static QueueHandle_t m_midi_event_queue;
    static SemaphoreHandle_t m_midi_semphr;

    static void _midi_transfer_cb(usb_transfer_t* transfer)
    {
        xSemaphoreGive(m_midi_semphr);
        if(m_deviceHandle != transfer->device_handle)
        {
            USBMIDI_LOG("Different device handlers")
            return;
        }

        int in_xfer = transfer->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK;

        if(!(transfer->status == 0 && in_xfer))
        {
            USBMIDI_LOG("transfer status is %d", transfer->status)
            return;
        }

        // read all bytes
        uint8_t *const p = transfer->data_buffer;
        for(size_t i{0}; i < transfer->actual_num_bytes; i += 4)
        {
            // skip if buffer is empty or unnecessary bytes were transmitted
            if ((p[i] + p[i+1] + p[i+2] + p[i+3]) == 0 || p[i+1] == 0xf8 || p[i+1] == 0xfe)
                continue;

            USBMIDI_LOG("midi: %02x %02x %02x",
            p[i+1], p[i+2], p[i+3]);

            MIDI::midi_event_t midi_event
            {
                MIDI::StatusEnum(p[i + 1] >> 4),  // status    bbbb 0000
                uint8_t(p[i + 1] & 0xf),          // channel   0000 bbbb
                {p[i + 2], p[i + 3]}              // val -> data 1 & data 2
            };

            xQueueSend(m_midi_event_queue, (void*)&midi_event, 1);

        }

        // USBMIDI_LOG("awaiting semphr");
        xSemaphoreTake(m_midi_semphr, portMAX_DELAY);
        // USBMIDI_LOG("semphr taken");
        // submit a new transfer
        esp_err_t ret = usb_host_transfer_submit(transfer);
        if (ret != ESP_OK)
            USBMIDI_LOG("usb_host_transfer_submit In fail: %d", ret);

        
        // USBMIDI_LOG("semphr given");
    }

    static void _claimInterace(const usb_intf_desc_t* intf_desc)
    {
        if(intf_desc->bInterfaceClass == USB_CLASS_AUDIO &&
            intf_desc->bInterfaceSubClass == 3 &&
            intf_desc->bInterfaceProtocol == 0)
        {
            esp_err_t ret = usb_host_interface_claim(
                m_clientHandle, 
                m_deviceHandle, 
                intf_desc->bInterfaceNumber,
                intf_desc->bAlternateSetting);
            
            if(ret != ESP_OK)
            {
                USBMIDI_LOG("usb_host_interface_claim failed: %d", ret);
                m_isMIDI = false;
                return;
            }

            USBMIDI_LOG("Interface claimed")
            m_isMIDI = true;
            return;
        }

        m_isMIDI = false;
    }

    static void _prepareEndpoints(const usb_ep_desc_t* ep_desc)
    {
        // check attributes
        if((ep_desc->bmAttributes & USB_BM_ATTRIBUTES_XFER_BULK) != USB_BM_ATTRIBUTES_XFER_BULK)
        {
            USBMIDI_LOG("Endpoint isn't bulk, bmAttrs: 0x%02x", ep_desc->bmAttributes)
            m_isMIDIReady = false;
            return;
        }

        if(ep_desc->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK)
        {
            for(auto& buf : m_bufMIDIin)
            {
                esp_err_t ret = usb_host_transfer_alloc(ep_desc->wMaxPacketSize, 0, &buf);
                if(ret != ESP_OK)
                {
                    buf = nullptr;
                    USBMIDI_LOG("usb_host_transfer_alloc IN failed: %d", ret);
                    continue;
                }

                buf->device_handle = m_deviceHandle;
                buf->bEndpointAddress = ep_desc->bEndpointAddress;
                buf->callback = USBMidi::_midi_transfer_cb; // !!! CALLBACK
                buf->context = nullptr;
                buf->num_bytes = ep_desc->wMaxPacketSize;

                ret = usb_host_transfer_submit(buf);
                if(ret != ESP_OK)
                    USBMIDI_LOG("usb_host_transfer_submit failed: %d", ret)

            }
        } else {
            esp_err_t ret = usb_host_transfer_alloc(ep_desc->wMaxPacketSize, 0, &m_bufMIDIOut);
            if(ret != ESP_OK)
            {
                m_bufMIDIOut = nullptr;
                USBMIDI_LOG("usb_host_transfer_alloc OUT failed: %d", ret);
                return;
            }

            m_bufMIDIOut->device_handle = m_deviceHandle;
            m_bufMIDIOut->bEndpointAddress = ep_desc->bEndpointAddress;
            m_bufMIDIOut->callback = USBMidi::_midi_transfer_cb;
            m_bufMIDIOut->context = nullptr;
        }

        // if both transfer bufs are initialized properly, we are ready to go
        m_isMIDIReady = m_bufMIDIOut && m_bufMIDIin[0];
        USBMIDI_LOG("MIDI is ready")
    }

    static void _handleDescriptor(const usb_config_desc_t* config_desc)
    {
        // using pointer to config_desc->val to run through all configs
        const uint8_t* p = config_desc->val;

        for(size_t i{0}, bLength{0}; i < config_desc->wTotalLength; i += bLength, p += bLength)
        {
            //
            bLength = *p;
            
            // validate descriptor
            if(i + bLength > config_desc->wTotalLength)
            {
                USBMIDI_LOG("Invalid USB Descriptor")
                return;
            }

            const uint8_t bDescriptorType = *(p + 1);
            switch(bDescriptorType)
            {
            case USB_B_DESCRIPTOR_TYPE_INTERFACE:
                USBMIDI_LOG("Descriptor type: interface");
                // claim interface if needed
                if(!m_isMIDI)
                {
                    _claimInterace((usb_intf_desc_t*)p);
                }

                break;
            
            case USB_B_DESCRIPTOR_TYPE_ENDPOINT:
                USBMIDI_LOG("Descriptor type: endpoint");
                _prepareEndpoints((usb_ep_desc_t*)p);
                break;
            
            case USB_B_DESCRIPTOR_TYPE_DEVICE ... USB_B_DESCRIPTOR_TYPE_STRING:
                // useless descriptors
                break;

            case USB_B_DESCRIPTOR_TYPE_DEVICE_QUALIFIER ... USB_B_DESCRIPTOR_TYPE_INTERFACE_POWER:
                // useless descriptors / should not be in config
                break;
            
            default:
                USBMIDI_LOG("Unknown USB Descriptor Type: 0x%x", bDescriptorType);
                break;
            }
        }
    }

    static void _client_event_cb(const usb_host_client_event_msg_t *event_msg, void* arg)
    {
        esp_err_t ret{ESP_OK};
        
        switch (event_msg->event)
        {
        case USB_HOST_CLIENT_EVENT_NEW_DEV:
            USBMIDI_LOG("New device, address: %u", event_msg->new_dev.address)            
            // Try opening the new device
            ret = usb_host_device_open(m_clientHandle, event_msg->new_dev.address, &m_deviceHandle);

            if(ret != ESP_OK)
                USBMIDI_LOG("usb_host_device_open failed: %d", ret)
            
            // Get device info
            usb_device_info_t dev_info;
            ret = usb_host_device_info(m_deviceHandle, &dev_info);
            if(ret != ESP_OK)
                USBMIDI_LOG("usb_host_device_info failed: %d", ret)
            
            USBMIDI_LOG("Device Info: \n\tspeed: %d \n\tdev_addr: %u \n\tvMaxPacketSize: %u \n\tbConfigurationValue: %u",
                int(dev_info.speed), dev_info.dev_addr, dev_info.bMaxPacketSize0, dev_info.bConfigurationValue)
            
            // Get device descriptor
            /* const usb_device_desc_t* dev_desc;
            ret = usb_host_get_device_descriptor(m_deviceHandle, &dev_desc);
            if(ret != ESP_OK)
                USBMIDI_LOG("usb_host_get_device_descriptor failed: %d", ret) */
            
            // Get configuration descriptor
            const usb_config_desc_t* config_desc;
            ret = usb_host_get_active_config_descriptor(m_deviceHandle, &config_desc);
            if(ret != ESP_OK)
                USBMIDI_LOG("usb_host_get_active_config_descriptor failed: %d", ret)
            
            _handleDescriptor(config_desc);
            break;

        case USB_HOST_CLIENT_EVENT_DEV_GONE:
            USBMIDI_LOG("Device gone, handle: %p", event_msg->dev_gone.dev_hdl)
            break;
        
        default:
            break;
        }
    }

    static void _usb_host_task(void* pvParams)
    {
        uint32_t event_flags;
        
        while(1)
        {
            esp_err_t ret = usb_host_lib_handle_events(k_host_event_timeout, &event_flags);
            if(ret == ESP_OK)
            {                   
                if(event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)
                USBMIDI_LOG("No more clients - all free")

                if(event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
                USBMIDI_LOG("No more clients - device free")
            }  
            else{
                if(ret != ESP_ERR_TIMEOUT)
                    USBMIDI_LOG("usb_host_lib_handle_events failed -> err:%d flags: %lu", ret, event_flags)
            }                                  

            ret = usb_host_client_handle_events(m_clientHandle, k_client_event_timeout);
            if(ret != ESP_OK && ret != ESP_ERR_TIMEOUT)
                USBMIDI_LOG("usb_host_client_handle_events failed -> err:%d", ret)

        }
    }

public:
    static esp_err_t install()
    {
        m_isMIDI = false;
        m_isMIDIReady = false;

        m_midi_event_queue = xQueueCreate(k_midi_event_queue_size, sizeof(MIDI::midi_event_t));
        m_midi_semphr = xSemaphoreCreateBinary();

        USBMIDI_LOG("Semphr created, initiating give")
        xSemaphoreGive(m_midi_semphr);
        USBMIDI_LOG("Semphr given")


        const usb_host_config_t config = {
            .intr_flags = ESP_INTR_FLAG_LEVEL1,
        };

        esp_err_t ret = usb_host_install(&config);
        USBMIDI_LOG("usb_host_install: %d - %s", ret, ret == ESP_OK ? "successfull" : "failed");
        return ret;
    }

    static esp_err_t registerClient()
    {
        const usb_host_client_config_t client_config = {
            .is_synchronous = false,
            .max_num_event_msg = 5,
            .async = {
                .client_event_callback = USBMidi::_client_event_cb,
                .callback_arg = m_clientHandle
            }
        };

        esp_err_t ret = usb_host_client_register(&client_config, &m_clientHandle);
        USBMIDI_LOG("usb_host_client_register: %d - %s", ret, ret == ESP_OK ? "successfull" : "failed");
        return ret;
    }

    static TaskHandle_t startUSBMidiTask()
    {        
        TaskHandle_t xTaskHandle{nullptr};
        xTaskCreatePinnedToCore(&USBMidi::_usb_host_task, "midi_usb_task", 4096, nullptr, 3, &xTaskHandle, 0);

        return xTaskHandle;
    }

    static void getEventFromQueue(MIDI::midi_event_t* midi_eventBuf, TickType_t xTicksToWait)
    {
        xQueueReceive(m_midi_event_queue, midi_eventBuf, xTicksToWait);
    }

    static BaseType_t takeSemphr(TickType_t xTicksToWait)
    {
        return xSemaphoreTake(m_midi_semphr, xTicksToWait);
    }

    static void giveSemphr()
    {
        xSemaphoreGive(m_midi_semphr);
    }
};

usb_host_client_handle_t USBMidi::m_clientHandle = nullptr;
usb_device_handle_t USBMidi::m_deviceHandle = nullptr;
bool USBMidi::m_isMIDI = false;
bool USBMidi::m_isMIDIReady = false;
std::array<usb_transfer_t*, k_transfers_count> USBMidi::m_bufMIDIin = {};
usb_transfer_t* USBMidi::m_bufMIDIOut = nullptr;
QueueHandle_t USBMidi::m_midi_event_queue = nullptr;
SemaphoreHandle_t USBMidi::m_midi_semphr = nullptr;

/*
    DONE * get the descriptors and check device type / class / whatever    
    DONE * claim the interface if all checks passed
    * alloc resources for usb_tranfer
    * initiate an usb_transfer 
    * pass midi messages to queues
*/
#endif