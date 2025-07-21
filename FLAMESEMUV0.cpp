#include <SDL2/SDL.h>
#include <cstdint>
#include <vector>
#include <cstring>
#include <iostream>
#include <chrono>
#include <thread>
#include <cmath>

// Constants for Wii memory sizes
const uint32_t MEM1_SIZE = 24 * 1024 * 1024;  // 24 MB
const uint32_t MEM2_SIZE = 64 * 1024 * 1024;  // 64 MB

// Memory-mapped I/O register addresses (for emulator integration)
const uint32_t REG_VIDEO_BG_COLOR = 0x0D000000;  // Background color register (example)
const uint32_t REG_INPUT_STATE    = 0x0D000004;  // Input state register (buttons)
const uint32_t REG_AUDIO_FREQ     = 0x0D000008;  // Audio frequency register (tone control)

// Forward declarations
class Video;
class Audio;
class Input;

class Memory {
public:
    Memory() {
        // Allocate and initialize MEM1 and MEM2
        mem1.resize(MEM1_SIZE);
        mem2.resize(MEM2_SIZE);
        std::memset(mem1.data(), 0, MEM1_SIZE);
        std::memset(mem2.data(), 0, MEM2_SIZE);
        // Initialize I/O register values
        videoBgColor = 0x00000000;  // default black background
        audioFreqValue = 0;
        video = nullptr;
        audio = nullptr;
        input = nullptr;
    }

    // Connect hardware components for I/O callbacks
    void connectVideo(Video* v)   { video = v; }
    void connectAudio(Audio* a)   { audio = a; }
    void connectInput(Input* i)   { input = i; }

    // Read 32-bit word from memory or I/O (PowerPC is big-endian)
    uint32_t read32(uint32_t address) {
        // Translate address to physical region (MEM1, MEM2 or I/O)
        if ((address >= 0x80000000 && address < 0x80000000 + MEM1_SIZE) || 
            (address >= 0xC0000000 && address < 0xC0000000 + MEM1_SIZE)) {
            // MEM1 (24MB)
            uint32_t offset = address & (MEM1_SIZE - 1);  // mask to 24MB
            if (offset + 3 < MEM1_SIZE) {
                // Combine 4 bytes in big-endian order
                uint32_t b0 = mem1[offset];
                uint32_t b1 = mem1[offset + 1];
                uint32_t b2 = mem1[offset + 2];
                uint32_t b3 = mem1[offset + 3];
                return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
            }
            SDL_Log("MEM1 read out of range: 0x%08X", address);
            return 0;
        } else if ((address >= 0x90000000 && address < 0x90000000 + MEM2_SIZE) || 
                   (address >= 0xD0000000 && address < 0xD0000000 + MEM2_SIZE)) {
            // MEM2 (64MB)
            uint32_t offset = address & (MEM2_SIZE - 1);
            if (offset + 3 < MEM2_SIZE) {
                uint32_t b0 = mem2[offset];
                uint32_t b1 = mem2[offset + 1];
                uint32_t b2 = mem2[offset + 2];
                uint32_t b3 = mem2[offset + 3];
                return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
            }
            SDL_Log("MEM2 read out of range: 0x%08X", address);
            return 0;
        } else {
            // I/O or other regions
            if (address == REG_VIDEO_BG_COLOR) {
                return videoBgColor;
            } else if (address == REG_INPUT_STATE) {
                return input ? input->getButtonState() : 0;
            } else if (address == REG_AUDIO_FREQ) {
                return audioFreqValue;
            }
            SDL_Log("Unhandled read from address 0x%08X", address);
            return 0;
        }
    }

    // Write 32-bit word to memory or I/O (big-endian format)
    void write32(uint32_t address, uint32_t value) {
        if ((address >= 0x80000000 && address < 0x80000000 + MEM1_SIZE) || 
            (address >= 0xC0000000 && address < 0xC0000000 + MEM1_SIZE)) {
            // MEM1 write
            uint32_t offset = address & (MEM1_SIZE - 1);
            if (offset + 3 < MEM1_SIZE) {
                // Split value into bytes (big-endian)
                mem1[offset]     = (value >> 24) & 0xFF;
                mem1[offset + 1] = (value >> 16) & 0xFF;
                mem1[offset + 2] = (value >> 8) & 0xFF;
                mem1[offset + 3] = value & 0xFF;
            } else {
                SDL_Log("MEM1 write out of range: 0x%08X", address);
            }
        } else if ((address >= 0x90000000 && address < 0x90000000 + MEM2_SIZE) || 
                   (address >= 0xD0000000 && address < 0xD0000000 + MEM2_SIZE)) {
            // MEM2 write
            uint32_t offset = address & (MEM2_SIZE - 1);
            if (offset + 3 < MEM2_SIZE) {
                mem2[offset]     = (value >> 24) & 0xFF;
                mem2[offset + 1] = (value >> 16) & 0xFF;
                mem2[offset + 2] = (value >> 8) & 0xFF;
                mem2[offset + 3] = value & 0xFF;
            } else {
                SDL_Log("MEM2 write out of range: 0x%08X", address);
            }
        } else {
            // I/O register write
            if (address == REG_VIDEO_BG_COLOR) {
                videoBgColor = value;
                if (video) video->setBackgroundColor(value);
            } else if (address == REG_INPUT_STATE) {
                SDL_Log("Ignoring write to input state register");
            } else if (address == REG_AUDIO_FREQ) {
                audioFreqValue = value;
                if (audio) audio->setToneFrequency((double)value);
            } else {
                SDL_Log("Unhandled write to address 0x%08X: value 0x%08X", address, value);
            }
        }
    }

private:
    std::vector<uint8_t> mem1;
    std::vector<uint8_t> mem2;
    Video*  video;
    Audio*  audio;
    Input*  input;
    uint32_t videoBgColor;
    uint32_t audioFreqValue;
};

