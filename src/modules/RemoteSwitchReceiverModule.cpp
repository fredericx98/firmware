#include "RemoteSwitchReceiverModule.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "configuration.h"
#include <string.h>

RemoteSwitchReceiverModule *remoteSwitchReceiverModule;

#define TIMER_CHECK_INTERVAL_MS 500

int32_t RemoteSwitchReceiverModule::runOnce()
{
    LOG_INFO("RemoteSwitchRx: runOnce has_rs=%d enabled=%d",
             (int)moduleConfig.has_remote_switch,
             (int)moduleConfig.remote_switch.enabled);
    if (!moduleConfig.has_remote_switch || !moduleConfig.remote_switch.enabled)
        return disable();

    uint32_t now = millis();
    const meshtastic_ModuleConfig_RemoteSwitchConfig &cfg = moduleConfig.remote_switch;

    const meshtastic_ModuleConfig_RemoteSwitchConfig_Rule *rules[4] = {
        &cfg.rule1, &cfg.rule2, &cfg.rule3, &cfg.rule4
    };

    for (int i = 0; i < 4; i++) {
        if (!rules[i]->enabled || rules[i]->pin == 0)
            continue;
        if (timers[i].active && now >= timers[i].expiresAt) {
            LOG_INFO("RemoteSwitchRx: rule%d timeout, pin %d LOW", i + 1, timers[i].pin);
            digitalWrite(timers[i].pin, LOW);
            timers[i].active = false;
        }
    }

    return TIMER_CHECK_INTERVAL_MS;
}

ProcessMessage RemoteSwitchReceiverModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    if (!moduleConfig.has_remote_switch || !moduleConfig.remote_switch.enabled)
        return ProcessMessage::CONTINUE;

    char payload[meshtastic_Constants_DATA_PAYLOAD_LEN + 1] = {};
    memcpy(payload, mp.decoded.payload.bytes, mp.decoded.payload.size);
    payload[mp.decoded.payload.size] = '\0';

    LOG_INFO("RemoteSwitchRx: from=0x%08x payload=\"%s\"", mp.from, payload);

    evaluateRules(mp.from, payload);

    return ProcessMessage::CONTINUE;
}

void RemoteSwitchReceiverModule::evaluateRules(uint32_t fromNode, const char *payload)
{
    const meshtastic_ModuleConfig_RemoteSwitchConfig &cfg = moduleConfig.remote_switch;

    const meshtastic_ModuleConfig_RemoteSwitchConfig_Rule *rules[4] = {
        &cfg.rule1, &cfg.rule2, &cfg.rule3, &cfg.rule4
    };

    for (int i = 0; i < 4; i++) {
        const auto *r = rules[i];

        if (!r->enabled || r->pin == 0)
            continue;

        // Filter by source node (0 = any)
        if (r->source_node != 0 && r->source_node != fromNode)
            continue;

        // Filter by trigger text (empty = any)
        if (strlen(r->trigger_text) > 0) {
            if (strstr(payload, r->trigger_text) == NULL)
                continue;
        }

        LOG_INFO("RemoteSwitchRx: rule%d matched -> pin %d HIGH for %ds", i + 1, r->pin, r->timeout_secs);
        activatePin(r->pin, r->timeout_secs * 1000UL);
        timers[i].pin = r->pin;
    }
}

void RemoteSwitchReceiverModule::activatePin(uint8_t pin, uint32_t timeoutMs)
{
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);

    // Find or create timer slot for this pin
    for (int i = 0; i < 4; i++) {
        if (timers[i].pin == pin || !timers[i].active) {
            timers[i].pin = pin;
            timers[i].expiresAt = millis() + timeoutMs;
            timers[i].active = true;
            return;
        }
    }
}
