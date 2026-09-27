#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <CoreAudio/CoreAudio.h>
#include <portaudio.h>

#define PORT 24242
#define INPUT_FRAMES 240
#define BUFFER_FRAMES 16384
#define TARGET_FRAMES 2400
#define DISCOVERY "WMIC_DISCOVER_V1"
#define READY "WMIC_READY_V1"
#define ACTIVE "WMIC_ACTIVE_V1"
#define IDLE "WMIC_IDLE_V1"

static bool audio_property(AudioObjectID object,
                           AudioObjectPropertySelector selector,
                           AudioObjectPropertyScope scope, void *value,
                           UInt32 *size)
{
    AudioObjectPropertyAddress address = {
        selector, scope, kAudioObjectPropertyElementMain
    };
    return AudioObjectGetPropertyData(object, &address, 0, NULL,
                                      size, value) == noErr;
}

static AudioObjectID blackhole_object(void)
{
    AudioObjectPropertyAddress address = {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address,
                                       0, NULL, &size) != noErr) return 0;
    AudioObjectID *devices = malloc(size);
    if (!devices) return 0;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0,
                                   NULL, &size, devices) != noErr) {
        free(devices);
        return 0;
    }
    AudioObjectID found = 0;
    for (UInt32 i = 0; i < size / sizeof(*devices); ++i) {
        CFStringRef name = NULL;
        UInt32 name_size = sizeof(name);
        if (audio_property(devices[i], kAudioObjectPropertyName,
                           kAudioObjectPropertyScopeGlobal, &name,
                           &name_size) && name) {
            char text[256] = {0};
            if (CFStringGetCString(name, text, sizeof(text),
                                   kCFStringEncodingUTF8) &&
                strstr(text, "BlackHole 2ch")) found = devices[i];
            CFRelease(name);
        }
    }
    free(devices);
    return found;
}

// An output feeding BlackHole makes the device globally "running". Only a
// separate process running an input stream on this device means a Mac app is
// listening to the microphone.
static bool input_consumer_active(AudioObjectID blackhole, bool *active)
{
    AudioObjectPropertyAddress address = {
        kAudioHardwarePropertyProcessObjectList,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address,
                                       0, NULL, &size) != noErr) return false;
    AudioObjectID *processes = malloc(size ? size : 1);
    if (!processes) return false;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0,
                                   NULL, &size, processes) != noErr) {
        free(processes);
        return false;
    }
    *active = false;
    for (UInt32 i = 0; i < size / sizeof(*processes); ++i) {
        UInt32 running_input = 0;
        UInt32 value_size = sizeof(running_input);
        if (!audio_property(processes[i], kAudioProcessPropertyIsRunningInput,
                            kAudioObjectPropertyScopeGlobal, &running_input,
                            &value_size) || !running_input) continue;
        AudioObjectPropertyAddress devices_address = {
            kAudioProcessPropertyDevices, kAudioObjectPropertyScopeInput,
            kAudioObjectPropertyElementMain
        };
        UInt32 devices_size = 0;
        if (AudioObjectGetPropertyDataSize(processes[i], &devices_address, 0,
                                           NULL, &devices_size) != noErr) continue;
        AudioObjectID *devices = malloc(devices_size ? devices_size : 1);
        if (!devices) continue;
        if (AudioObjectGetPropertyData(processes[i], &devices_address, 0,
                                       NULL, &devices_size, devices) == noErr) {
            for (UInt32 j = 0; j < devices_size / sizeof(*devices); ++j) {
                if (devices[j] == blackhole) *active = true;
            }
        }
        free(devices);
        if (*active) break;
    }
    free(processes);
    return true;
}

typedef struct {
    pthread_mutex_t lock;
    int16_t samples[BUFFER_FRAMES];
    uint64_t written;
    double position;
    bool primed;
    bool have_sequence;
    uint32_t expected_sequence;
    uint64_t packets;
    uint64_t lost;
    uint64_t underruns;
} audio_state_t;

static volatile sig_atomic_t running = 1;

