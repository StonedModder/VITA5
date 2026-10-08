// VITA5 app — host tests for the shared-memory protocol, NV12 conversion
// and pad mapping. Host evidence only: these run on Linux/WSL against an
// in-memory region and say nothing about console behaviour.
//
// The file is a translation unit in the PS5 build too (the build compiles
// every source under app/); the tests are compiled in only when
// VD5_HOST_TESTS is defined (app/Makefile `test`).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/vd_settings.hpp"

#ifdef VD5_HOST_TESTS

#include "vd/nv12.hpp"
#include "vd/pad_bridge.hpp"
#include "vd/vd_shm.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace
{

int g_failures = 0;

#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                       \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

// ---- a stand-in kernel producer driving the documented protocol ----------

struct FakeRegion
{
    std::vector<std::uint8_t> bytes;

    FakeRegion() : bytes(VD_SHM_TOTAL_BYTES)
    {
        auto *header = reinterpret_cast<VdSharedHeader *>(bytes.data());
        header->magic = VD_SHM_MAGIC;
        header->version = VD_SHM_VERSION;
        header->video_width = 960;
        header->video_height = 544;
        header->video_pixel_format = VD_PIX_NV12;
        header->audio_sample_rate = 48000;
        header->audio_channels = 2;
        header->audio_bits = 16;
    }

    VdSharedHeader *header()
    {
        return reinterpret_cast<VdSharedHeader *>(bytes.data());
    }

    void produce_video(std::uint32_t slot, std::uint8_t fill)
    {
        volatile std::uint32_t *seq = &header()->video_slot_seq[slot];
        const std::uint32_t before = __atomic_load_n(seq, __ATOMIC_RELAXED);
        __atomic_store_n(seq, before + 1u, __ATOMIC_RELEASE);
        std::memset(VD_SHM_VIDEO_PTR(bytes.data(), slot), fill, VD_VIDEO_MAX_FRAME);
        __atomic_store_n(seq, before + 2u, __ATOMIC_RELEASE);
        header()->video_producer = (slot + 1u) % VD_VIDEO_SLOTS;
    }

    void produce_audio(std::uint32_t slot, std::uint8_t fill)
    {
        volatile std::uint32_t *seq = &header()->audio_slot_seq[slot];
        const std::uint32_t before = __atomic_load_n(seq, __ATOMIC_RELAXED);
        __atomic_store_n(seq, before + 1u, __ATOMIC_RELEASE);
        std::memset(VD_SHM_AUDIO_PTR(bytes.data(), slot), fill, VD_AUDIO_CHUNK_BYTES);
        __atomic_store_n(seq, before + 2u, __ATOMIC_RELEASE);
        header()->audio_producer = (slot + 1u) % VD_AUDIO_SLOTS;
    }
};

// ---- NV12 ----------------------------------------------------------------

void test_nv12_basic()
{
    // Y = 128 with neutral chroma is mid grey (130, 130, 130).
    const std::uint8_t frame[6] = {128, 128, 128, 128, 128, 128};
    std::uint8_t rgba[4] = {};
    vd5::nv12_pixel(frame, frame + 4, 2, 0, 0, rgba);
    CHECK(rgba[0] == 130 && rgba[1] == 130 && rgba[2] == 130 && rgba[3] == 255);

    // Y = 16 is black, Y = 235 is white (both with neutral chroma).
    std::uint8_t black[4] = {};
    vd5::nv12_pixel(frame, frame + 4, 2, 1, 0, black); // same chroma block
    CHECK(black[0] == 130); // Y is per pixel; both samples are 128 here

    const std::uint8_t ramp[6] = {16, 235, 16, 16, 128, 128};
    std::uint8_t out[4] = {};
    vd5::nv12_pixel(ramp, ramp + 4, 2, 0, 0, out);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0);
    vd5::nv12_pixel(ramp, ramp + 4, 2, 1, 0, out);
    CHECK(out[0] == 255 && out[1] == 255 && out[2] == 255);
}

void test_nv12_chroma()
{
    // Full-scale blue chroma: U = 255, V = 128, Y = 128.
    const std::uint8_t frame[6] = {128, 128, 128, 128, 255, 128};
    std::uint8_t rgba[4] = {};
    vd5::nv12_pixel(frame, frame + 4, 2, 0, 0, rgba);
    CHECK(rgba[2] == 255);   // B pinned high
    CHECK(rgba[0] == 130);   // R follows Y only
    CHECK(rgba[1] == 81);    // G = (298*112 - 100*127 + 128) >> 8
}

