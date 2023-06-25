// SPDX-FileCopyrightText: 2021-2023 Filipe Coelho <falktx@falktx.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audio-discovery.hpp"
#include "audio-process.hpp"

#include <jack/jack.h>
#include <unistd.h>

struct ClientData {
    DeviceAudio* dev;
    jack_port_t* ports[2];
};

static int jack_process(const unsigned frames, void* const arg)
{
    ClientData* const c = static_cast<ClientData*>(arg);
    float* buffers[2] = {
        (float*)jack_port_get_buffer(c->ports[0], frames),
        (float*)jack_port_get_buffer(c->ports[1], frames)
    };
    runDeviceAudio(c->dev, buffers);
    return 0;
}

void do_capture(const char* const deviceID)
{
    if (jack_client_t* const c = jack_client_open("awoosb-capture", JackNoStartServer, nullptr))
    {
        ClientData d;

        const uint16_t bufferSize = jack_get_buffer_size(c);
        const uint32_t sampleRate = jack_get_sample_rate(c);

        if ((d.dev = initDeviceAudio(deviceID, false, bufferSize, sampleRate)) == nullptr)
            goto end;

        d.ports[0] = jack_port_register(c, "p1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput|JackPortIsTerminal, 0);
        d.ports[1] = jack_port_register(c, "p2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput|JackPortIsTerminal, 0);

        jack_set_process_callback(c, jack_process, &d);
        jack_activate(c);

        jack_connect(c, "awoosb-capture:p1", "awoosb-playback:p1");
        jack_connect(c, "awoosb-capture:p2", "awoosb-playback:p2");

        while (true) sleep(1);

        jack_deactivate(c);

        closeDeviceAudio(d.dev);

    end:
        jack_client_close(c);
    }
}

void do_playback(const char* const deviceID)
{
    if (jack_client_t* const c = jack_client_open("awoosb-playback", JackNoStartServer, nullptr))
    {
        ClientData d;

        const uint16_t bufferSize = jack_get_buffer_size(c);
        const uint32_t sampleRate = jack_get_sample_rate(c);

        if ((d.dev = initDeviceAudio(deviceID, true, bufferSize, sampleRate)) == nullptr)
            goto end;

        d.ports[0] = jack_port_register(c, "p1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput|JackPortIsTerminal, 0);
        d.ports[1] = jack_port_register(c, "p2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput|JackPortIsTerminal, 0);

        jack_set_process_callback(c, jack_process, &d);
        jack_activate(c);

        jack_connect(c, "PulseAudio JACK Sink:front-left", "awoosb-playback:p1");
        jack_connect(c, "PulseAudio JACK Sink:front-right", "awoosb-playback:p2");
        jack_connect(c, "awoosb-capture:p1", "awoosb-playback:p1");
        jack_connect(c, "awoosb-capture:p2", "awoosb-playback:p2");
        // jack_connect(c, "mod-monitor:out_1", "awoosb-playback:p1");
        // jack_connect(c, "mod-monitor:out_2", "awoosb-playback:p2");

        while (true) sleep(1);

        jack_deactivate(c);

        closeDeviceAudio(d.dev);

    end:
        jack_client_close(c);
    }
}

int main(int argc, const char* argv[])
{
    std::vector<DeviceID> inputs, outputs;
    enumerateSoundcards(inputs, outputs);

    if (argc > 2)
        do_capture(argv[1]);
    else if (argc > 1)
        do_playback(argv[1]);
    else
        do_playback(outputs[outputs.size() - 1].id.c_str());

    cleanup();

    return 0;
}