// Video subsystem
class Video {
public:
    Video() : renderer(nullptr), window(nullptr), bgColor(0x00000000) {}

    bool init() {
        // Create window with Metal backend for M1 optimization
        window = SDL_CreateWindow("Wii Memory Emulator - 60 FPS",
                                  SDL_WINDOWPOS_CENTERED,
                                  SDL_WINDOWPOS_CENTERED,
                                  854, 480,  // Wii resolution
                                  SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
        if (!window) {
            SDL_Log("Failed to create window: %s", SDL_GetError());
            return false;
        }

        // Create renderer with vsync for smooth 60 FPS
        renderer = SDL_CreateRenderer(window, -1, 
                                      SDL_RENDERER_ACCELERATED | 
                                      SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) {
            SDL_Log("Failed to create renderer: %s", SDL_GetError());
            return false;
        }

        return true;
    }

    void shutdown() {
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
    }

    void setBackgroundColor(uint32_t color) {
        bgColor = color;
    }

    void render() {
        // Extract RGBA from 32-bit color
        uint8_t r = (bgColor >> 24) & 0xFF;
        uint8_t g = (bgColor >> 16) & 0xFF;
        uint8_t b = (bgColor >> 8) & 0xFF;
        uint8_t a = bgColor & 0xFF;

        SDL_SetRenderDrawColor(renderer, r, g, b, a);
        SDL_RenderClear(renderer);

        // Draw some debug info
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_Rect rect = {10, 10, 200, 50};
        SDL_RenderDrawRect(renderer, &rect);

        SDL_RenderPresent(renderer);
    }

private:
    SDL_Renderer* renderer;
    SDL_Window* window;
    uint32_t bgColor;
};

// Audio subsystem
class Audio {
public:
    Audio() : deviceId(0), frequency(440.0), phase(0.0) {}

    bool init() {
        SDL_AudioSpec desired, obtained;
        SDL_zero(desired);
        desired.freq = 48000;
        desired.format = AUDIO_F32;
        desired.channels = 2;
        desired.samples = 512;  // Low latency
        desired.callback = audioCallback;
        desired.userdata = this;

        deviceId = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
        if (deviceId == 0) {
            SDL_Log("Failed to open audio device: %s", SDL_GetError());
            return false;
        }

        SDL_PauseAudioDevice(deviceId, 0);  // Start playback
        return true;
    }

    void shutdown() {
        if (deviceId) {
            SDL_CloseAudioDevice(deviceId);
        }
    }

    void setToneFrequency(double freq) {
        frequency = freq;
    }

private:
    static void audioCallback(void* userdata, uint8_t* stream, int len) {
        Audio* audio = static_cast<Audio*>(userdata);
        float* buffer = reinterpret_cast<float*>(stream);
        int samples = len / sizeof(float) / 2;  // stereo

        for (int i = 0; i < samples; i++) {
            float sample = 0.0f;
            if (audio->frequency > 0) {
                sample = 0.1f * sinf(audio->phase);
                audio->phase += 2.0f * M_PI * audio->frequency / 48000.0f;
                if (audio->phase > 2.0f * M_PI) {
                    audio->phase -= 2.0f * M_PI;
                }
            }
            buffer[i * 2] = sample;      // left
            buffer[i * 2 + 1] = sample;  // right
        }
    }

    SDL_AudioDeviceID deviceId;
    double frequency;
    float phase;
};

// Input subsystem
class Input {
public:
    Input() : buttonState(0) {}