void test_nv12_plane_layout()
{
    // 4x2: Y plane is 8 bytes, UV plane 4 bytes; the second 2x2 block must
    // use its own UV pair, and the bottom row must use the same UV row.
    constexpr int kW = 4, kH = 2;
    std::uint8_t frame[kW * kH + kW * kH / 2];
    for (int i = 0; i < kW * kH; ++i)
        frame[i] = (std::uint8_t)(16 + i * 8);
    // UV row: block 0 = (255, 128) blue, block 1 = (128, 255) red.
    frame[kW * kH + 0] = 255;
    frame[kW * kH + 1] = 128;
    frame[kW * kH + 2] = 128;
    frame[kW * kH + 3] = 255;

    std::uint8_t rgba[kW * kH * 4];
    vd5::nv12_to_rgba(frame, kW, kH, rgba);

    std::uint8_t expect[4] = {};
    vd5::nv12_pixel(frame, frame + kW * kH, kW, 0, 1, expect);
    CHECK(std::memcmp(rgba + kW * 1 * 4 + 0 * 4, expect, 4) == 0); // (0,1)
    vd5::nv12_pixel(frame, frame + kW * kH, kW, 3, 0, expect);
    CHECK(std::memcmp(rgba + 3 * 4, expect, 4) == 0); // (3,0)
    // The two blocks differ in chroma.
    CHECK(rgba[2] == 255);         // (0,0) blue-ish
    CHECK(rgba[3 * 4 + 0] > 150);  // (3,0) red-ish
}

// ---- the SPSC seq protocol ------------------------------------------------

void test_video_reader()
{
    FakeRegion region;
    vd5::Shm shm;
    CHECK(shm.attach_memory(region.bytes.data(), region.bytes.size()));
    CHECK(shm.contract_ok());

    std::vector<std::uint8_t> copy(VD_VIDEO_MAX_FRAME);
    std::uint32_t width = 0, height = 0;
    CHECK(!shm.read_video(copy.data(), copy.size(), &width, &height)); // nothing yet

    region.produce_video(0, 0x11);
    CHECK(shm.read_video(copy.data(), copy.size(), &width, &height));
    CHECK(width == 960 && height == 544);
    CHECK(copy[0] == 0x11 && copy[VD_VIDEO_MAX_FRAME - 1] == 0x11);
    CHECK(!shm.read_video(copy.data(), copy.size(), &width, &height)); // same frame again

    // Both slots filled: the newest (slot before producer) wins.
    region.produce_video(1, 0x22);
    region.produce_video(0, 0x33);
    CHECK(shm.read_video(copy.data(), copy.size(), &width, &height));
    CHECK(copy[0] == 0x33);

    // A slot mid-write (odd seq) is never returned.
    volatile std::uint32_t *seq = &region.header()->video_slot_seq[1];
    __atomic_store_n(seq, __atomic_load_n(seq, __ATOMIC_RELAXED) + 1u, __ATOMIC_RELEASE);
    region.header()->video_producer = 0; // newest "slot" is 1, mid-write
    CHECK(!shm.read_video(copy.data(), copy.size(), &width, &height));
    CHECK(shm.stats().video_torn == 0); // skipped, not torn

    // Too-small buffers are refused.
    CHECK(!shm.read_video(copy.data(), 16, &width, &height));
}

void test_audio_reader_sequential()
{
    FakeRegion region;
    vd5::Shm shm;
    CHECK(shm.attach_memory(region.bytes.data(), region.bytes.size()));
    std::uint8_t chunk[VD_AUDIO_CHUNK_BYTES];

    region.produce_audio(0, 0x41);
    region.produce_audio(1, 0x42);
    region.produce_audio(2, 0x43);
    CHECK(shm.read_audio(chunk, sizeof(chunk)));
    CHECK(chunk[0] == 0x41); // strictly sequential, oldest first
    CHECK(shm.read_audio(chunk, sizeof(chunk)));
    CHECK(chunk[0] == 0x42);
    CHECK(shm.read_audio(chunk, sizeof(chunk)));
    CHECK(chunk[0] == 0x43);
    CHECK(!shm.read_audio(chunk, sizeof(chunk)));

    region.produce_audio(3, 0x44);
    CHECK(shm.read_audio(chunk, sizeof(chunk)));
    CHECK(chunk[0] == 0x44);
}

