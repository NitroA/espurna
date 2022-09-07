/*

TERMINAL MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>
Copyright (C) 2020 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

#include "espurna.h"

#if TERMINAL_SUPPORT

#include "api.h"
#include "crash.h"
#include "mqtt.h"
#include "settings.h"
#include "system.h"
#include "telnet.h"
#include "terminal.h"
#include "utils.h"
#include "wifi.h"
#include "ws.h"

#include "libs/URL.h"
#include "libs/StreamAdapter.h"
#include "libs/PrintString.h"

#include "web_asyncwebprint.ipp"

#include <algorithm>
#include <utility>

#include <Schedule.h>
#include <Stream.h>

// not yet CONNECTING or LISTENING
extern "C" struct tcp_pcb *tcp_bound_pcbs;
// accepting or sending data
extern "C" struct tcp_pcb *tcp_active_pcbs;
// // TIME-WAIT status
extern "C" struct tcp_pcb *tcp_tw_pcbs;

// FS 'range', declared at compile time via .ld script PROVIDE declarations
// (althought, in recent Core versions, these may be set at runtime)
extern "C" uint32_t _FS_start;
extern "C" uint32_t _FS_end;

namespace espurna {
namespace terminal {
namespace {

namespace build {

constexpr size_t serialBufferSize() {
    return TERMINAL_SERIAL_BUFFER_SIZE;
}

Stream& SerialPort = TERMINAL_SERIAL_PORT;

} // namespace build

// -----------------------------------------------------------------------------
// Commands
// -----------------------------------------------------------------------------

namespace commands {

void help(CommandContext&& ctx) {
    auto names = terminal::names();

    std::sort(names.begin(), names.end(),
        [](const __FlashStringHelper* lhs, const __FlashStringHelper* rhs) {
            // XXX: Core's ..._P funcs only allow 2nd pointer to be in PROGMEM,
            //      explicitly load the 1st one
            // TODO: can we just assume linker already sorted all strings?
            const String lhs_as_string(lhs);
            return strncasecmp_P(lhs_as_string.c_str(), reinterpret_cast<const char*>(rhs), lhs_as_string.length()) < 0;
        });

    ctx.output.print(F("Available commands:\n"));
    for (auto* name : names) {
        ctx.output.printf("> %s\n", reinterpret_cast<const char*>(name));
    }

    terminalOK(ctx);
}

void netstat(CommandContext&& ctx) {
    const struct tcp_pcb* pcbs[] {
        tcp_active_pcbs,
        tcp_tw_pcbs,
        tcp_bound_pcbs,
    };

    for (const auto* list : pcbs) {
        for (const tcp_pcb* pcb = list; pcb != nullptr; pcb = pcb->next) {
            ctx.output.printf_P(PSTR("state %s local %s:%hu remote %s:%hu\n"),
                    tcp_debug_state_str(pcb->state),
                    IPAddress(pcb->local_ip).toString().c_str(),
                    pcb->local_port,
                    IPAddress(pcb->remote_ip).toString().c_str(),
                    pcb->remote_port);
        }
    }
}

namespace dns {

using FoundCallback = std::function<void(const char* name, const ip_addr_t* addr, void* arg)>;

namespace internal {

struct Task {
    Task() = delete;
    explicit Task(String hostname, FoundCallback callback) :
        _hostname(std::move(hostname)),
        _callback(std::move(callback))
    {}

    ip_addr_t* addr() {
        return &_addr;
    }

    const String& hostname() const {
        return _hostname;
    }

    void found_callback(const char* name, const ip_addr_t* addr, void* arg) {
        _callback(name, addr, arg);
    }

    void found_callback() {
        _callback(_hostname.c_str(), &_addr, nullptr);
    }

private:
    String _hostname;
    FoundCallback _callback;
    ip_addr_t _addr { IPADDR_NONE };
};

using TaskPtr = std::unique_ptr<Task>;
TaskPtr task;

void found_callback(const char* name, const ip_addr_t* addr, void* arg) {
    if (task) {
        task->found_callback(name, addr, arg);
        task.reset();
    }
}

} // namespace internal

bool started() {
    return static_cast<bool>(internal::task);
}

void start(String hostname, FoundCallback callback) {
    auto task = std::make_unique<internal::Task>(
            std::move(hostname), std::move(callback));

    const auto result = dns_gethostbyname(
            task->hostname().c_str(), task->addr(),
            internal::found_callback, nullptr);

    switch (result) {
    // No need to wait, return result immediately
    case ERR_OK:
        task->found_callback();
        break;
    // Task needs to linger for a bit
    case ERR_INPROGRESS:
        internal::task = std::move(task);
        break;
    }
}

} // namespace dns

void host(CommandContext&& ctx) {
    if (ctx.argv.size() != 2) {
        terminalError(ctx, F("HOST <hostname>"));
        return;
    }

    dns::start(std::move(ctx.argv[1]),
        [&](const char* name, const ip_addr_t* addr, void*) {
            if (!addr) {
                ctx.output.printf_P(PSTR("%s not found\n"), name);
                return;
            }

            ctx.output.printf_P(PSTR("%s has address %s\n"),
                name, IPAddress(addr).toString().c_str());
        });

    while (dns::started()) {
        delay(100);
    }
}

#if SECURE_CLIENT == SECURE_CLIENT_BEARSSL
void mfln_probe(CommandContext&& ctx) {
    if (ctx.argv.size() != 3) {
        terminalError(ctx, F("<url> <value>"));
        return;
    }

    URL _url(std::move(ctx.argv[1]));
    uint16_t requested_mfln = atol(ctx.argv[2].c_str());

    auto client = std::make_unique<BearSSL::WiFiClientSecure>();
    client->setInsecure();

    if (client->probeMaxFragmentLength(_url.host.c_str(), _url.port, requested_mfln)) {
        terminalOK(ctx);
        return;
    }

    terminalError(ctx, F("Buffer size not supported"));
}
#endif

void reset(CommandContext&& ctx) {
    prepareReset(CustomResetReason::Terminal);
    terminalOK(ctx);
}

void erase_config(CommandContext&& ctx) {
    terminalOK(ctx);
    customResetReason(CustomResetReason::Terminal);
    forceEraseSDKConfig();
}

void heap(CommandContext&& ctx) {
    const auto stats = systemHeapStats();
    ctx.output.printf_P(PSTR("initial: %lu available: %lu contiguous: %lu\n"),
            systemInitialFreeHeap(), stats.available, stats.usable);

    terminalOK(ctx);
}

void uptime(CommandContext&& ctx) {
    ctx.output.printf_P(PSTR("uptime %s\n"), getUptime().c_str());
    terminalOK(ctx);
}

void info(CommandContext&& ctx) {
    ctx.output.printf_P(PSTR("%s %s built %s\n"), getAppName(), getVersion(), buildTime().c_str());
    ctx.output.printf_P(PSTR("mcu: esp8266 chipid: %s freq: %hhumhz\n"), getFullChipId().c_str(), system_get_cpu_freq());
    ctx.output.printf_P(PSTR("sdk: %s core: %s\n"),
            ESP.getSdkVersion(), getCoreVersion().c_str());
    ctx.output.printf_P(PSTR("md5: %s\n"), ESP.getSketchMD5().c_str());
    ctx.output.printf_P(PSTR("support: %s\n"), getEspurnaModules());
#if SENSOR_SUPPORT
    ctx.output.printf_P(PSTR("sensors: %s\n"), getEspurnaSensors());
#endif
#if SYSTEM_CHECK_ENABLED
    ctx.output.printf_P(PSTR("system: %s boot counter: %u\n"),
        systemCheck() ? PSTR("OK") : PSTR("UNSTABLE"), systemStabilityCounter());
#endif
#if DEBUG_SUPPORT
    crashResetReason(ctx.output);
#endif
    terminalOK(ctx);
}

struct Layout {
    Layout() = delete;

    constexpr Layout(const Layout&) = default;
    constexpr Layout(Layout&&) = default;
    constexpr Layout(const char* const name, uint32_t start, uint32_t end) :
        _name(name),
        _start(start),
        _end(end)
    {}

    constexpr uint32_t size() const {
        return _end - _start;
    }

    constexpr uint32_t start() const {
        return _start;
    }

    constexpr uint32_t end() const {
        return _end;
    }

    constexpr const char* name() const {
        return _name;
    }

private:
    const char* const _name;
    uint32_t _start;
    uint32_t _end;
};

struct Layouts {
    using List = std::forward_list<Layout>;

    Layouts() = delete;
    explicit Layouts(uint32_t size) :
        _size(size),
        _current(size),
        _sectors(size / SPI_FLASH_SEC_SIZE)
    {}

    const Layout* head() const {
        if (_list.empty()) {
            return nullptr;
        }

        return &_list.front();
    }

    bool lock() {
        if (_lock) {
            return true;
        }

        _lock = true;
        return false;
    }

    uint32_t sectors() const {
        return _sectors;
    }

    uint32_t size() const {
        return _size - _current;
    }

    uint32_t current() const {
        return _current;
    }

    Layouts& add(const char* const name, uint32_t size) {
        if (!_lock && _current >= size) {
            Layout layout(name, _current - size, _current);
            _current -= layout.size();
            _list.push_front(layout);
        }

        return *this;
    }

    template <typename T>
    void foreach(T&& callback) {
        for (auto& layout : _list) {
            callback(layout);
        }
    }

private:
    bool _lock { false };
    List _list;
    uint32_t _size;
    uint32_t _current;
    uint32_t _sectors;
};

void storage(CommandContext&& ctx) {
    ctx.output.printf_P(PSTR("flash chip ID: 0x%06X\n"), ESP.getFlashChipId());
    ctx.output.printf_P(PSTR("speed: %u\n"), ESP.getFlashChipSpeed());
    ctx.output.printf_P(PSTR("mode: %s\n"), getFlashChipMode());

    ctx.output.printf_P(PSTR("size: %u (SPI), %u (SDK)\n"),
        ESP.getFlashChipRealSize(), ESP.getFlashChipSize());

    Layouts layouts(ESP.getFlashChipRealSize());

    // SDK specifies a hard-coded layout, there's no data beyond
    // (...addressable by the Core, since it adheres the setting)
    if (ESP.getFlashChipRealSize() > ESP.getFlashChipSize()) {
        layouts.add("unused", ESP.getFlashChipRealSize() - ESP.getFlashChipSize());
    }

    // app is at a normal location, [0...size), but... since it is offset by the free space, make sure it is aligned
    // to the sector size (...and it is expected from the getFreeSketchSpace, as the app will align to use the fixed
    // sector address for OTA writes).
    layouts.add("sdk", 4 * SPI_FLASH_SEC_SIZE);
    layouts.add("eeprom", eepromSpace());

    auto app_size = (ESP.getSketchSize() + FLASH_SECTOR_SIZE - 1) & (~(FLASH_SECTOR_SIZE - 1));
    auto ota_size = layouts.current() - app_size;

    // OTA is allowed to use all but one eeprom sectors that, leaving the last one
    // for the settings snapshot during the update
    layouts.add("ota", ota_size);
    layouts.add("app", app_size);

    layouts.foreach(
        [&](const Layout& layout) {
            ctx.output.printf_P("%-6s [%08X...%08X) (%u bytes)\n",
                    layout.name(), layout.start(), layout.end(), layout.size());
        });

    terminalOK(ctx);
}

void adc(CommandContext&& ctx) {
    const int pin = (ctx.argv.size() == 2)
        ? ctx.argv[1].toInt()
        : A0;

    ctx.output.printf_P(PSTR("value %d\n"), analogRead(pin));
    terminalOK(ctx);
}

#if SYSTEM_CHECK_ENABLED
void stable(CommandContext&& ctx) {
    systemForceStable();
    prepareReset(CustomResetReason::Stability);
}

void unstable(CommandContext&& ctx) {
    systemForceUnstable();
    prepareReset(CustomResetReason::Stability);
}

void trap(CommandContext&& ctx) {
    __builtin_trap();
}
#endif

void setup() {
    terminalRegisterCommand(F("COMMANDS"), commands::help);
    terminalRegisterCommand(F("HELP"), commands::help);

    terminalRegisterCommand(F("INFO"), commands::info);
    terminalRegisterCommand(F("STORAGE"), commands::storage);
    terminalRegisterCommand(F("UPTIME"), commands::uptime);
    terminalRegisterCommand(F("HEAP"), commands::heap);

    terminalRegisterCommand(F("NETSTAT"), commands::netstat);
    terminalRegisterCommand(F("HOST"), commands::host);
#if SECURE_CLIENT == SECURE_CLIENT_BEARSSL
    terminalRegisterCommand(F("MFLN.PROBE"), commands::mfln_probe);
#endif

    terminalRegisterCommand(F("ADC"), commands::adc);

    terminalRegisterCommand(F("RESET"), commands::reset);
    terminalRegisterCommand(F("ERASE.CONFIG"), commands::erase_config);

#if SYSTEM_CHECK_ENABLED
    terminalRegisterCommand(F("STABLE"), commands::stable);
    terminalRegisterCommand(F("UNSTABLE"), commands::unstable);
    terminalRegisterCommand(F("TRAP"), commands::trap);
#endif
}

} // namespace commands

#if TERMINAL_SERIAL_SUPPORT
namespace serial {

void loop() {
    using LineBuffer = LineBuffer<build::serialBufferSize()>;
    static LineBuffer buffer;

    static auto& port = build::SerialPort;

#if defined(ARDUINO_ESP8266_RELEASE_2_7_2) \
    || defined(ARDUINO_ESP8266_RELEASE_2_7_3) \
    || defined(ARDUINO_ESP8266_RELEASE_2_7_4)
    // 'Stream::readBytes()' includes a deadline, so any
    // call without using the actual value will result
    // in a 1second wait (by default)
    std::array<char, build::serialBufferSize()> tmp;
    const auto available = port.available();
    port.readBytes(tmp.data(), available);
    buffer.append(tmp.data(), available);
#else
    // Recent Core versions allow to access RX buffer directly
    const auto available = port.peekAvailable();
    if (available <= 0) {
        return;
    }

    buffer.append(port.peekBuffer(), available);
    port.peekConsume(available);
#endif

    if (buffer.overflow()) {
        terminal::error(port, F("Serial buffer overflow"));
        buffer.reset();
    }

    for (;;) {
        const auto result = buffer.line();
        if (result.overflow) {
            terminal::error(port, F("Command line buffer overflow"));
            continue;
        }

        if (!result.line.length()) {
            break;
        }

        find_and_call(result.line, port);
    }
}

} // namespace serial
#endif

#if MQTT_SUPPORT && TERMINAL_MQTT_SUPPORT
namespace mqtt {

void setup() {
    mqttRegister([](unsigned int type, const char* topic, char* payload) {
        if (type == MQTT_CONNECT_EVENT) {
            mqttSubscribe(MQTT_TOPIC_CMD);
            return;
        }

        if (type == MQTT_MESSAGE_EVENT) {
            String t = mqttMagnitude(topic);
            if (!t.startsWith(MQTT_TOPIC_CMD)) return;
            if (!strlen(payload)) return;

            String cmd(payload);
            if (!cmd.endsWith("\r\n") && !cmd.endsWith("\n")) {
                cmd += '\n';
            }

            // TODO: unlike http handler, we have only one output stream
            //       and **must** have a fixed-size output buffer
            //       (wishlist: MQTT client does some magic and we don't buffer twice)
            schedule_function([cmd]() {
                PrintString out(TCP_MSS);
                api_find_and_call(cmd, out);

                static const auto topic = mqttTopic(MQTT_TOPIC_CMD, false);
                if (out.length()) {
                    mqttSendRaw(topic.c_str(), out.c_str(), false);
                }
            });

            return;
        }
    });

}

} // namespace mqtt
#endif

#if WEB_SUPPORT
namespace web {

void onVisible(JsonObject& root) {
    wsPayloadModule(root, PSTR("cmd"));
}

void onAction(uint32_t client_id, const char* action, JsonObject& data) {
    if (strncmp_P(action, PSTR("cmd"), 3) != 0) {
        return;
    }

    alignas(4) static constexpr char Key[] PROGMEM = "line";
    if (!data.containsKey(FPSTR(Key)) || !data[FPSTR(Key)].is<String>()) {
        return;
    }

    const auto cmd = data[FPSTR(Key)].as<String>();
    if (!cmd.length()) {
        return;
    }

    schedule_function([cmd, client_id]() {
        // XXX: ???
        PrintString out(256);
        api_find_and_call(cmd, out);

        wsPost(client_id,
            [out](JsonObject& root) {
                auto& cmd = root.createNestedObject(FPSTR("cmd"));
                cmd[FPSTR("result")] = static_cast<const String&>(out);
            });
    });
}

void setup() {
    wsRegister()
        .onVisible(onVisible)
        .onAction(onAction);
}

} // namespace web
#endif

// -----------------------------------------------------------------------------
// Pubic API
// -----------------------------------------------------------------------------

#if TERMINAL_WEB_API_SUPPORT
namespace api {

// XXX: new `apiRegister()` depends that `webServer()` is available, meaning we can't call this setup func
// before the `webSetup()` is called. ATM, just make sure it is in order.

void setup() {
#if API_SUPPORT
    apiRegister(getSetting("termWebApiPath", TERMINAL_WEB_API_PATH),
        [](ApiRequest& api) {
            api.handle([](AsyncWebServerRequest* request) {
                auto* response = request->beginResponseStream(F("text/plain"));
                for (auto* name : names()) {
                    response->print(name);
                    response->print("\r\n");
                }

                request->send(response);
            });

            return true;
        },
        [](ApiRequest& api) {
            // TODO: since HTTP spec allows query string to contain repeating keys, allow iteration
            // over every received 'line' to provide a way to call multiple commands at once
            auto cmd = api.param(F("line"));
            if (!cmd.length()) {
                return false;
            }

            if (!cmd.endsWith("\r\n") && !cmd.endsWith("\n")) {
                cmd += '\n';
            }

            api.handle([&](AsyncWebServerRequest* request) {
                AsyncWebPrint::scheduleFromRequest(request,
                    [cmd](Print& out) {
                        api_find_and_call(cmd, out);
                    });
            });

            return true;
        }
    );
#else
    webRequestRegister([](AsyncWebServerRequest* request) {
        String path(F(API_BASE_PATH));
        path += getSetting("termWebApiPath", TERMINAL_WEB_API_PATH);
        if (path != request->url()) {
            return false;
        }

        if (!apiAuthenticate(request)) {
            request->send(403);
            return true;
        }

        auto* cmd_param = request->getParam("line", (request->method() == HTTP_PUT));
        if (!cmd_param) {
            request->send(500);
            return true;
        }

        auto cmd = cmd_param->value();
        if (!cmd.length()) {
            request->send(500);
            return true;
        }

        if (!cmd.endsWith("\r\n") && !cmd.endsWith("\n")) {
            cmd += '\n';
        }

        AsyncWebPrint::scheduleFromRequest(request,
            [cmd](Print& out) {
                api_find_and_call(cmd, out);
            });

        return true;
    });
#endif // API_SUPPORT
}

} // namespace api
#endif // TERMINAL_WEB_API_SUPPORT

void loop() {
#if TERMINAL_SERIAL_SUPPORT
    // TODO: check if something else is using this port?
    serial::loop();
#endif
}

void setup() {
#if WEB_SUPPORT
    // Show DEBUG panel with input
    web::setup();
#endif

#if MQTT_SUPPORT && TERMINAL_MQTT_SUPPORT
    // Similar to the above, accept cmdline(s) in payload
    mqtt::setup();
#endif

    // Initialize default commands
    commands::setup();

    // Register loop
    espurnaRegisterLoop(loop);
}

} // namespace
} // namespace terminal
} // namespace espurna

void terminalOK(const espurna::terminal::CommandContext& ctx) {
    espurna::terminal::ok(ctx);
}

void terminalError(const espurna::terminal::CommandContext& ctx, const String& message) {
    espurna::terminal::error(ctx, message);
}

void terminalRegisterCommand(const __FlashStringHelper* name, espurna::terminal::CommandFunc func) {
    espurna::terminal::add(name, func);
}

#if TERMINAL_WEB_API_SUPPORT
void terminalWebApiSetup() {
    espurna::terminal::api::setup();
}
#endif

void terminalSetup() {
    espurna::terminal::setup();
}

#endif // TERMINAL_SUPPORT