static void stop(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static void push_sample(audio_state_t *state, int16_t sample)
{
    state->samples[state->written % BUFFER_FRAMES] = sample;
    state->written++;
    if ((double)state->written - state->position >= BUFFER_FRAMES - 2) {
        state->position = (double)(state->written - TARGET_FRAMES);
    }
}

static int playback(const void *input, void *output, unsigned long frames,
                    const PaStreamCallbackTimeInfo *time_info,
                    PaStreamCallbackFlags flags, void *context)
{
    (void)input;
    (void)time_info;
    (void)flags;
    audio_state_t *state = context;
    int16_t *stereo = output;
    pthread_mutex_lock(&state->lock);
    for (unsigned long i = 0; i < frames; ++i) {
        double available = (double)state->written - state->position;
        if (!state->primed && available >= 1200) state->primed = true;
        int16_t value = 0;
        if (state->primed && available >= 2) {
            uint64_t index = (uint64_t)state->position;
            double fraction = state->position - (double)index;
            int16_t a = state->samples[index % BUFFER_FRAMES];
            int16_t b = state->samples[(index + 1) % BUFFER_FRAMES];
            value = (int16_t)((double)a + ((double)b - a) * fraction);
            double correction = (available - TARGET_FRAMES) * 0.000002;
            if (correction > 0.01) correction = 0.01;
            if (correction < -0.01) correction = -0.01;
            state->position += 0.5 * (1.0 + correction);
        } else if (state->primed) {
            state->primed = false;
            state->position = (double)state->written;
            state->underruns++;
        }
        stereo[2 * i] = value;
        stereo[2 * i + 1] = value;
    }
    pthread_mutex_unlock(&state->lock);
    return paContinue;
}

static int blackhole_device(void)
{
    int count = Pa_GetDeviceCount();
    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo *device = Pa_GetDeviceInfo(i);
        if (device && strstr(device->name, "BlackHole 2ch") &&
            device->maxOutputChannels >= 2) return i;
    }
    return paNoDevice;
}

static void list_devices(void)
{
    int count = Pa_GetDeviceCount();
    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo *device = Pa_GetDeviceInfo(i);
        if (device && device->maxOutputChannels)
            printf("%d: %s (%d output channels)\n", i, device->name,
                   device->maxOutputChannels);
    }
}

static bool receive_audio(audio_state_t *state, const uint8_t *packet,
                          size_t length)
{
    if (length != 8 + INPUT_FRAMES * sizeof(int16_t) ||
        memcmp(packet, "WMIC", 4) != 0) return false;
    uint32_t network_sequence;
    memcpy(&network_sequence, packet + 4, sizeof(network_sequence));
    uint32_t sequence = ntohl(network_sequence);
    pthread_mutex_lock(&state->lock);
    if (state->have_sequence && sequence < 100 &&
        state->expected_sequence > 100) {
        state->have_sequence = false;
        state->primed = false;
        state->position = (double)state->written;
    }
    if (state->have_sequence) {
        int32_t delta = (int32_t)(sequence - state->expected_sequence);
        if (delta < 0) {
            pthread_mutex_unlock(&state->lock);
            return false;
        }
        if (delta > 0 && delta < 10) {
            state->lost += (uint32_t)delta;
            for (int j = 0; j < delta * INPUT_FRAMES; ++j) push_sample(state, 0);
        } else if (delta >= 10) {
            state->primed = false;
            state->position = (double)state->written;
        }
    }
    state->have_sequence = true;
    state->expected_sequence = sequence + 1;
    for (int i = 0; i < INPUT_FRAMES; ++i) {
        uint16_t value = (uint16_t)packet[8 + i * 2] |
                         ((uint16_t)packet[9 + i * 2] << 8);
        push_sample(state, (int16_t)value);
    }
    state->packets++;
    pthread_mutex_unlock(&state->lock);
    return true;
}