void test_pad_publish()
{
    FakeRegion region;
    vd5::Shm shm;
    CHECK(shm.attach_memory(region.bytes.data(), region.bytes.size()));

    VdPadReport report{};
    vd5::fill_report(&report, 7, 123456789ull, vd5::scepad::kCross | vd5::scepad::kR1,
                     255, 0, 128, 128, 200, 0);
    CHECK(shm.publish_pad(report));

    const std::uint8_t *slot = VD_SHM_PAD_PTR(region.bytes.data(), 0);
    VdPadReport back{};
    CHECK(vd_pad_deserialize(slot, VD_PAD_WIRE_BYTES, &back) == 0);
    CHECK(back.report_id == 7);
    CHECK(back.timestamp_us == 123456789ull);
    CHECK(back.buttons == (VD_PAD_CROSS | VD_PAD_R1));
    CHECK(back.left_x == 32639 && back.left_y == -32768); // clamped full range
    CHECK(back.right_x == 0 && back.right_y == 0);
    CHECK(back.l2 == 200 && back.r2 == 0);
    // Slot zero padding beyond the wire format.
    for (std::size_t i = VD_PAD_WIRE_BYTES; i < VD_PAD_REPORT_BYTES; ++i)
        CHECK(slot[i] == 0);
    // Released: the slot's seq is even and the producer index advanced.
    CHECK((region.header()->pad_slot_seq[0] & 1u) == 0u);
    CHECK(region.header()->pad_producer == 1u);
    CHECK(shm.stats().pad_reports == 1u);
}

// ---- the scePad -> Vita button table -------------------------------------

void test_button_mapping()
{
    using namespace vd5;
    CHECK(map_pad_buttons(scepad::kCross) == VD_PAD_CROSS);
    CHECK(map_pad_buttons(scepad::kOptions) == VD_PAD_START);
    CHECK(map_pad_buttons(scepad::kCreate) == VD_PAD_SELECT);
    CHECK(map_pad_buttons(scepad::kTouchpad) == VD_PAD_TOUCH);
    CHECK(map_pad_buttons(scepad::kTriangle | scepad::kL2 | scepad::kUp) ==
          (VD_PAD_TRIANGLE | VD_PAD_L2 | VD_PAD_UP));
    // Intercepted input is not forwarded.
    CHECK(map_pad_buttons(scepad::kCross | scepad::kIntercepted) == 0u);
    CHECK(map_pad_buttons(0) == 0u);
}

void test_axis_mapping()
{
    CHECK(vd5::map_axis(128) == 0);
    CHECK(vd5::map_axis(255) == 32639);
    CHECK(vd5::map_axis(0) == -32768);
    CHECK(vd5::map_axis(129) == 257);
}

// ---- the seq protocol under a racing producer ----------------------------

void test_video_reader_races()
{
    FakeRegion region;
    vd5::Shm shm;
    CHECK(shm.attach_memory(region.bytes.data(), region.bytes.size()));

    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::atomic<int> delivered{0};
    std::thread producer([&] {
        std::uint32_t generation = 0;
        while (!stop.load(std::memory_order_relaxed))
        {
            const std::uint32_t slot = generation % VD_VIDEO_SLOTS;
            volatile std::uint32_t *seq = &region.header()->video_slot_seq[slot];
            const std::uint32_t before = __atomic_load_n(seq, __ATOMIC_RELAXED);
            __atomic_store_n(seq, before + 1u, __ATOMIC_RELEASE);
            const std::uint8_t fill = (std::uint8_t)(generation & 0xff);
            std::memset(VD_SHM_VIDEO_PTR(region.bytes.data(), slot), fill, VD_VIDEO_MAX_FRAME);
            __atomic_store_n(seq, before + 2u, __ATOMIC_RELEASE);
            region.header()->video_producer = slot + 1u;
            ++generation;
        }
    });

    std::vector<std::uint8_t> copy(VD_VIDEO_MAX_FRAME);
    std::uint32_t width = 0, height = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (shm.read_video(copy.data(), copy.size(), &width, &height))
        {
            // Every delivered frame must be one solid generation: a torn
            // copy (mixed fills) must never be returned to the app.
            const std::uint8_t fill = copy[0];
            for (std::size_t i = 1; i < copy.size(); i += 997)
            {
                if (copy[i] != fill)
                {
                    bad.fetch_add(1);
                    break;
                }
            }
            delivered.fetch_add(1);
        }
    }
    stop.store(true);
    producer.join();

    std::printf("race: %d frames delivered, %d torn frames escaped, %llu torn drops\n",
                delivered.load(), bad.load(),
                (unsigned long long)shm.stats().video_torn);
    CHECK(bad.load() == 0);
    CHECK(delivered.load() > 0);
}

void test_attach_refuses_missing_service()
{
    vd5::Shm shm;
    // On a host without the dock service the real attach must fail cleanly.
    // (On a console with /dev/vdshm present this may succeed; the check is
    // that it never traps and always leaves a usable state.)
    const bool ok = shm.attach();
    if (!ok)
        CHECK(shm.last_error()[0] != '\0');
    shm.detach();
    CHECK(!shm.attached());
}

} // namespace