    void update() {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                buttonState |= 0x80000000;  // Quit flag
            } else if (event.type == SDL_KEYDOWN) {
                switch (event.key.keysym.sym) {
                    case SDLK_UP:    buttonState |= 0x00000001; break;
                    case SDLK_DOWN:  buttonState |= 0x00000002; break;
                    case SDLK_LEFT:  buttonState |= 0x00000004; break;
                    case SDLK_RIGHT: buttonState |= 0x00000008; break;
                    case SDLK_a:     buttonState |= 0x00000010; break;
                    case SDLK_b:     buttonState |= 0x00000020; break;
                    case SDLK_SPACE: buttonState |= 0x00000040; break;
                }
            } else if (event.type == SDL_KEYUP) {
                switch (event.key.keysym.sym) {
                    case SDLK_UP:    buttonState &= ~0x00000001; break;
                    case SDLK_DOWN:  buttonState &= ~0x00000002; break;
                    case SDLK_LEFT:  buttonState &= ~0x00000004; break;
                    case SDLK_RIGHT: buttonState &= ~0x00000008; break;
                    case SDLK_a:     buttonState &= ~0x00000010; break;
                    case SDLK_b:     buttonState &= ~0x00000020; break;
                    case SDLK_SPACE: buttonState &= ~0x00000040; break;
                }
            }
        }
    }

    uint32_t getButtonState() const {
        return buttonState;
    }

    bool shouldQuit() const {
        return (buttonState & 0x80000000) != 0;
    }

private:
    uint32_t buttonState;
};

// Main emulator class
class WiiEmulator {
public:
    WiiEmulator() : running(false) {}

    bool init() {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
            SDL_Log("SDL initialization failed: %s", SDL_GetError());
            return false;
        }

        if (!video.init() || !audio.init()) {
            return false;
        }

        // Connect components to memory
        memory.connectVideo(&video);
        memory.connectAudio(&audio);
        memory.connectInput(&input);

        return true;
    }

    void shutdown() {
        video.shutdown();
        audio.shutdown();
        SDL_Quit();
    }

    void run() {
        running = true;
        
        // Timing for 60 FPS
        const auto frameTime = std::chrono::microseconds(16667);  // ~60 FPS
        auto nextFrame = std::chrono::high_resolution_clock::now();
        
        // Demo variables
        uint32_t colorCycle = 0;
        int toneFreq = 440;
        
        while (running) {
            auto frameStart = std::chrono::high_resolution_clock::now();
            
            // Update input
            input.update();
            if (input.shouldQuit()) {
                running = false;
                break;
            }
            
            // Demo: Read input and update system
            uint32_t buttons = memory.read32(REG_INPUT_STATE);
            
            // Change background color based on input
            if (buttons & 0x00000001) colorCycle += 0x01000000;  // UP - increase red
            if (buttons & 0x00000002) colorCycle -= 0x01000000;  // DOWN - decrease red
            if (buttons & 0x00000004) colorCycle += 0x00010000;  // LEFT - increase green
            if (buttons & 0x00000008) colorCycle -= 0x00010000;  // RIGHT - decrease green
            
            // Change audio tone with A/B buttons
            if (buttons & 0x00000010) toneFreq = std::min(toneFreq + 10, 2000);  // A - higher
            if (buttons & 0x00000020) toneFreq = std::max(toneFreq - 10, 100);   // B - lower
            
            // Space toggles audio on/off
            static bool audioOn = false;
            static bool spacePressed = false;
            if ((buttons & 0x00000040) && !spacePressed) {
                audioOn = !audioOn;
                spacePressed = true;
            } else if (!(buttons & 0x00000040)) {
                spacePressed = false;
            }
            
            // Write to memory-mapped registers
            memory.write32(REG_VIDEO_BG_COLOR, colorCycle);
            memory.write32(REG_AUDIO_FREQ, audioOn ? toneFreq : 0);
            
            // Test memory read/write
            static bool memTestDone = false;
            if (!memTestDone) {
                // Write test pattern to MEM1
                memory.write32(0x80000000, 0xDEADBEEF);
                uint32_t testRead = memory.read32(0x80000000);
                SDL_Log("Memory test - Written: 0xDEADBEEF, Read: 0x%08X", testRead);
                memTestDone = true;
            }
            
            // Render
            video.render();
            
            // Frame timing for consistent 60 FPS
            nextFrame += frameTime;
            std::this_thread::sleep_until(nextFrame);
            
            // Log FPS occasionally
            static int frameCount = 0;
            static auto lastFpsLog = frameStart;
            frameCount++;
            if (frameCount >= 300) {  // Every 5 seconds at 60 FPS
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    frameStart - lastFpsLog).count();
                double fps = (frameCount * 1000.0) / elapsed;
                SDL_Log("FPS: %.2f", fps);
                frameCount = 0;
                lastFpsLog = frameStart;
            }
        }
    }

private:
    Memory memory;
    Video video;
    Audio audio;
    Input input;
    bool running;
};

int main(int argc, char* argv[]) {
    WiiEmulator emulator;
    
    if (!emulator.init()) {
        SDL_Log("Failed to initialize emulator");
        return 1;
    }
    
    SDL_Log("Wii Memory Emulator started - 60 FPS");
    SDL_Log("Controls:");
    SDL_Log("  Arrow Keys: Change background color (R/G channels)");
    SDL_Log("  A/B: Change audio tone frequency");
    SDL_Log("  Space: Toggle audio on/off");
    SDL_Log("  ESC/Close: Quit");
    
    emulator.run();
    emulator.shutdown();
    
    return 0;
}