static double now_seconds(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (double)time.tv_sec + (double)time.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--list") != 0)) {
        fprintf(stderr, "Usage: %s [--list]\n", argv[0]);
        return 2;
    }
    PaError error = Pa_Initialize();
    if (error != paNoError) {
        fprintf(stderr, "PortAudio: %s\n", Pa_GetErrorText(error));
        return 1;
    }
    if (argc == 2) {
        list_devices();
        Pa_Terminate();
        return 0;
    }
    int device = blackhole_device();
    if (device == paNoDevice) {
        fprintf(stderr, "BlackHole 2ch output not found. Install and restart macOS, then retry.\n");
        Pa_Terminate();
        return 1;
    }
    AudioObjectID blackhole = blackhole_object();
    if (!blackhole) {
        fprintf(stderr, "BlackHole CoreAudio device not found.\n");
        Pa_Terminate();
        return 1;
    }
    audio_state_t state = {.lock = PTHREAD_MUTEX_INITIALIZER};
    const PaDeviceInfo *device_info = Pa_GetDeviceInfo(device);
    PaStreamParameters output = {
        .device = device,
        .channelCount = 2,
        .sampleFormat = paInt16,
        .suggestedLatency = device_info->defaultLowOutputLatency,
    };
    PaStream *stream = NULL;
    error = Pa_OpenStream(&stream, NULL, &output, 48000, 480, paNoFlag,
                          playback, &state);
    if (error != paNoError) {
        fprintf(stderr, "BlackHole output: %s\n", Pa_GetErrorText(error));
        Pa_Terminate();
        return 1;
    }
    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) {
        perror("UDP socket");
        Pa_CloseStream(stream);
        Pa_Terminate();
        return 1;
    }
    int yes = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(socket_fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("UDP bind");
        close(socket_fd);
        Pa_CloseStream(stream);
        Pa_Terminate();
        return 1;
    }
    error = Pa_StartStream(stream);
    if (error != paNoError) {
        fprintf(stderr, "Start output: %s\n", Pa_GetErrorText(error));
        close(socket_fd);
        Pa_CloseStream(stream);
        Pa_Terminate();
        return 1;
    }
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    struct sockaddr_in broadcast = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    struct sockaddr_in board = {0};
    bool board_known = false;
    fprintf(stderr, "Feeding BlackHole 2ch from Waveshare Wi-Fi mic; Ctrl-C to stop.\n");
    double last_discovery = -100;
    double last_broadcast = -100;
    double last_control = -100;
    double last_idle_message = -100;
    double last_report = now_seconds();
    double last_audio = 0;
    double last_ready = 0;
    bool connected = false;
    bool input_active = false;
    bool activity_warning = false;
    uint64_t previous_packets = 0;
    while (running) {
        double now = now_seconds();
        if (now - last_discovery >= 1.0) {
            const struct sockaddr_in *destination = board_known ? &board : &broadcast;
            sendto(socket_fd, DISCOVERY, sizeof(DISCOVERY) - 1, 0,
                   (const struct sockaddr *)destination, sizeof(*destination));
            // An occasional broadcast can discover a board whose DHCP address changed.
            if (board_known && now - last_broadcast >= 10.0) {
                sendto(socket_fd, DISCOVERY, sizeof(DISCOVERY) - 1, 0,
                       (struct sockaddr *)&broadcast, sizeof(broadcast));
                last_broadcast = now;
            }
            last_discovery = now;
        }
        if (now - last_control >= 0.25) {
            bool detected = false;
            if (!input_consumer_active(blackhole, &detected)) {
                // Keep the microphone usable on a CoreAudio query failure.
                detected = true;
                if (!activity_warning) {
                    fprintf(stderr, "Cannot read input activity; keeping microphone awake.\n");
                    activity_warning = true;
                }
            } else {
                activity_warning = false;
            }
            bool changed = detected != input_active;
            if (changed) {
                fprintf(stderr, "Mac input %s; board %s.\n",
                        detected ? "opened" : "closed",
                        detected ? "waking" : "idling");
                pthread_mutex_lock(&state.lock);
                state.primed = false;
                state.have_sequence = false;
                state.position = (double)state.written;
                pthread_mutex_unlock(&state.lock);
                input_active = detected;
                last_audio = 0;
            }
            if (input_active || changed || now - last_idle_message >= 5.0) {
                const char *control = input_active ? ACTIVE : IDLE;
                const struct sockaddr_in *destination = board_known ? &board : &broadcast;
                sendto(socket_fd, control, strlen(control), 0,
                       (const struct sockaddr *)destination, sizeof(*destination));
                if (!input_active) last_idle_message = now;
            }
            last_control = now;
        }
        uint8_t packet[512];
        struct sockaddr_in sender = {0};
        socklen_t sender_size = sizeof(sender);
        ssize_t received = recvfrom(socket_fd, packet, sizeof(packet), 0,
                                    (struct sockaddr *)&sender, &sender_size);
        if (received > 0 && received == sizeof(READY) - 1 &&
            memcmp(packet, READY, received) == 0) {
            board = sender;
            board_known = true;
            last_ready = now_seconds();
            // The board's packet sequence restarts at zero after a flash or
            // power cycle. A fresh discovery reply is the resync point.
            if (!connected && last_audio > 0 &&
                now_seconds() - last_audio > 0.5) {
                pthread_mutex_lock(&state.lock);
                state.have_sequence = false;
                state.primed = false;
                state.position = (double)state.written;
                pthread_mutex_unlock(&state.lock);
            }
            if (!connected) {
                char address[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &sender.sin_addr, address, sizeof(address));
                fprintf(stderr, "Microphone connected at %s.\n", address);
                last_audio = 0;
            }
            connected = true;
        } else if (received > 0) {
            if (receive_audio(&state, packet, (size_t)received))
                last_audio = now_seconds();
        }
        now = now_seconds();
        if (connected && input_active && last_audio > 0 &&
            now - last_audio > 3.0) {
            connected = false;
            fprintf(stderr, "Microphone audio stopped; waiting to reconnect.\n");
        }
        if (now - last_report >= 5.0) {
            pthread_mutex_lock(&state.lock);
            uint64_t packets = state.packets;
            uint64_t lost = state.lost;
            uint64_t underruns = state.underruns;
            pthread_mutex_unlock(&state.lock);
            fprintf(stderr, "Audio packets: %llu (+%llu), lost: %llu, underruns: %llu, reply age: %.1fs\n",
                    (unsigned long long)packets,
                    (unsigned long long)(packets - previous_packets),
                    (unsigned long long)lost,
                    (unsigned long long)underruns,
                    last_ready > 0 ? now - last_ready : -1.0);
            previous_packets = packets;
            last_report = now;
        }
    }
    close(socket_fd);
    // A virtual output can wait indefinitely for its buffer to drain when
    // nobody is reading it. Abort promptly on SIGTERM so launchd can restart.
    Pa_AbortStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();
    pthread_mutex_destroy(&state.lock);
    return 0;
}
