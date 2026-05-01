#pragma once
#include "SinglePortModule.h"
#include "concurrency/OSThread.h"
#include "configuration.h"

class RemoteSwitchReceiverModule : public SinglePortModule, private concurrency::OSThread
{
  public:
    RemoteSwitchReceiverModule()
        : SinglePortModule("remoteswitchrx", meshtastic_PortNum_DETECTION_SENSOR_APP), OSThread("RemoteSwitchRx")
    {
    }

  protected:
    virtual int32_t runOnce() override;
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

  private:
    struct PinTimer {
        uint8_t pin;
        uint32_t expiresAt;
        bool active;
    };

    PinTimer timers[4] = {};

    void evaluateRules(uint32_t fromNode, const char *payload);
    void activatePin(uint8_t pin, uint32_t timeoutMs);
};

extern RemoteSwitchReceiverModule *remoteSwitchReceiverModule;