void test_l3_chords()
{
    using namespace vd5;
    ChordEvents e = chord_events(scepad::kL3 | scepad::kR3, scepad::kL3);
    CHECK(e.toggle_capture && e.upscale_cycle == 0 && !e.toggle_fullscreen);
    e = chord_events(scepad::kL3 | scepad::kRight, scepad::kL3);
    CHECK(!e.toggle_capture && e.upscale_cycle == +1);
    e = chord_events(scepad::kL3 | scepad::kLeft, scepad::kL3);
    CHECK(e.upscale_cycle == -1);
    e = chord_events(scepad::kL3 | scepad::kUp, scepad::kL3);
    CHECK(e.toggle_fullscreen && e.upscale_cycle == 0);
    e = chord_events(scepad::kL3 | scepad::kDown, scepad::kL3);
    CHECK(e.toggle_touch_target && !e.press_home);
    e = chord_events(scepad::kL3 | scepad::kTouchpad, scepad::kL3);
    CHECK(e.press_home && !e.toggle_touch_target);
    // Held chords do not repeat (no rising edge).
    e = chord_events(scepad::kL3 | scepad::kRight, scepad::kL3 | scepad::kRight);
    CHECK(e.upscale_cycle == 0);
    // Without L3 nothing is a chord.
    e = chord_events(scepad::kRight, 0);
    CHECK(e.upscale_cycle == 0 && !e.toggle_fullscreen && !e.toggle_capture);
    // While L3 is held the chord buttons never reach the Vita.
    CHECK(mask_modifiers(scepad::kL3 | scepad::kR3 | scepad::kUp | scepad::kCross) ==
          scepad::kCross);
    CHECK(mask_modifiers(scepad::kL3 | scepad::kTouchpad | scepad::kCross) == scepad::kCross);
    CHECK(mask_modifiers(scepad::kCross | scepad::kDown) == (scepad::kCross | scepad::kDown));
}

void test_upscale_settings()
{
    vd5::VdSettings s;
    CHECK(s.upscale == vd5::UpscaleMode::off); // owner default: upscaling off
    const std::string text = vd5::encode_settings(s);
    vd5::VdSettings back;
    back.upscale = vd5::UpscaleMode::fsr2;
    CHECK(vd5::decode_settings(text, &back));
    CHECK(back.upscale == vd5::UpscaleMode::off);
    // Legacy 0/1/2 keep Off / Sharp 2x / FSR 2x.
    CHECK(vd5::decode_settings("resolution=1\nupscale=2\n", &back));
    CHECK(back.upscale == vd5::UpscaleMode::fsr2);
    CHECK(vd5::decode_settings("resolution=1\nupscale=1\n", &back));
    CHECK(back.upscale == vd5::UpscaleMode::sharp2);
    CHECK(vd5::decode_settings("resolution=1\nupscale=5\n", &back));
    CHECK(back.upscale == vd5::UpscaleMode::sharp4);
    CHECK(vd5::decode_settings("resolution=1\nupscale=9\n", &back));
    CHECK(back.upscale == vd5::UpscaleMode::off); // out of range
    CHECK(std::string(vd5::vd_settings_upscale_label(vd5::UpscaleMode::sharp2)) == "Sharp 2x");
    CHECK(std::string(vd5::vd_settings_upscale_label(vd5::UpscaleMode::fsr4)) == "FSR 4x");
    CHECK(vd5::vd_settings_upscale_factor(vd5::UpscaleMode::fsr3) == 3);
    CHECK(vd5::vd_settings_upscale_is_fsr(vd5::UpscaleMode::fsr2));
    CHECK(!vd5::vd_settings_upscale_is_fsr(vd5::UpscaleMode::sharp3));
    CHECK(vd5::vd_settings_cycle_upscale(vd5::UpscaleMode::fsr4, +1) == vd5::UpscaleMode::off);
    CHECK(vd5::vd_settings_cycle_upscale(vd5::UpscaleMode::off, -1) == vd5::UpscaleMode::fsr4);
}

int main()
{
    test_nv12_basic();
    test_nv12_chroma();
    test_nv12_plane_layout();
    test_video_reader();
    test_audio_reader_sequential();
    test_pad_publish();
    test_button_mapping();
    test_axis_mapping();
    test_video_reader_races();
    test_attach_refuses_missing_service();
    test_l3_chords();
    test_upscale_settings();

    if (g_failures == 0)
    {
        std::printf("vd_tests: all checks passed\n");
        return 0;
    }
    std::printf("vd_tests: %d checks FAILED\n", g_failures);
    return 1;
}

#else  // !VD5_HOST_TESTS

// The PS5 build compiles every source under app/; the tests exist only for
// the host build (app/Makefile `test`).
namespace vd5::tests
{
int host_tests_not_built_in_this_target()
{
    return 0;
}
} // namespace vd5::tests

#endif // VD5_HOST_TESTS
